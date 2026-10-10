/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Session executor against a simulated receiver: detection, identity, the
 * apply session (signal-group restart, baud change, base mode, outputs,
 * save), retries, rollback and restart evidence.
 *
 * The simulated receiver is SYNTHETIC: it follows the documented command
 * set and reply formats (acknowledgement with the '$'-inclusive XOR, version
 * and base-mode logs, configuration dump, restart banner at the saved baud),
 * not captured behaviour. Its timings (30 ms replies, 3 s restarts) are made
 * up for the test.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "um98x_base.h"
#include "um98x_exec.h"
#include "um98x_profile.h"
#include "um98x_test_vectors.h"
#include "unity.h"

#define T_START   ((int64_t)UINT32_MAX + 1000)
#define MAX_EV    256
#define MAX_RX    160
#define LINE_MAX_ 200

typedef struct {
    int64_t at;
    uint32_t baud;   /* receiver baud when it sent the line */
    char line[LINE_MAX_];
} sim_ev_t;

typedef struct {
    char cmd[100];
    int64_t at;
    uint32_t host_baud;
    int64_t gap;     /* since the last line delivered to the host, INT64_MAX = none */
} sim_rx_t;

typedef struct {
    int64_t now;
    uint32_t host_baud;
    um98x_chan_t ch;
    um98x_port_t port;
    um98x_exec_t ex;

    /* receiver */
    bool present;
    uint32_t rx_baud, saved_baud;
    uint8_t sg_m, sg_s;
    char mode[100], saved_mode[100];
    bool output, saved_output;
    bool r1230;
    const char *version;
    int64_t busy_until;
    int64_t restart_ms;
    bool banner;
    bool in_loader;
    uint32_t loader_baud;
    bool ignore_baud_cmd;
    bool dies_on_restart;
    bool sg_sticky;
    bool sg_no_ack;      /* a signal-group change restarts the receiver before its ack goes out */
    uint32_t deaf_baud;  /* the next deaf_n commands received at this baud are lost */
    int deaf_n;
    char drop_prefix[40];
    int drop_n;
    char reject_prefix[40];
    int reject_n;
    int saves;
    char b1c, saved_b1c;  /* B1C/B2a in RTCM: 'E' enabled, 'D' disabled, 0 = not in the dump */
    bool b1c_restarts;    /* turning B1C/B2a on restarts the receiver */
    char restart_after[40];   /* the next command with this prefix is acknowledged, then the receiver restarts */
    char ant[120], saved_ant[120];   /* antenna descriptor values ("" = not in the dump) */
    unsigned gsv;         /* satellite-in-view output period, 0 = off */

    /* records */
    sim_rx_t rx[MAX_RX];
    int nrx;
    sim_ev_t q[MAX_EV];
    int nq;
    int64_t last_line_at;
    uint64_t rtcm, lines, bytes;
    long waits;
    unsigned events;
} sim_t;

static sim_t S;

/* ---- receiver ----------------------------------------------------------- */

static void emit(sim_t *s, const char *line, int64_t delay)
{
    TEST_ASSERT_LESS_THAN_INT(MAX_EV, s->nq);
    sim_ev_t *e = &s->q[s->nq++];
    e->at = s->now + delay;
    e->baud = s->rx_baud;
    snprintf(e->line, sizeof(e->line), "%s", line);
}

static void emit_checked(sim_t *s, char lead, const char *content, int64_t delay)
{
    char tmp[LINE_MAX_];
    char line[LINE_MAX_];
    snprintf(tmp, sizeof(tmp), "%c%s", lead, content);
    snprintf(line, sizeof(line), "%s*%02X", tmp, um98x_xor8(tmp, strlen(tmp)));
    emit(s, line, delay);
}

static void reply(sim_t *s, const char *cmd, const char *resp, int64_t delay)
{
    char content[LINE_MAX_];
    snprintf(content, sizeof(content), "command,%s,response: %s", cmd, resp);
    emit_checked(s, '$', content, delay);
}

static void restart(sim_t *s, int64_t dur)
{
    s->busy_until = s->now + dur;
    s->rx_baud = s->saved_baud;
    strcpy(s->mode, s->saved_mode);
    s->output = s->saved_output;
    s->b1c = s->saved_b1c;
    strcpy(s->ant, s->saved_ant);
    s->gsv = 0;   /* never saved */
    if (s->dies_on_restart) {
        s->present = false;
        return;
    }
    if (s->banner) {
        emit(s, TV_BANNER, dur);
    }
}

static bool starts(const char *s, const char *prefix)
{
    return prefix[0] != '\0' && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void receive(sim_t *s, const char *cmd)
{
    TEST_ASSERT_LESS_THAN_INT(MAX_RX, s->nrx);
    sim_rx_t *r = &s->rx[s->nrx++];
    snprintf(r->cmd, sizeof(r->cmd), "%s", cmd);
    r->at = s->now;
    r->host_baud = s->host_baud;
    r->gap = s->last_line_at == INT64_MIN ? INT64_MAX : s->now - s->last_line_at;

    if (!s->present || s->now < s->busy_until) {
        return;
    }
    if (s->host_baud != (s->in_loader ? s->loader_baud : s->rx_baud)) {
        return;   /* garbled at the wrong baud */
    }
    if (s->in_loader) {
        if (strcmp(cmd, "6") == 0) {
            s->in_loader = false;
            restart(s, 1000);
        }
        return;
    }
    char up[100];
    size_t i;
    for (i = 0; cmd[i] != '\0' && i < sizeof(up) - 1; i++) {
        up[i] = (char)toupper((unsigned char)cmd[i]);
    }
    up[i] = '\0';
    if (s->drop_n > 0 && starts(up, s->drop_prefix)) {
        s->drop_n--;
        return;
    }
    if (s->deaf_n > 0 && s->rx_baud == s->deaf_baud) {
        s->deaf_n--;
        return;
    }
    if (s->reject_n > 0 && starts(up, s->reject_prefix)) {
        s->reject_n--;
        reply(s, cmd, "PARSING FAILD NO MATCHING FUNC", 30);
        return;
    }
    unsigned a, b;
    if (strcmp(up, "VERSIONA") == 0) {
        reply(s, cmd, "OK", 30);
        emit(s, s->version, 50);
    } else if (strcmp(up, "UNLOG COM1") == 0) {
        s->output = false;
        reply(s, cmd, "OK", 30);
    } else if (strcmp(up, "CONFIG") == 0) {
        char item[LINE_MAX_];
        reply(s, cmd, "OK", 30);
        snprintf(item, sizeof(item), "CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP %u %u", s->sg_m, s->sg_s);
        emit_checked(s, '$', item, 60);
        snprintf(item, sizeof(item), "CONFIG,COM1,CONFIG COM1 %u", (unsigned)s->rx_baud);
        emit_checked(s, '$', item, 90);
        emit_checked(s, '$', "CONFIG,MMP,CONFIG MMP ENABLE", 120);
        if (s->b1c != 0) {
            snprintf(item, sizeof(item), "CONFIG,RTCMB1CB2A,CONFIG RTCMB1CB2A %s", s->b1c == 'E' ? "ENABLE" : "DISABLE");
            emit_checked(s, '$', item, 150);
        }
        emit_checked(s, '$', "CONFIG,MASK,CONFIG MASK 5.0", 160);
        if (s->ant[0] != '\0') {
            snprintf(item, sizeof(item), "CONFIG,BASEANTENNAMODEL,CONFIG BASEANTENNAMODEL %s", s->ant);
            emit_checked(s, '$', item, 170);
        }
    } else if (strcmp(up, "MODE") == 0) {
        char log[LINE_MAX_];
        reply(s, cmd, "OK", 30);
        snprintf(log, sizeof(log), "MODEA,COM1,0,0.0,FINE,0,0.000,0,0,0;%s", s->mode);
        emit_checked(s, '#', log, 50);
    } else if (sscanf(up, "CONFIG SIGNALGROUP %u %u", &a, &b) == 2) {
        bool change = a != s->sg_m || b != s->sg_s;
        if (!(change && s->sg_no_ack)) {
            reply(s, cmd, "OK", 30);
        }
        if (change) {
            if (!s->sg_sticky) {
                s->sg_m = (uint8_t)a;   /* saves itself */
                s->sg_s = (uint8_t)b;
            }
            restart(s, s->restart_ms);
        }
    } else if (sscanf(up, "CONFIG COM1 %u", &a) == 1) {
        if (!s->ignore_baud_cmd) {
            reply(s, cmd, "OK", 30);   /* still at the old baud */
            s->rx_baud = a;
        }
    } else if (strncmp(up, "MODE BASE ", 10) == 0) {
        snprintf(s->mode, sizeof(s->mode), "%s", up);
        reply(s, cmd, "OK", 30);
    } else if (sscanf(up, "RTCM%u COM1 %u", &a, &b) == 2) {
        if (a == 1230 && !s->r1230) {
            reply(s, cmd, "PARSING FAILD NO MATCHING FUNC  RTCM1230", 30);
        } else {
            s->output = true;
            reply(s, cmd, "OK", 30);
        }
    } else if (sscanf(up, "GPGSV COM1 %u", &a) == 1) {
        s->gsv = a;
        reply(s, cmd, "OK", 30);
    } else if (strcmp(up, "UNLOG COM1 GPGSV") == 0) {
        s->gsv = 0;
        reply(s, cmd, "OK", 30);
    } else if (strcmp(up, "CONFIG RTCMB1CB2A ENABLE") == 0) {
        s->b1c = 'E';
        if (s->b1c_restarts) {
            s->saved_b1c = 'E';   /* saves itself, like a signal-group change */
            restart(s, s->restart_ms);
        } else {
            reply(s, cmd, "OK", 30);
        }
    } else if (strncmp(up, "CONFIG BASEANTENNAMODEL ", 24) == 0) {
        snprintf(s->ant, sizeof(s->ant), "%s", cmd + 24);
        reply(s, cmd, "OK", 30);
    } else if (strcmp(up, "SAVECONFIG") == 0) {
        s->saved_baud = s->rx_baud;
        strcpy(s->saved_mode, s->mode);
        s->saved_output = s->output;
        s->saved_b1c = s->b1c;
        strcpy(s->saved_ant, s->ant);
        s->saves++;
        reply(s, cmd, "OK", 300);
    } else if (strcmp(up, "RESET") == 0) {
        reply(s, cmd, "OK", 30);
        restart(s, s->restart_ms);
    } else {
        reply(s, cmd, "PARSING FAILD NO MATCHING FUNC", 30);
    }
    if (starts(up, s->restart_after)) {
        s->restart_after[0] = '\0';
        restart(s, s->restart_ms);   /* "OK, then a receiver reset" */
    }
}

/* ---- port --------------------------------------------------------------- */

static void advance(sim_t *s, int64_t t)
{
    if (t <= s->now) {
        return;
    }
    if (s->present && s->output && !s->in_loader) {
        int64_t from = s->now > s->busy_until ? s->now : s->busy_until;
        if (t > from) {
            int64_t frames = t / 100 - from / 100;   /* 10 RTCM frames per second */
            s->bytes += (uint64_t)frames * 120u;
            if (s->host_baud == s->rx_baud) {
                s->rtcm += (uint64_t)frames;
            }
        }
    }
    s->now = t;
}

static void p_lock(void *c)
{
    (void)c;
}

static void p_unlock(void *c)
{
    (void)c;
}

static int64_t p_now(void *c)
{
    return ((sim_t *)c)->now;
}

static bool p_write(void *c, const char *data, size_t len)
{
    sim_t *s = c;
    advance(s, s->now + 1);
    char buf[256];
    size_t n = 0;
    for (size_t i = 0; i <= len; i++) {
        char ch = i < len ? data[i] : '\n';
        if (ch == '\r' || ch == '\n') {
            if (n > 0) {
                buf[n] = '\0';
                receive(s, buf);
                n = 0;
            }
        } else if (n < sizeof(buf) - 1) {
            buf[n++] = ch;
        }
    }
    return true;
}

static bool p_set_baud(void *c, uint32_t baud)
{
    sim_t *s = c;
    s->host_baud = baud;
    advance(s, s->now + 20);
    return true;
}

static uint32_t p_get_baud(void *c)
{
    return ((sim_t *)c)->host_baud;
}

static void p_wait(void *c, int64_t until)
{
    sim_t *s = c;
    s->waits++;
    TEST_ASSERT_LESS_THAN_INT32_MESSAGE(1000000, s->waits, "simulation does not end");
    if (until <= s->now) {
        until = s->now + 1;
    }
    for (;;) {
        int idx = -1;
        for (int i = 0; i < s->nq; i++) {
            if (idx < 0 || s->q[i].at < s->q[idx].at) {
                idx = i;
            }
        }
        if (idx < 0 || s->q[idx].at > until) {
            advance(s, until);
            return;
        }
        sim_ev_t e = s->q[idx];
        memmove(&s->q[idx], &s->q[idx + 1], sizeof(s->q[0]) * (size_t)(s->nq - idx - 1));
        s->nq--;
        advance(s, e.at);
        s->bytes += strlen(e.line) + 2;
        if (e.baud != s->host_baud) {
            continue;   /* arrives as garbage */
        }
        s->lines++;
        s->last_line_at = s->now;
        um98x_line_t l;
        um98x_line_classify(e.line, strlen(e.line), &l);
        unsigned ev = um98x_chan_dispatch(&s->ch, e.line, strlen(e.line), &l, s->now);
        s->events |= ev;
        if (ev & UM98X_CHAN_WAKE) {
            return;
        }
    }
}

static void p_counts(void *c, um98x_link_counts_t *out)
{
    sim_t *s = c;
    out->rtcm_frames = s->rtcm;
    out->text_lines = s->lines;
    out->rx_bytes = s->bytes;
}

static bool p_prompt(void *c)
{
    sim_t *s = c;
    return s->in_loader && s->host_baud == s->loader_baud;
}

static sim_t *sim_init(void)
{
    sim_t *s = &S;
    memset(s, 0, sizeof(*s));
    s->now = T_START;
    s->host_baud = 115200;
    s->present = true;
    s->rx_baud = s->saved_baud = 115200;
    s->sg_m = 4;   /* the receiver's factory signal group */
    s->sg_s = 5;
    strcpy(s->mode, "MODE ROVER");
    strcpy(s->saved_mode, s->mode);
    s->output = s->saved_output = true;
    s->r1230 = true;
    s->b1c = s->saved_b1c = 'E';   /* the factory value per public dumps */
    s->version = TV_VERSIONA_UM982;
    s->restart_ms = 3000;
    s->banner = true;
    s->last_line_at = INT64_MIN;
    um98x_chan_init(&s->ch);
    s->port = (um98x_port_t){
        .ctx = s,
        .lock = p_lock,
        .unlock = p_unlock,
        .now_ms = p_now,
        .write = p_write,
        .set_baud = p_set_baud,
        .get_baud = p_get_baud,
        .wait = p_wait,
        .counts = p_counts,
        .prompt_seen = p_prompt,
    };
    s->ex = (um98x_exec_t){ .ch = &s->ch, .port = &s->port };
    return s;
}

/* Receiver already configured with the default profile. */
static void configured(sim_t *s)
{
    s->sg_m = 3;
    s->sg_s = 6;
    strcpy(s->mode, "MODE BASE 1234 TIME 600 0");
    strcpy(s->saved_mode, s->mode);
}

static int count(const sim_t *s, const char *prefix)
{
    int n = 0;
    for (int i = 0; i < s->nrx; i++) {
        if (strncmp(s->rx[i].cmd, prefix, strlen(prefix)) == 0) {
            n++;
        }
    }
    return n;
}

/* Index of the n-th (0-based) command equal to cmd, -1 if none. */
static int nth(const sim_t *s, const char *cmd, int n)
{
    for (int i = 0; i < s->nrx; i++) {
        if (strcmp(s->rx[i].cmd, cmd) == 0 && n-- == 0) {
            return i;
        }
    }
    return -1;
}

static void default_profile(um98x_base_profile_t *p)
{
    um98x_profile_default(p, 1234);
}

static void known_identity(gnss_versiona_t *v)
{
    TEST_ASSERT_TRUE(gnss_versiona_parse(TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), v));
}

/* ---- tests -------------------------------------------------------------- */

TEST_CASE("exec: version query reads the identity", "[um98x_exec]")
{
    sim_t *s = sim_init();
    gnss_versiona_t v;
    bool ack = false;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_version(&s->ex, false, 0, &v, &ack));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_UINT32(18975, v.build);
    TEST_ASSERT_TRUE(ack);
    TEST_ASSERT_EQUAL_INT(1, count(s, "VERSIONA"));

    /* Nobody home: three tries, "no answer". */
    s = sim_init();
    s->present = false;
    int64_t t0 = s->now;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_version(&s->ex, false, 0, &v, &ack));
    TEST_ASSERT_FALSE(ack);
    TEST_ASSERT_EQUAL_INT(3, count(s, "VERSIONA"));
    TEST_ASSERT_INT64_WITHIN(20, t0 + 8000, s->now);
}

TEST_CASE("exec: apply on a factory receiver: signal group first, restart, then the rest", "[um98x_exec]")
{
    sim_t *s = sim_init();
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    memset(&id, 0, sizeof(id));   /* identity unknown: queried in the session */
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("UM982", id.model);
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_TRUE(r.mode_sent);
    TEST_ASSERT_TRUE(r.rtcm1230_ok);
    TEST_ASSERT_FALSE(r.baud_changed);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_UINT32(0, r.found_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.baud);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", r.firmware);
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);

    /* Order: identity, quiet port, read-back, signal group and its restart,
     * quiet port and read-back again, base mode, outputs, one save. */
    int sg = nth(s, "CONFIG SIGNALGROUP 3 6", 0);
    TEST_ASSERT_EQUAL_INT(0, nth(s, "VERSIONA", 0));
    TEST_ASSERT_EQUAL_INT(1, nth(s, "UNLOG COM1", 0));
    TEST_ASSERT_EQUAL_INT(2, nth(s, "CONFIG", 0));
    TEST_ASSERT_EQUAL_INT(3, nth(s, "MODE", 0));
    TEST_ASSERT_EQUAL_INT(4, sg);
    TEST_ASSERT_GREATER_THAN_INT(sg, nth(s, "UNLOG COM1", 1));
    TEST_ASSERT_GREATER_THAN_INT(nth(s, "UNLOG COM1", 1), nth(s, "CONFIG", 1));
    TEST_ASSERT_GREATER_THAN_INT(nth(s, "CONFIG", 1), nth(s, "MODE", 1));
    TEST_ASSERT_GREATER_THAN_INT(nth(s, "MODE", 1), nth(s, "MODE BASE 1234 TIME 600 0", 0));
    TEST_ASSERT_GREATER_THAN_INT(nth(s, "MODE BASE 1234 TIME 600 0", 0), nth(s, "RTCM1005 COM1 1", 0));
    TEST_ASSERT_EQUAL_INT(s->nrx - 1, nth(s, "SAVECONFIG", 0));
    TEST_ASSERT_EQUAL_INT(1, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG COM1"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    TEST_ASSERT_EQUAL_INT(8, count(s, "RTCM"));
    for (int i = 0; i < s->nrx; i++) {
        TEST_ASSERT_NULL(strstr(s->rx[i].cmd, "COM2"));
        TEST_ASSERT_NULL(strstr(s->rx[i].cmd, "COM3"));
        /* 100 ms after the receiver's last line (version polls during the
         * restart follow their own clock). */
        if (strcmp(s->rx[i].cmd, "VERSIONA") != 0) {
            TEST_ASSERT_GREATER_OR_EQUAL_INT64(100, s->rx[i].gap);
        }
    }

    /* The receiver now runs the profile. */
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
    TEST_ASSERT_EQUAL_UINT8(6, s->sg_s);
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0", s->saved_mode);
    TEST_ASSERT_TRUE(s->saved_output);
    TEST_ASSERT_EQUAL_INT(1, s->saves);
    /* Our own restart is not reported as a receiver restart. */
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.banners);
    TEST_ASSERT_FALSE(s->ch.expect_restart);
}

TEST_CASE("exec: a receiver that runs the profile gets no base mode and no signal group", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    snprintf(id.firmware, sizeof(id.firmware), "R4.10Build13495");   /* known from before an update */
    id.build = 13495;
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.signal_group_sent);
    TEST_ASSERT_FALSE(r.mode_sent);
    /* The known identity is confirmed on the quiet port; the applied state
     * gets the firmware of this session. */
    TEST_ASSERT_EQUAL_INT(1, count(s, "VERSIONA"));
    TEST_ASSERT_EQUAL_INT(nth(s, "UNLOG COM1", 0) + 1, nth(s, "VERSIONA", 0));
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", r.firmware);
    TEST_ASSERT_EQUAL_UINT32(18975, id.build);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "MODE BASE"));
    TEST_ASSERT_EQUAL_INT(1, count(s, "SAVECONFIG"));

    /* A lock or re-survey forces the base mode. */
    s = sim_init();
    configured(s);
    um98x_apply_opts_t force = { .force_mode = true };
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, &force, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.mode_sent);
    TEST_ASSERT_EQUAL_INT(1, count(s, "MODE BASE 1234 TIME 600 0"));
    TEST_ASSERT_GREATER_THAN_INT64(0, r.mode_ack_ms);

    /* Fixed profile: sent as ECEF, and not again once the receiver runs it. */
    s = sim_init();
    configured(s);
    p.base_mode = UM98X_BASE_FIXED;
    p.fixed_lat_deg = 43.6532;
    p.fixed_lon_deg = -79.3832;
    p.fixed_height_m = 112.5;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.mode_sent);
    char want[UM98X_MODE_CMD_MAX];
    um98x_mode_command(&p, want, sizeof(want));
    TEST_ASSERT_EQUAL_STRING(want, s->saved_mode);
    s->nrx = 0;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.mode_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "MODE BASE"));
}

TEST_CASE("exec: a receiver that rejects 1230 is configured without it", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->r1230 = false;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_FALSE(r.rtcm1230_ok);
    TEST_ASSERT_EQUAL_INT(2, count(s, "RTCM1230"));   /* one resend after the first refusal */
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
}

TEST_CASE("exec: a fatal rejection stops the session and rolls back with RESET", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1077");
    s->reject_n = 10;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("RTCM1077 COM1 1", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", r.reason);
    TEST_ASSERT_FALSE(r.saved);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_TRUE(r.rollback_answered);
    TEST_ASSERT_EQUAL_INT(2, count(s, "RTCM1077"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RTCM1087"));   /* stopped there */
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_INT(1, count(s, "RESET"));
    TEST_ASSERT_EQUAL_INT(0, s->saves);
    /* RESET brought back the saved configuration (outputs on again). */
    TEST_ASSERT_TRUE(s->output);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
}

TEST_CASE("exec: lost commands are resent until acknowledged", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    snprintf(s->drop_prefix, sizeof(s->drop_prefix), "RTCM1087");
    s->drop_n = 2;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(3, count(s, "RTCM1087"));
    int a = nth(s, "RTCM1087 COM1 1", 0);
    int b = nth(s, "RTCM1087 COM1 1", 1);
    int c = nth(s, "RTCM1087 COM1 1", 2);
    TEST_ASSERT_INT64_WITHIN(5, 4000, s->rx[b].at - s->rx[a].at);   /* 3000 ms timeout + 1000 ms */
    TEST_ASSERT_INT64_WITHIN(5, 4000, s->rx[c].at - s->rx[b].at);

    /* Never answered: fatal after 4 sends. */
    s = sim_init();
    configured(s);
    snprintf(s->drop_prefix, sizeof(s->drop_prefix), "RTCM1087");
    s->drop_n = 100;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(4, count(s, "RTCM1087"));
    TEST_ASSERT_EQUAL_STRING("timeout", r.reason);
    TEST_ASSERT_TRUE(r.rolled_back);
}

TEST_CASE("exec: the baud change is verified at the new baud and saved", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.baud_changed);
    TEST_ASSERT_EQUAL_UINT32(460800, r.baud);
    TEST_ASSERT_EQUAL_UINT32(460800, s->host_baud);
    TEST_ASSERT_EQUAL_UINT32(460800, s->saved_baud);
    int cfg = nth(s, "CONFIG COM1 460800", 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, cfg);
    TEST_ASSERT_EQUAL_UINT32(115200, s->rx[cfg].host_baud);
    TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[cfg + 1].cmd);   /* verified at the new baud */
    TEST_ASSERT_EQUAL_UINT32(460800, s->rx[cfg + 1].host_baud);
    TEST_ASSERT_GREATER_THAN_INT(cfg, nth(s, "RTCM1005 COM1 1", 0));
}

TEST_CASE("exec: a baud change that does not take is undone and never saved", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->ignore_baud_cmd = true;
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("CONFIG COM1 460800", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("receiver stays at the old baud", r.reason);
    TEST_ASSERT_FALSE(r.baud_changed);
    TEST_ASSERT_EQUAL_UINT32(115200, s->host_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.baud);
    /* Silent at the new baud, answering at the old one: it never switched,
     * so the command went out once more; it is told to go back only when
     * both bauds are silent. */
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG COM1 460800"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG COM1 115200"));
    int first = nth(s, "CONFIG COM1 460800", 0);
    int second = nth(s, "CONFIG COM1 460800", 1);
    TEST_ASSERT_EQUAL_UINT32(115200, s->rx[first].host_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, s->rx[second].host_baud);
    bool asked_old = false;
    for (int i = first + 1; i < second; i++) {
        if (strcmp(s->rx[i].cmd, "VERSIONA") == 0 && s->rx[i].host_baud == 115200) {
            asked_old = true;   /* the old baud was checked before the second send */
        }
    }
    TEST_ASSERT_TRUE(asked_old);
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_INT(0, s->saves);
    TEST_ASSERT_TRUE(r.rolled_back);
}

TEST_CASE("exec: a baud command lost once is sent again and the change goes through", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    snprintf(s->drop_prefix, sizeof(s->drop_prefix), "CONFIG COM1");
    s->drop_n = 1;   /* the first CONFIG COM1 460800 never arrives */
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.baud_changed);
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_UINT32(460800, s->saved_baud);
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG COM1 460800"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG COM1 115200"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
}

TEST_CASE("exec: silent at both bauds, the receiver is told to go back at the new one", "[um98x_exec]")
{
    /* It switches, then hears nothing at 460800 for a while. */
    sim_t *s = sim_init();
    configured(s);
    s->deaf_baud = 460800;
    s->deaf_n = 3;
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("no answer at the new baud", r.reason);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG COM1 460800"));   /* the old baud was silent too: no second send */
    int back = nth(s, "CONFIG COM1 115200", 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, back);
    TEST_ASSERT_EQUAL_UINT32(460800, s->rx[back].host_baud);
}

TEST_CASE("exec: an unsupported model is never configured", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->version = TV_VERSIONA_UM980;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    memset(&id, 0, sizeof(id));
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_UNSUPPORTED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(1, s->nrx);
    TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[0].cmd);
    TEST_ASSERT_NOT_NULL(strstr(r.reason, "UM980"));

    /* Known as a UM982, but a UM980 answers in the session: stopped and
     * rolled back before anything else is configured. */
    s = sim_init();
    s->version = TV_VERSIONA_UM980;
    known_identity(&id);
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("unsupported model UM980", r.reason);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));

    /* No identity at all: nothing configuring either. */
    s = sim_init();
    s->present = false;
    memset(&id, 0, sizeof(id));
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(3, count(s, "VERSIONA"));
    TEST_ASSERT_EQUAL_INT(s->nrx, count(s, "VERSIONA"));
    TEST_ASSERT_FALSE(r.rolled_back);

    /* Invalid profile. */
    p.com1_baud = 12345;
    TEST_ASSERT_EQUAL(UM98X_E_ARG, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
}

TEST_CASE("exec: after a signal-group restart at the saved baud, detection finds the receiver", "[um98x_exec]")
{
    sim_t *s = sim_init();
    /* Running at 460800 that was never saved: the restart lands at 115200. */
    s->host_baud = 460800;
    s->rx_baud = 460800;
    s->saved_baud = 115200;
    strcpy(s->saved_mode, "MODE BASE 1234 TIME 600 0");
    strcpy(s->mode, s->saved_mode);
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 460800, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_EQUAL_UINT32(115200, r.found_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.baud);
    TEST_ASSERT_FALSE(r.baud_changed);   /* the profile's baud is where it came back */
    TEST_ASSERT_FALSE(r.mode_sent);      /* its saved base mode already matches */
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
    TEST_ASSERT_EQUAL_INT(1, s->saves);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    /* Quiet port and read-back again after the restart, at the new baud. */
    int unlog2 = nth(s, "UNLOG COM1", 1);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, unlog2);
    TEST_ASSERT_EQUAL_UINT32(115200, s->rx[unlog2].host_baud);
}

TEST_CASE("exec: an unacknowledged signal group that lands at the saved baud is found in the session", "[um98x_exec]")
{
    sim_t *s = sim_init();
    /* COM1 at an unsaved 460800; the receiver resets before its ack goes
     * out and comes back at its saved 115200, where no resend reaches it. */
    s->host_baud = 460800;
    s->rx_baud = 460800;
    s->saved_baud = 115200;
    s->sg_no_ack = true;
    strcpy(s->saved_mode, "MODE BASE 1234 TIME 600 0");
    strcpy(s->mode, s->saved_mode);
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 460800, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_EQUAL_UINT32(115200, r.found_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.answer_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.baud);
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
    TEST_ASSERT_EQUAL_INT(4, count(s, "CONFIG SIGNALGROUP"));   /* the step's own resends, then no more */
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_FALSE(s->ch.expect_restart);

    /* A negative reply still fails the step at once. */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", r.reason);
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_TRUE(r.rolled_back);
}

TEST_CASE("exec: after a rollback the receiver's baud is where it answered last", "[um98x_exec]")
{
    /* CONFIG COM1 460800 takes effect, but the checks at 460800 go unheard
     * and so does CONFIG COM1 115200; detection finds it at 460800; the
     * rollback's RESET brings it back to its saved 115200, where it answers. */
    sim_t *s = sim_init();
    configured(s);
    s->deaf_baud = 460800;
    s->deaf_n = 3;
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_UINT32(460800, r.found_baud);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_TRUE(r.rollback_answered);
    TEST_ASSERT_EQUAL_UINT32(115200, s->rx_baud);
    TEST_ASSERT_EQUAL_UINT32(115200, r.answer_baud);   /* not the 460800 the detection saw */
    TEST_ASSERT_EQUAL_UINT32(115200, r.baud);

    /* A RESET nobody answers leaves the receiver's baud unknown. */
    s = sim_init();
    configured(s);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1077");
    s->reject_n = 10;
    s->dies_on_restart = true;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_FALSE(r.rollback_answered);
    TEST_ASSERT_EQUAL_UINT32(0, r.answer_baud);
}

TEST_CASE("exec: the recovery RESET is our own restart, sent once", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_reset(&s->ex));
    TEST_ASSERT_EQUAL_INT(1, count(s, "RESET"));
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.banners);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);   /* not reported as the receiver's own */
    TEST_ASSERT_FALSE(s->ch.expect_restart);
    TEST_ASSERT_EQUAL_UINT32(0, s->events & UM98X_CHAN_RESTART);

    /* No answer: never resent. */
    s = sim_init();
    s->dies_on_restart = true;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_reset(&s->ex));
    TEST_ASSERT_EQUAL_INT(1, count(s, "RESET"));
    TEST_ASSERT_FALSE(s->ch.expect_restart);
}

TEST_CASE("exec: a receiver lost after its signal-group restart gets nothing more", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->dies_on_restart = true;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_LOST, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.receiver_lost);
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("receiver lost after signal-group reset", r.reason);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    /* After the signal group only version polls and detection probes went out. */
    for (int i = nth(s, "CONFIG SIGNALGROUP 3 6", 0) + 1; i < s->nrx; i++) {
        TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[i].cmd);
    }
}

TEST_CASE("exec: a signal group the receiver keeps refusing fails the session", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->sg_sticky = true;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("signal group rejected", r.reason);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_TRUE(r.rolled_back);
}

TEST_CASE("exec: detection listens first and finds a streaming receiver without sending", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->rx_baud = s->saved_baud = 460800;
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(115200, false, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_EQUAL_UINT32(460800, d.baud);
    TEST_ASSERT_TRUE(d.by_listen);
    TEST_ASSERT_TRUE(d.saw_bytes);
    TEST_ASSERT_EQUAL_UINT32(460800, s->host_baud);
    for (int i = 0; i < s->nrx; i++) {
        TEST_ASSERT_EQUAL_UINT32(115200, s->rx[i].host_baud);   /* nothing sent at 460800 */
        TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[i].cmd);
    }
    TEST_ASSERT_EQUAL_INT(2, s->nrx);   /* two probes at the wrong baud */
}

TEST_CASE("exec: detection probes a quiet receiver and reads its identity", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->output = s->saved_output = false;
    s->rx_baud = s->saved_baud = 921600;
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(115200, false, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_EQUAL_UINT32(921600, d.baud);
    TEST_ASSERT_FALSE(d.by_listen);
    /* The acknowledgement (it comes first) is enough to find it; the
     * identity is the next query, and the log that followed the ack is a
     * late reply, not a restart. */
    TEST_ASSERT_FALSE(d.version_valid);
    gnss_versiona_t v;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_version(&s->ex, false, 0, &v, NULL));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
}

TEST_CASE("exec: detection tells silence from bytes without answers", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->present = false;
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(115200, true, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    int64_t t0 = s->now;
    TEST_ASSERT_EQUAL(UM98X_E_NOT_FOUND, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_FALSE(d.saw_bytes);
    TEST_ASSERT_EQUAL_INT(16, s->nrx);   /* two probes per baud */
    /* Full list: about 8 x (1.5 s + 2 x 2 s). */
    TEST_ASSERT_INT64_WITHIN(1000, t0 + 8 * 5500, s->now);

    /* Something streams at a baud outside the short list. */
    s = sim_init();
    s->rx_baud = s->saved_baud = 57600;
    n = um98x_detect_candidates(115200, false, cands, UM98X_DETECT_MAX);
    TEST_ASSERT_EQUAL(UM98X_E_NOT_FOUND, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_TRUE(d.saw_bytes);
    n = um98x_detect_candidates(115200, true, cands, UM98X_DETECT_MAX);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_EQUAL_UINT32(57600, d.baud);
}

TEST_CASE("exec: the loader prompt stops detection; its restart key brings the application back", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->in_loader = true;
    s->loader_baud = 115200;
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(460800, false, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    TEST_ASSERT_EQUAL(UM98X_E_PROMPT, um98x_exec_detect(&s->ex, cands, n, &d));
    TEST_ASSERT_EQUAL_UINT32(115200, s->host_baud);
    for (int i = 0; i < s->nrx; i++) {
        TEST_ASSERT_EQUAL_UINT32(460800, s->rx[i].host_baud);   /* never a probe into the loader */
    }
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_leave_loader(&s->ex, UM98X_LOADER_WAIT_MS));
    TEST_ASSERT_EQUAL_STRING("6", s->rx[s->nrx - 1].cmd);
    TEST_ASSERT_FALSE(s->in_loader);

    /* An application that does not come back. */
    s = sim_init();
    s->in_loader = true;
    s->loader_baud = 115200;
    s->dies_on_restart = true;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_leave_loader(&s->ex, UM98X_LOADER_WAIT_MS));
}

TEST_CASE("exec: restart evidence: banners and version logs nobody asked for", "[um98x_exec]")
{
    sim_t *s = sim_init();
    um98x_line_t l;
    um98x_line_classify(TV_BANNER, strlen(TV_BANNER), &l);
    unsigned ev = um98x_chan_dispatch(&s->ch, TV_BANNER, strlen(TV_BANNER), &l, s->now);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_INT64(s->now, s->ch.last_answer_ms);

    s->ch.expect_restart = true;   /* our own restart */
    ev = um98x_chan_dispatch(&s->ch, TV_BANNER, strlen(TV_BANNER), &l, s->now);
    TEST_ASSERT_FALSE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(2, s->ch.stats.banners);
    s->ch.expect_restart = false;

    /* A version log within 10 s of our own query is a late reply. */
    um98x_line_classify(TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l);
    s->ch.last_version_write_ms = s->now - UM98X_UNSOLICITED_VERSION_MS;
    ev = um98x_chan_dispatch(&s->ch, TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l, s->now);
    TEST_ASSERT_FALSE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.unmatched);
    s->ch.last_version_write_ms = s->now - UM98X_UNSOLICITED_VERSION_MS - 1;
    ev = um98x_chan_dispatch(&s->ch, TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l, s->now);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(2, s->ch.stats.restarts);

    /* NMEA and broken lines are counted apart and say nothing about liveness. */
    static const char nmea[] = "$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48";
    um98x_line_classify(nmea, strlen(nmea), &l);
    int64_t before = s->ch.last_answer_ms;
    TEST_ASSERT_EQUAL_UINT(0, um98x_chan_dispatch(&s->ch, nmea, strlen(nmea), &l, s->now + 5));
    static const char broken[] = "$command,unlog,response: OK*22";
    um98x_line_classify(broken, strlen(broken), &l);
    TEST_ASSERT_EQUAL_UINT(0, um98x_chan_dispatch(&s->ch, broken, strlen(broken), &l, s->now + 5));
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.other);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.bad);
    TEST_ASSERT_EQUAL_INT64(before, s->ch.last_answer_ms);
}

TEST_CASE("exec: caller commands: queries complete on their data", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    char text[1024];
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_command(&s->ex, "MODE", 0, text, sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text, "MODE BASE 1234 TIME 600 0"));
    char body[UM98X_MODE_BODY_MAX];
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_query_mode(&s->ex, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0", body);

    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_command(&s->ex, "CONFIG", 0, text, sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text, "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 6"));
    TEST_ASSERT_NOT_NULL(strstr(text, "$CONFIG,RTCMB1CB2A"));

    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_command(&s->ex, "VERSIONA", 0, text, sizeof(text)));
    TEST_ASSERT_NOT_NULL(strstr(text, "#VERSIONA,"));
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_command(&s->ex, "unlog com1", 0, NULL, 0));
    TEST_ASSERT_EQUAL(UM98X_E_REJECTED, um98x_exec_command(&s->ex, "NOSUCHCOMMAND", 0, text, sizeof(text)));
    TEST_ASSERT_EQUAL_INT(2, count(s, "NOSUCHCOMMAND"));
    TEST_ASSERT_EQUAL(UM98X_E_ARG, um98x_exec_command(&s->ex, "", 0, NULL, 0));

    /* RESET through the caller's API ends on its ack (or banner). */
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_command(&s->ex, "RESET", 0, NULL, 0));

    /* Shorter caller timeout: 4 sends of 500 ms with 1000 ms gaps. */
    s = sim_init();
    s->present = false;
    int64_t t0 = s->now;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_command(&s->ex, "UNLOG COM1", 500, NULL, 0));
    TEST_ASSERT_EQUAL_INT(4, s->nrx);
    TEST_ASSERT_INT64_WITHIN(20, t0 + 4 * 500 + 3 * 1000, s->now);
}

TEST_CASE("exec: restoring output sends the RTCM messages only", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->output = false;
    um98x_base_profile_t p;
    default_profile(&p);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_restore_rtcm(&s->ex, &p, &r));
    TEST_ASSERT_TRUE(s->output);
    TEST_ASSERT_TRUE(r.rtcm1230_ok);
    TEST_ASSERT_EQUAL_INT(p.rtcm_count, s->nrx);
    TEST_ASSERT_EQUAL_INT(p.rtcm_count, count(s, "RTCM"));
    TEST_ASSERT_EQUAL_INT(0, s->saves);

    /* A failing message does not stop the others. */
    s = sim_init();
    configured(s);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1033");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_restore_rtcm(&s->ex, &p, &r));
    TEST_ASSERT_EQUAL_STRING("RTCM1033 COM1 1", r.failed_cmd);
    TEST_ASSERT_EQUAL_INT(1, count(s, "RTCM1127"));
}

/* ---- antenna descriptor, B1C/B2a, the rollback's RESET timeline, sky view ---- */

#define EXEC_IGS_NAME "TRM59800.00     SCIS"

TEST_CASE("exec: B1C/B2a that reads back off is turned on; on or unknown is left alone", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->b1c = s->saved_b1c = 'D';
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.b1cb2a_sent);
    TEST_ASSERT_TRUE(r.b1cb2a_ok);
    TEST_ASSERT_EQUAL_STRING("", r.b1cb2a_reply);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG RTCMB1CB2A ENABLE"));
    TEST_ASSERT_LESS_THAN_INT(nth(s, "RTCM1005 COM1 1", 0), nth(s, "CONFIG RTCMB1CB2A ENABLE", 0));
    TEST_ASSERT_EQUAL('E', s->saved_b1c);   /* saved with the rest */
    TEST_ASSERT_TRUE(r.settings_read);
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, r.settings.b1cb2a);   /* as it runs now */
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, r.settings.mmp);
    TEST_ASSERT_TRUE(r.settings.mask_known);
    TEST_ASSERT_EQUAL_INT16(50, r.settings.mask_ddeg);

    /* Already on: not sent. */
    s = sim_init();
    configured(s);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.b1cb2a_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG RTCMB1CB2A"));
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_ON, r.settings.b1cb2a);

    /* Not in the dump: unknown, not sent. */
    s = sim_init();
    configured(s);
    s->b1c = s->saved_b1c = 0;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.b1cb2a_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG RTCMB1CB2A"));
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_UNKNOWN, r.settings.b1cb2a);
}

TEST_CASE("exec: a rejected B1C/B2a command is recorded and the session goes on", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->b1c = s->saved_b1c = 'D';
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG RTCMB1CB2A");
    s->reject_n = 10;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_TRUE(r.b1cb2a_sent);
    TEST_ASSERT_FALSE(r.b1cb2a_ok);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", r.b1cb2a_reply);
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG RTCMB1CB2A ENABLE"));   /* one resend after the refusal */
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_EQUAL_UINT8(UM98X_SETTING_OFF, r.settings.b1cb2a);   /* still off */
    TEST_ASSERT_EQUAL_INT(8, count(s, "RTCM"));
}

TEST_CASE("exec: a restart while B1C/B2a is in flight fails the session", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    s->b1c = s->saved_b1c = 'D';
    s->b1c_restarts = true;
    s->restart_ms = 1500;   /* the banner comes well within the command's reply time */
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("CONFIG RTCMB1CB2A ENABLE", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("receiver restarted after RTCMB1CB2A", r.reason);
    TEST_ASSERT_FALSE(r.saved);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RTCM1005"));
}

TEST_CASE("exec: a restart after the B1C/B2a ack, before the save, fails the session", "[um98x_exec]")
{
    /* The receiver acknowledges, then restarts: it comes back with its saved
     * base mode, and a save would store that with our outputs. */
    sim_t *s = sim_init();
    configured(s);
    strcpy(s->mode, "MODE ROVER");   /* the base mode is sent in this session */
    strcpy(s->saved_mode, s->mode);
    s->b1c = s->saved_b1c = 'D';
    snprintf(s->restart_after, sizeof(s->restart_after), "CONFIG RTCMB1CB2A");
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("CONFIG RTCMB1CB2A ENABLE", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("receiver restarted after RTCMB1CB2A", r.reason);
    TEST_ASSERT_TRUE(r.mode_sent);
    TEST_ASSERT_TRUE(r.b1cb2a_sent);
    TEST_ASSERT_FALSE(r.b1cb2a_ok);
    TEST_ASSERT_EQUAL_STRING("receiver restarted", r.b1cb2a_reply);
    TEST_ASSERT_FALSE(r.saved);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, s->saves);
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_STRING("MODE ROVER", s->saved_mode);

    /* Any restart between the configuring steps and the save does the same. */
    s = sim_init();
    configured(s);
    strcpy(s->mode, "MODE ROVER");
    strcpy(s->saved_mode, s->mode);
    snprintf(s->restart_after, sizeof(s->restart_after), "RTCM1005");
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("receiver restarted during the session", r.reason);
    TEST_ASSERT_FALSE(r.b1cb2a_sent);
    TEST_ASSERT_FALSE(r.saved);
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_STRING("MODE ROVER", s->saved_mode);
}

TEST_CASE("exec: the antenna descriptor is sent only when the receiver holds another one", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);   /* no antenna line in its dump: unknown */
    um98x_base_profile_t p;
    default_profile(&p);
    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", EXEC_IGS_NAME);
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000112233");
    p.antenna_setup_id = 3;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.antenna_sent);
    TEST_ASSERT_TRUE(r.antenna_ok);
    int ant = nth(s, "CONFIG BASEANTENNAMODEL \"" EXEC_IGS_NAME "\" \"5000112233\" 3 USER", 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ant);
    TEST_ASSERT_LESS_THAN_INT(nth(s, "RTCM1005 COM1 1", 0), ant);
    TEST_ASSERT_EQUAL_STRING("\"" EXEC_IGS_NAME "\" \"5000112233\" 3 USER", s->saved_ant);
    TEST_ASSERT_TRUE(r.settings.antenna_known);
    TEST_ASSERT_EQUAL_STRING(EXEC_IGS_NAME, r.settings.antenna_model);

    /* The receiver holds it now: the next session reads it back and sends nothing. */
    s->nrx = 0;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.antenna_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG BASEANTENNAMODEL"));
    TEST_ASSERT_TRUE(r.settings.antenna_known);
    TEST_ASSERT_EQUAL_STRING("5000112233", r.settings.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(3, r.settings.antenna_setup_id);

    /* No model in the profile: never sent, the receiver keeps its own. */
    s = sim_init();
    configured(s);
    default_profile(&p);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.antenna_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG BASEANTENNAMODEL"));

    /* A rejection is recorded; the session is saved without it. */
    s = sim_init();
    configured(s);
    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", EXEC_IGS_NAME);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG BASEANTENNAMODEL");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_TRUE(r.antenna_sent);
    TEST_ASSERT_FALSE(r.antenna_ok);
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", r.antenna_reply);
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_FALSE(r.settings.antenna_known);
}

/* Index of the last command named cmd, -1 if none. */
static int last_of(const sim_t *s, const char *cmd)
{
    for (int i = s->nrx - 1; i >= 0; i--) {
        if (strcmp(s->rx[i].cmd, cmd) == 0) {
            return i;
        }
    }
    return -1;
}

TEST_CASE("exec: the rollback RESET listens until the banner, then asks", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1077");
    s->reject_n = 10;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.rollback_answered);
    int reset = nth(s, "RESET", 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, reset);
    TEST_ASSERT_LESS_THAN_INT(s->nrx, reset + 1);
    /* Nothing between the RESET and its banner (3 s later): the first query
     * comes after it. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(s->rx[reset].at + s->restart_ms, s->rx[reset + 1].at);
    TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[reset + 1].cmd);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);   /* our own restart */
    TEST_ASSERT_FALSE(s->ch.expect_restart);
}

TEST_CASE("exec: without a banner the rollback asks from 15 s to 20 s, then 10 s at the previous baud", "[um98x_exec]")
{
    /* The baud change took (unsaved), then a fatal step: the RESET goes out
     * at 460800, the receiver's saved baud is 115200; it never comes back. */
    sim_t *s = sim_init();
    configured(s);
    s->banner = false;
    s->dies_on_restart = true;
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1077");
    s->reject_n = 10;
    um98x_base_profile_t p;
    default_profile(&p);
    p.com1_baud = 460800;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_FALSE(r.rollback_answered);
    TEST_ASSERT_EQUAL_UINT32(0, r.answer_baud);
    int reset = nth(s, "RESET", 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, reset);
    TEST_ASSERT_EQUAL_UINT32(460800, s->rx[reset].host_baud);
    int64_t t0 = s->rx[reset].at;
    int at_saved = 0;
    int at_previous = 0;
    for (int i = reset + 1; i < s->nrx; i++) {
        TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[i].cmd);
        TEST_ASSERT_GREATER_OR_EQUAL_INT64(t0 + UM98X_RESTART_BANNER_MS, s->rx[i].at);   /* silent for 15 s */
        if (s->rx[i].host_baud == 115200) {
            TEST_ASSERT_LESS_OR_EQUAL_INT64(t0 + UM98X_RESTART_LOST_MS, s->rx[i].at);
            TEST_ASSERT_EQUAL_INT(0, at_previous);   /* the saved baud first */
            at_saved++;
        } else {
            TEST_ASSERT_EQUAL_UINT32(460800, s->rx[i].host_baud);
            TEST_ASSERT_LESS_OR_EQUAL_INT64(t0 + UM98X_RESTART_LOST_MS + UM98X_ROLLBACK_PREVIOUS_MS + 100, s->rx[i].at);
            at_previous++;
        }
    }
    TEST_ASSERT_INT_WITHIN(1, 5, at_saved);       /* one a second from 15 s to 20 s */
    TEST_ASSERT_INT_WITHIN(1, 10, at_previous);   /* one a second for 10 s */
    TEST_ASSERT_EQUAL_INT(last_of(s, "VERSIONA"), s->nrx - 1);
}

TEST_CASE("exec: satellite-in-view output on and off, never saved", "[um98x_exec]")
{
    sim_t *s = sim_init();
    configured(s);
    char why[64];
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_sat_view(&s->ex, true, 5, why, sizeof(why)));
    TEST_ASSERT_EQUAL_UINT(5, s->gsv);
    TEST_ASSERT_EQUAL_STRING("", why);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_sat_view(&s->ex, false, 0, why, sizeof(why)));
    TEST_ASSERT_EQUAL_UINT(0, s->gsv);
    TEST_ASSERT_EQUAL_INT(2, s->nrx);
    TEST_ASSERT_EQUAL_STRING("GPGSV COM1 5", s->rx[0].cmd);
    TEST_ASSERT_EQUAL_STRING("UNLOG COM1 GPGSV", s->rx[1].cmd);
    TEST_ASSERT_EQUAL_INT(0, s->saves);

    /* A restart ends it (it was never saved). */
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_sat_view(&s->ex, true, 5, why, sizeof(why)));
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_reset(&s->ex));
    TEST_ASSERT_EQUAL_UINT(0, s->gsv);

    /* Rejected, or no answer. */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "GPGSV");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_REJECTED, um98x_exec_sat_view(&s->ex, true, 5, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("PARSING FAILD NO MATCHING FUNC", why);
    s = sim_init();
    s->present = false;
    int64_t t0 = s->now;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_sat_view(&s->ex, true, 5, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("timeout", why);
    /* Two sends: the toggle holds RTCM back for less than a caster's 10 s stall limit. */
    TEST_ASSERT_EQUAL_INT(2, count(s, "GPGSV COM1 5"));
    TEST_ASSERT_LESS_THAN_INT64(10000, s->now - t0);
    TEST_ASSERT_EQUAL(UM98X_E_ARG, um98x_exec_sat_view(&s->ex, true, 0, why, sizeof(why)));
}
