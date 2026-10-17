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
 * up for the test. Its refusals are worded as UM982 build 25164 words them
 * ("PARSING FAILED GRAMMAR ERROR," for a signal group, "PARSING FAILED NO
 * MATCHING FUNC" otherwise), it can answer commands sent while it restarts
 * with build 25164's restarting answer (notices), and it answers the
 * signal-group query with the bodies the bench recorded for its group.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "um98x_bands.h"
#include "um98x_base.h"
#include "um98x_exec.h"
#include "um98x_profile.h"
#include "um98x_test_vectors.h"
#include "unity.h"

#define T_START   ((int64_t)UINT32_MAX + 1000)
#define SIM_REFUSAL "PARSING FAILED NO MATCHING FUNC"
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
    bool ant_collapse;    /* stores the descriptor with runs of spaces as one, as build 25164 does */
    unsigned gsv;         /* satellite-in-view output period, 0 = off */
    bool sg_quiet_only;   /* a signal-group command is taken (and acknowledged) only with COM1 output off */
    int64_t version_mute_ms;     /* after a restart, the version query is acknowledged without its log this long */
    int64_t version_mute_until;
    int version_empty;           /* version queries answered without the log */
    int sg_line_lost;            /* after a signal-group change: this many dumps come without their SIGNALGROUP line */
    int sg_line_lost_left;
    bool notices;                /* commands sent while it restarts get the restarting answer */
    int groups;                  /* signal-group query: 0 the bench body for its group, 1 refused (no such
                                    command), 2 acknowledged without a log */
    int64_t groups_log_ms;       /* its log this long after the acknowledgement (0: 20 ms) */
    const char *basepos;         /* BASEPOSA: the body of its log (NULL: refused, no such command) */
    const char *sppnav;          /* SPPNAVA: likewise */

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

/* A line that does not fit is a fault in the test, so it fails the test
 * instead of reaching the executor cut short. */
static void emit(sim_t *s, const char *line, int64_t delay)
{
    TEST_ASSERT_LESS_THAN_INT(MAX_EV, s->nq);
    sim_ev_t *e = &s->q[s->nq++];
    e->at = s->now + delay;
    e->baud = s->rx_baud;
    int n = snprintf(e->line, sizeof(e->line), "%s", line);
    TEST_ASSERT_TRUE_MESSAGE(n >= 0 && (size_t)n < sizeof(e->line), "simulated line longer than LINE_MAX_");
}

static void emit_checked(sim_t *s, char lead, const char *content, int64_t delay)
{
    char tmp[LINE_MAX_ - 3];   /* leaves room for "*XX" in a queued line */
    char line[LINE_MAX_];
    int n = snprintf(tmp, sizeof(tmp), "%c%s", lead, content);
    TEST_ASSERT_TRUE_MESSAGE(n >= 0 && (size_t)n < sizeof(tmp), "simulated line longer than LINE_MAX_");
    snprintf(line, sizeof(line), "%s*%02X", tmp, um98x_xor8(tmp, (size_t)n));
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
    s->version_mute_until = s->busy_until + s->version_mute_ms;
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

    if (!s->present) {
        return;
    }
    if (s->now < s->busy_until) {
        if (s->notices && !s->in_loader && s->host_baud == s->rx_baud) {
            emit(s, TV_NOTICE, 30);   /* no echo: it answers whatever comes while it restarts */
        }
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
        reply(s, cmd, starts(up, "CONFIG SIGNALGROUP") ? TV_SG_REFUSAL : SIM_REFUSAL, 30);
        return;
    }
    unsigned a, b;
    if (strcmp(up, "VERSIONA") == 0) {
        reply(s, cmd, "OK", 30);
        if (s->now < s->version_mute_until) {
            s->version_empty++;   /* "OK", but no version yet so soon after a restart */
        } else {
            emit(s, s->version, 50);
        }
    } else if (strcmp(up, "UNLOG COM1") == 0) {
        s->output = false;
        reply(s, cmd, "OK", 30);
    } else if (strcmp(up, "CONFIG") == 0) {
        char item[LINE_MAX_];
        reply(s, cmd, "OK", 30);
        if (s->sg_line_lost_left > 0) {
            s->sg_line_lost_left--;   /* the line is lost (a bad checksum is dropped the same way) */
        } else {
            snprintf(item, sizeof(item), "CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP %u %u", s->sg_m, s->sg_s);
            emit_checked(s, '$', item, 60);
        }
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
        if (s->sg_quiet_only && s->output) {
            return;   /* ignored while the port still streams */
        }
        bool change = a != s->sg_m || b != s->sg_s;
        if (!(change && s->sg_no_ack)) {
            reply(s, cmd, "OK", 30);
        }
        if (change) {
            if (!s->sg_sticky) {
                s->sg_m = (uint8_t)a;   /* saves itself */
                s->sg_s = (uint8_t)b;
            }
            s->sg_line_lost_left = s->sg_line_lost;
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
    } else if (strcmp(up, "ANT1GROUPA") == 0 && s->groups != 1) {
        reply(s, cmd, "OK", 30);
        if (s->groups == 0) {
            /* Main antenna input, per group, as build 25164 reported it. */
            static const char *const k_body[8] = {
                [3] = "3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35",
                [4] = "4,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x07,GQ,0x15",
                [5] = "5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05",
                [7] = "7,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15",
            };
            char log[LINE_MAX_];
            const char *body = s->sg_m < 8 && k_body[s->sg_m] != NULL ? k_body[s->sg_m] : "0,0";
            snprintf(log, sizeof(log), "ANT1GROUPA,COM1,0,0.0,FINE,0,0.000,0,0,0;%s", body);
            emit_checked(s, '#', log, s->groups_log_ms > 0 ? 30 + s->groups_log_ms : 50);
        }
    } else if ((strcmp(up, "BASEPOSA") == 0 && s->basepos != NULL) || (strcmp(up, "SPPNAVA") == 0 && s->sppnav != NULL)) {
        bool base = up[0] == 'B';
        char log[LINE_MAX_];
        reply(s, cmd, "OK", 30);
        snprintf(log, sizeof(log), "%s,COM1,0,0.0,FINE,0,0.000,0,0,0;%s", base ? "BASEPOSA" : "SPPNAVA",
                 base ? s->basepos : s->sppnav);
        emit_checked(s, '#', log, 50);
    } else if (sscanf(up, "RTCM%u COM1 %u", &a, &b) == 2) {
        if (a == 1230 && !s->r1230) {
            reply(s, cmd, "PARSING FAILED NO MATCHING FUNC  RTCM1230", 30);
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
        if (s->ant_collapse) {
            size_t o = 0;
            for (size_t i = 0; s->ant[i] != '\0'; i++) {
                if (!(s->ant[i] == ' ' && o > 0 && s->ant[o - 1] == ' ')) {
                    s->ant[o++] = s->ant[i];
                }
            }
            s->ant[o] = '\0';
        }
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
        reply(s, cmd, SIM_REFUSAL, 30);
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

TEST_CASE("exec: published receiver habits: signal group only on a quiet port, version late after a restart",
          "[um98x_exec]")
{
    /* Two behaviours reported for these receivers in public code: the signal
     * group is acknowledged only once output is off, and shortly after a
     * restart the version query gets its "OK" without the version (1 s too
     * early, 2 s fine). The session quiets COM1 first and keeps polling the
     * version after the restart until the log comes. */
    sim_t *s = sim_init();
    s->sg_quiet_only = true;
    s->version_mute_ms = 2000;
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    memset(&id, 0, sizeof(id));
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_STRING("UM982", id.model);
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
    TEST_ASSERT_EQUAL_UINT8(6, s->sg_s);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_LESS_THAN_INT(nth(s, "CONFIG SIGNALGROUP 3 6", 0), nth(s, "UNLOG COM1", 0));
    TEST_ASSERT_GREATER_THAN_INT(0, s->version_empty);   /* the early polls got "OK" only */
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));

    /* A signal group sent while the port streams gets nothing back: the
     * command times out without a change, so it must never go first. */
    s = sim_init();
    s->sg_quiet_only = true;
    char text[64];
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_command(&s->ex, "CONFIG SIGNALGROUP 3 6", 0, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT8(4, s->sg_m);
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
    TEST_ASSERT_EQUAL_STRING(SIM_REFUSAL, r.reason);
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

    /* A negative reply still fails the step at once (and changed nothing:
     * with the group read back to go to, no RESET). */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING(TV_SG_REFUSAL, r.reason);
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_TRUE(r.signal_group_refused_plain);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
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

TEST_CASE("exec: a signal group the receiver refuses is reported with the group it ran before", "[um98x_exec]")
{
    /* A receiver firmware that does not offer 7 0 refuses it outright, as
     * build 25164 refuses every pair but its four: nothing changed and no
     * restart, so no RESET either (the caller applies 4 5 again at once,
     * which restores the quieted output). The receiver keeps its group. */
    sim_t *s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP 7 0");
    s->reject_n = 10;
    um98x_base_profile_t p;
    default_profile(&p);
    p.signal_group_master = 7;
    p.signal_group_slave = 0;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_rejected);
    TEST_ASSERT_TRUE(r.signal_group_refused_plain);
    TEST_ASSERT_FALSE(r.signal_group_sent);
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 7 0", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING(TV_SG_REFUSAL, r.reason);
    TEST_ASSERT_TRUE(r.sg_before.known);
    TEST_ASSERT_EQUAL_UINT8(4, r.sg_before.master);
    TEST_ASSERT_EQUAL_UINT8(5, r.sg_before.slave);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.banners);   /* the receiver never restarted */
    TEST_ASSERT_EQUAL_UINT8(4, s->sg_m);   /* the saved group is unchanged */
    TEST_ASSERT_EQUAL_UINT8(5, s->sg_s);

    /* The same refusal when the read-back named no group: nothing to go
     * back to, so the rollback restores the outputs with RESET as for any
     * failure; unless the caller has a group of its own to go back to. */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP 7 0");
    s->reject_n = 10;
    s->sg_line_lost_left = 1;   /* the first dump comes without its SIGNALGROUP line */
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_refused_plain);
    TEST_ASSERT_FALSE(r.sg_before.known);
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(1, count(s, "RESET"));
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP 7 0");
    s->reject_n = 10;
    s->sg_line_lost_left = 1;
    um98x_apply_opts_t o = { .sg_fallback = true };
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, &o, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_refused_plain);
    TEST_ASSERT_FALSE(r.rolled_back);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));

    /* Acknowledged, restarted, and the read-back still shows 4 5. */
    s = sim_init();
    s->sg_sticky = true;
    default_profile(&p);
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_TRUE(r.signal_group_rejected);
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("signal group rejected", r.reason);
    TEST_ASSERT_TRUE(r.sg_before.known);
    TEST_ASSERT_EQUAL_UINT8(4, r.sg_before.master);
    TEST_ASSERT_EQUAL_UINT8(5, r.sg_before.slave);
    TEST_ASSERT_TRUE(r.sg_read.known);   /* the read-back after the restart: still 4 5 */
    TEST_ASSERT_EQUAL_UINT8(4, r.sg_read.master);
    TEST_ASSERT_EQUAL_UINT8(5, r.sg_read.slave);
    TEST_ASSERT_TRUE(r.rolled_back);

    /* Any other failure is not a refused group; an accepted one carries the
     * group from before as well. */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1077");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_FALSE(r.signal_group_rejected);
    s = sim_init();
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.signal_group_rejected);
    TEST_ASSERT_TRUE(r.sg_before.known);
    TEST_ASSERT_EQUAL_UINT8(4, r.sg_before.master);
    TEST_ASSERT_EQUAL_UINT8(3, r.sg_read.master);   /* read back after the change */
    TEST_ASSERT_EQUAL_UINT8(6, r.sg_read.slave);
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
}

TEST_CASE("exec: a read-back that does not name the signal group is no refusal", "[um98x_exec]")
{
    /* The dump has no end marker, and a line with a bad checksum is
     * dropped: after the restart the first dump lacks the SIGNALGROUP line.
     * It is read once more, which names 3 6: the session goes on. */
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    sim_t *s = sim_init();
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    int dumps = 0;
    while (nth(s, "CONFIG", dumps) >= 0) {
        dumps++;
    }

    s = sim_init();
    s->sg_line_lost = 1;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.signal_group_rejected);
    TEST_ASSERT_TRUE(r.sg_read.known);
    TEST_ASSERT_EQUAL_UINT8(3, r.sg_read.master);
    TEST_ASSERT_EQUAL_UINT8(6, r.sg_read.slave);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_INT(1, s->saves);
    /* one dump more than without the loss: the read-back after the restart, once more */
    TEST_ASSERT_TRUE(nth(s, "CONFIG", dumps) >= 0);
    TEST_ASSERT_EQUAL_INT(-1, nth(s, "CONFIG", dumps + 1));

    /* Never named: an ordinary failure (the station retries later), not a
     * refused group that would put the group from before back. */
    s = sim_init();
    s->sg_line_lost = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.signal_group_rejected);
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_FALSE(r.sg_read.known);
    TEST_ASSERT_EQUAL_STRING("CONFIG SIGNALGROUP 3 6", r.failed_cmd);
    TEST_ASSERT_EQUAL_STRING("signal group not in the read-back", r.reason);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_INT(0, count(s, "SAVECONFIG"));
    TEST_ASSERT_TRUE(r.rolled_back);
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);   /* the receiver did take it (and saved it itself) */
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
    /* The acknowledgement comes first; the probe then waits for the log
     * that follows it, which gives the identity (and is read whole, not cut
     * by the caller's lease release), and is no restart. */
    TEST_ASSERT_TRUE(d.version_valid);
    TEST_ASSERT_EQUAL_STRING("UM982", d.version.model);
    int at_baud = 0;
    for (int i = 0; i < s->nrx; i++) {
        at_baud += (s->rx[i].host_baud == 921600);
    }
    TEST_ASSERT_EQUAL_INT(1, at_baud);   /* one probe at the right baud: no second query for the identity */
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.unmatched);
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

/* "#<content>*crc32" (the reflected CRC-32 0xEDB88320, initial 0, no final XOR). */
static void crc_log(char *out, size_t cap, const char *content)
{
    uint32_t crc = 0;
    for (const char *p = content; *p != '\0'; p++) {
        crc ^= (uint8_t)*p;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    snprintf(out, cap, "#%s*%08x", content, (unsigned)crc);
}

TEST_CASE("exec: every version log tells the receiver's time since its restart while it has no time", "[um98x_exec]")
{
    sim_t *s = sim_init();
    TEST_ASSERT_EQUAL_INT64(UM98X_NONE_MS, s->ch.last_version_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s->ch.version_uptime_ms);
    char line[300];
    crc_log(line, sizeof(line),
            "VERSIONA,40,GPS,UNKNOWN,1,1014000,0,0,18,41;\"UM982\",\"R4.10Build25164\",\"HRPT00-S10C-P\","
            "\"1234567890123-SYNTH00000001\",\"0123456789abcdef\",\"2025/06/17\"");
    um98x_line_t l;
    um98x_line_classify(line, strlen(line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_VERSION, l.kind);
    /* Whoever asked: here a late reply to our own query. */
    s->ch.last_version_write_ms = s->now - 100;
    (void)um98x_chan_dispatch(&s->ch, line, strlen(line), &l, s->now);
    TEST_ASSERT_EQUAL_INT64(s->now, s->ch.last_version_ms);
    TEST_ASSERT_EQUAL_UINT32(1014000, s->ch.version_uptime_ms);

    /* With a time it does not tell. */
    um98x_line_classify(TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l);
    (void)um98x_chan_dispatch(&s->ch, TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l, s->now + 10);
    TEST_ASSERT_EQUAL_INT64(s->now + 10, s->ch.last_version_ms);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s->ch.version_uptime_ms);
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

TEST_CASE("exec: replies to the station's commands are told from what the receiver sends unasked", "[um98x_exec]")
{
    sim_t *s = sim_init();
    um98x_line_t l;
    /* An acknowledgement nothing waits for (a late one), and the restarting
     * answer: replies (only commands get these on this port). */
    char ack[96];
    char tmp[80];
    int n = snprintf(tmp, sizeof(tmp), "$command,MODE,response: OK");
    snprintf(ack, sizeof(ack), "%s*%02X", tmp, um98x_xor8(tmp, (size_t)n));
    um98x_line_classify(ack, strlen(ack), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_TRUE(um98x_chan_dispatch(&s->ch, ack, strlen(ack), &l, s->now) & UM98X_CHAN_REPLY);
    um98x_line_classify(TV_NOTICE, strlen(TV_NOTICE), &l);
    TEST_ASSERT_TRUE(um98x_chan_dispatch(&s->ch, TV_NOTICE, strlen(TV_NOTICE), &l, s->now) & UM98X_CHAN_REPLY);

    /* A version log: a reply within 10 s of our own query, the receiver's own
     * restart news after that. */
    um98x_line_classify(TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l);
    s->ch.last_version_write_ms = s->now - 500;
    TEST_ASSERT_TRUE(um98x_chan_dispatch(&s->ch, TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l, s->now) &
                     UM98X_CHAN_REPLY);
    s->ch.last_version_write_ms = s->now - UM98X_UNSOLICITED_VERSION_MS - 1;
    unsigned ev = um98x_chan_dispatch(&s->ch, TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), &l, s->now + 30000);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_FALSE(ev & UM98X_CHAN_REPLY);

    /* A banner: a reply to our own restart, unasked otherwise. */
    um98x_line_classify(TV_BANNER, strlen(TV_BANNER), &l);
    s->ch.expect_restart = true;
    TEST_ASSERT_TRUE(um98x_chan_dispatch(&s->ch, TV_BANNER, strlen(TV_BANNER), &l, s->now + 40000) & UM98X_CHAN_REPLY);
    s->ch.expect_restart = false;
    TEST_ASSERT_FALSE(um98x_chan_dispatch(&s->ch, TV_BANNER, strlen(TV_BANNER), &l, s->now + 80000) &
                      UM98X_CHAN_REPLY);

    /* NMEA (the satellite-in-view output) and broken lines: unasked. */
    static const char nmea[] = "$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48";
    um98x_line_classify(nmea, strlen(nmea), &l);
    TEST_ASSERT_FALSE(um98x_chan_dispatch(&s->ch, nmea, strlen(nmea), &l, s->now) & UM98X_CHAN_REPLY);
    static const char broken[] = "$command,unlog,response: OK*22";
    um98x_line_classify(broken, strlen(broken), &l);
    TEST_ASSERT_FALSE(um98x_chan_dispatch(&s->ch, broken, strlen(broken), &l, s->now) & UM98X_CHAN_REPLY);

    /* The answer to a query in flight. */
    char body[UM98X_MODE_BODY_MAX];
    configured(s);
    s->events = 0;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_query_mode(&s->ex, body, sizeof(body)));
    TEST_ASSERT_EQUAL_STRING("MODE BASE 1234 TIME 600 0", body);
    TEST_ASSERT_TRUE(s->events & UM98X_CHAN_REPLY);
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
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_restore_rtcm(&s->ex, &p, NULL, 0, &r));
    TEST_ASSERT_TRUE(s->output);
    TEST_ASSERT_TRUE(r.rtcm1230_ok);
    TEST_ASSERT_EQUAL_INT(p.rtcm_count, s->nrx);
    TEST_ASSERT_EQUAL_INT(p.rtcm_count, count(s, "RTCM"));
    TEST_ASSERT_EQUAL_INT(0, s->saves);
    TEST_ASSERT_EQUAL_UINT8(0, r.rtcm_refused_n);

    /* A failing message does not stop the others. */
    s = sim_init();
    configured(s);
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1033");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_restore_rtcm(&s->ex, &p, NULL, 0, &r));
    TEST_ASSERT_EQUAL_STRING("RTCM1033 COM1 1", r.failed_cmd);
    TEST_ASSERT_EQUAL_INT(1, count(s, "RTCM1127"));
    TEST_ASSERT_EQUAL_UINT8(1, r.rtcm_refused_n);
    TEST_ASSERT_EQUAL_UINT16(1033, r.rtcm_refused[0]);
}

TEST_CASE("exec: a type the receiver refused in the failed apply is left out of the restore", "[um98x_exec]")
{
    /* As on the bench: the profile holds a type this firmware does not
     * know; the apply fails on it (rolled back), and the restore that
     * follows must not send it again. */
    sim_t *s = sim_init();
    configured(s);
    um98x_base_profile_t p;
    default_profile(&p);
    p.rtcm[p.rtcm_count].msg = 1999;
    p.rtcm[p.rtcm_count].period_s = 1;
    p.rtcm_count++;
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "RTCM1999");
    s->reject_n = 100;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_STRING("RTCM1999 COM1 1", r.failed_cmd);
    TEST_ASSERT_EQUAL_UINT8(1, r.rtcm_refused_n);
    TEST_ASSERT_EQUAL_UINT16(1999, r.rtcm_refused[0]);
    int sent = count(s, "RTCM1999 COM1 1");

    /* The restore: every other type, 1999 not again; still listed as refused. */
    s->nrx = 0;
    uint16_t skip[UM98X_MAX_RTCM_MSGS];
    memcpy(skip, r.rtcm_refused, sizeof(skip));
    size_t n_skip = r.rtcm_refused_n;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_restore_rtcm(&s->ex, &p, skip, n_skip, &r));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RTCM1999 COM1 1"));
    TEST_ASSERT_EQUAL_INT(p.rtcm_count - 1, count(s, "RTCM"));
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_EQUAL_UINT8(1, r.rtcm_refused_n);
    TEST_ASSERT_EQUAL_UINT16(1999, r.rtcm_refused[0]);
    TEST_ASSERT_GREATER_THAN_INT(0, sent);

    /* An optional type refused in the apply (1230 on a firmware without it)
     * is listed too, and the restore leaves it out as well. */
    s = sim_init();
    configured(s);
    s->r1230 = false;
    default_profile(&p);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.rtcm1230_ok);
    TEST_ASSERT_EQUAL_UINT8(1, r.rtcm_refused_n);
    TEST_ASSERT_EQUAL_UINT16(1230, r.rtcm_refused[0]);
    s->nrx = 0;
    memcpy(skip, r.rtcm_refused, sizeof(skip));
    n_skip = r.rtcm_refused_n;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_restore_rtcm(&s->ex, &p, skip, n_skip, &r));
    TEST_ASSERT_EQUAL_INT(0, count(s, "RTCM1230"));
    TEST_ASSERT_FALSE(r.rtcm1230_ok);
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
    TEST_ASSERT_EQUAL_STRING(SIM_REFUSAL, r.b1cb2a_reply);
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
    TEST_ASSERT_EQUAL_STRING(SIM_REFUSAL, r.antenna_reply);
    TEST_ASSERT_EQUAL_STRING("", r.failed_cmd);
    TEST_ASSERT_FALSE(r.settings.antenna_known);
    TEST_ASSERT_EQUAL_INT(-1, nth(s, "CONFIG", 1));   /* the session's dump only: no read-back after a refused step */
}

TEST_CASE("exec: a receiver that collapses spaces in the antenna name: read back, sent once", "[um98x_exec]")
{
    /* Build 25164 on the bench: "TRM59800.00     SCIS" (the IGS name
     * padded to 16 characters, then the radome) is stored as
     * "TRM59800.00 SCIS". */
    sim_t *s = sim_init();
    configured(s);
    s->ant_collapse = true;
    um98x_base_profile_t p;
    default_profile(&p);
    snprintf(p.antenna_model, sizeof(p.antenna_model), "TRM59800.00     SCIS");
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000123");
    p.antenna_setup_id = 0;
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.antenna_sent);
    TEST_ASSERT_TRUE(r.antenna_ok);
    TEST_ASSERT_EQUAL_INT(1, count(s, "CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"5000123\" 0 USER"));
    TEST_ASSERT_EQUAL_STRING("\"TRM59800.00 SCIS\" \"5000123\" 0 USER", s->saved_ant);
    /* The dump read right after the step, before the outputs: what the
     * receiver holds is what the status shows, not the profile's text. */
    int ant = nth(s, "CONFIG BASEANTENNAMODEL \"TRM59800.00     SCIS\" \"5000123\" 0 USER", 0);
    TEST_ASSERT_EQUAL_STRING("CONFIG", s->rx[ant + 1].cmd);
    TEST_ASSERT_LESS_THAN_INT(nth(s, "RTCM1005 COM1 1", 0), ant + 1);
    TEST_ASSERT_TRUE(r.settings.antenna_known);
    TEST_ASSERT_EQUAL_STRING("TRM59800.00 SCIS", r.settings.antenna_model);
    TEST_ASSERT_EQUAL_STRING("5000123", r.settings.antenna_serial);

    /* The next apply reads "TRM59800.00 SCIS" back: the same name, nothing sent. */
    s->nrx = 0;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.antenna_sent);
    TEST_ASSERT_EQUAL_INT(0, count(s, "CONFIG BASEANTENNAMODEL"));
    TEST_ASSERT_EQUAL_STRING("TRM59800.00 SCIS", r.settings.antenna_model);
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
    TEST_ASSERT_EQUAL_STRING(SIM_REFUSAL, why);
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

/* ---- the restarting answer and the signal-group query -------------------- */

static unsigned feed_line(sim_t *s, const char *line, int64_t at)
{
    um98x_line_t l;
    um98x_line_classify(line, strlen(line), &l);
    return um98x_chan_dispatch(&s->ch, line, strlen(line), &l, at);
}

TEST_CASE("exec: the restarting answer is restart evidence once, and the receiver answers meanwhile", "[um98x_exec]")
{
    sim_t *s = sim_init();
    int64_t t0 = s->now;
    unsigned ev = feed_line(s, TV_NOTICE, t0);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_WAKE);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.resetting);
    TEST_ASSERT_EQUAL_INT64(t0, s->ch.last_answer_ms);   /* it answers: not silent */
    TEST_ASSERT_TRUE(um98x_chan_restarting(&s->ch, t0 + 1000));

    /* More answers of the same restart, then its banner: still one restart,
     * the evidence time moved to the banner, and no longer restarting. */
    TEST_ASSERT_FALSE(feed_line(s, TV_NOTICE, t0 + 1000) & UM98X_CHAN_RESTART);
    TEST_ASSERT_FALSE(feed_line(s, TV_NOTICE, t0 + 2000) & UM98X_CHAN_RESTART);
    ev = feed_line(s, TV_BANNER, t0 + 8500);
    TEST_ASSERT_FALSE(ev & UM98X_CHAN_RESTART);
    TEST_ASSERT_TRUE(ev & UM98X_CHAN_WAKE);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(3, s->ch.stats.resetting);
    TEST_ASSERT_EQUAL_INT64(t0 + 8500, s->ch.last_restart_ms);
    TEST_ASSERT_FALSE(um98x_chan_restarting(&s->ch, t0 + 8600));

    /* Said once and never followed by a banner: restarting for 20 s only. */
    s = sim_init();
    t0 = s->now;
    (void)feed_line(s, TV_NOTICE, t0);
    TEST_ASSERT_TRUE(um98x_chan_restarting(&s->ch, t0 + UM98X_RESTART_LOST_MS));
    TEST_ASSERT_FALSE(um98x_chan_restarting(&s->ch, t0 + UM98X_RESTART_LOST_MS + 1));
    /* A later restart counts again. */
    TEST_ASSERT_TRUE(feed_line(s, TV_NOTICE, t0 + UM98X_RESTART_LOST_MS + 1) & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(2, s->ch.stats.restarts);
    /* A banner long after it is a restart of its own. */
    TEST_ASSERT_TRUE(feed_line(s, TV_BANNER, t0 + 3 * UM98X_RESTART_LOST_MS) & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(3, s->ch.stats.restarts);

    /* During our own restart it is expected: no evidence. */
    s = sim_init();
    s->ch.expect_restart = true;
    TEST_ASSERT_FALSE(feed_line(s, TV_NOTICE, s->now) & UM98X_CHAN_RESTART);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.resetting);
    TEST_ASSERT_EQUAL_INT64(UM98X_NONE_MS, s->ch.notice_restart_ms);
}

TEST_CASE("exec: an identity query that meets a restart says not ready, and counts it", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->notices = true;
    s->restart_ms = 9000;   /* as build 25164: the banner 8-9 s after the restart began */
    restart(s, s->restart_ms);   /* the receiver restarts on its own */
    gnss_versiona_t v;
    bool not_ready = false;
    TEST_ASSERT_EQUAL(UM98X_E_TIMEOUT, um98x_exec_version(&s->ex, false, 0, &v, &not_ready));
    TEST_ASSERT_TRUE(not_ready);   /* it answered: restarting, not silent */
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(1, s->ch.stats.resetting);
    TEST_ASSERT_TRUE(s->events & UM98X_CHAN_RESTART);

    /* In restart mode the polls run on until the banner and the log. */
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_version(&s->ex, true, s->now + 20000, &v, &not_ready));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.restarts);   /* the banner ends the same restart */
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.banners);
}

TEST_CASE("exec: detection finds a restarting receiver by its answer", "[um98x_exec]")
{
    sim_t *s = sim_init();
    s->notices = true;
    s->output = s->saved_output = false;   /* nothing to hear while listening */
    s->restart_ms = 9000;
    restart(s, s->restart_ms);
    uint32_t cands[] = { 115200 };
    um98x_detect_out_t d;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_detect(&s->ex, cands, 1, &d));
    TEST_ASSERT_EQUAL_UINT32(115200, d.baud);
    TEST_ASSERT_FALSE(d.by_listen);
    TEST_ASSERT_FALSE(d.version_valid);
    TEST_ASSERT_EQUAL_INT(1, count(s, "VERSIONA"));
}

TEST_CASE("exec: a signal-group resend met by the restarting answer stops there and waits for the banner", "[um98x_exec]")
{
    /* The ack is lost in the restart; the resend 4 s later is answered with
     * the restarting notice: the change took effect, nothing more is sent
     * until the banner, and the restart is ours (no evidence). */
    sim_t *s = sim_init();
    s->notices = true;
    s->sg_no_ack = true;
    s->restart_ms = 9000;
    strcpy(s->saved_mode, "MODE BASE 1234 TIME 600 0");
    strcpy(s->mode, s->saved_mode);
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.signal_group_sent);
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_INT(2, count(s, "CONFIG SIGNALGROUP"));
    TEST_ASSERT_EQUAL_UINT8(3, s->sg_m);
    TEST_ASSERT_EQUAL_UINT64(0, s->ch.stats.restarts);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(1, s->ch.stats.resetting);
    TEST_ASSERT_EQUAL_UINT64(1, s->ch.stats.banners);
    TEST_ASSERT_EQUAL_INT(0, count(s, "RESET"));
    /* Nothing was sent between the notice and the banner. */
    int sg = nth(s, "CONFIG SIGNALGROUP 3 6", 1);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sg);
    TEST_ASSERT_EQUAL_STRING("VERSIONA", s->rx[sg + 1].cmd);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(s->rx[sg].at + 4000, s->rx[sg + 1].at);
}

TEST_CASE("exec: every session asks once what the group configures, after the signal-group step", "[um98x_exec]")
{
    sim_t *s = sim_init();
    um98x_base_profile_t p;
    default_profile(&p);
    gnss_versiona_t id;
    known_identity(&id);
    um98x_apply_result_t r;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(1, count(s, "ANT1GROUPA"));
    TEST_ASSERT_GREATER_THAN_INT(nth(s, "CONFIG SIGNALGROUP 3 6", 0), nth(s, "ANT1GROUPA", 0));
    TEST_ASSERT_LESS_THAN_INT(nth(s, "SAVECONFIG", 0), nth(s, "ANT1GROUPA", 0));
    TEST_ASSERT_TRUE(r.bands_asked);
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_OK, r.bands.state);
    TEST_ASSERT_EQUAL_UINT8(3, r.bands.group);   /* after the change: the new group */
    TEST_ASSERT_EQUAL_HEX32(0x0F, r.bands.mask[UM98X_BSYS_GALILEO]);
    TEST_ASSERT_EQUAL_STRING("", r.bands_reply);

    /* Nothing to change: still asked once. */
    configured(s);
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_INT(2, count(s, "ANT1GROUPA"));
    TEST_ASSERT_EQUAL_UINT8(3, r.bands.group);

    /* A firmware without the query refuses it: unknown, the session goes on. */
    s = sim_init();
    configured(s);
    s->groups = 1;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_REFUSED, r.bands.state);
    TEST_ASSERT_EQUAL_STRING(SIM_REFUSAL, r.bands_reply);
    TEST_ASSERT_EQUAL_INT(1, count(s, "ANT1GROUPA"));   /* a refusal is final at once */

    /* Accepted, but no log this firmware reads: unread after the wait, not resent. */
    s = sim_init();
    configured(s);
    s->groups = 2;
    int64_t t0 = s->now;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_TRUE(r.saved);
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_UNREAD, r.bands.state);
    TEST_ASSERT_EQUAL_INT(1, count(s, "ANT1GROUPA"));
    TEST_ASSERT_LESS_THAN_INT64(12000, s->now - t0);

    /* Its log a second and a half after the acknowledgement (the receiver's
     * next cycle; a 1000 ms wait missed one on the board): read. */
    s = sim_init();
    configured(s);
    s->groups_log_ms = 1500;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_OK, r.bands.state);
    TEST_ASSERT_EQUAL_UINT8(3, r.bands.group);
    TEST_ASSERT_EQUAL_STRING("", r.bands_reply);
    TEST_ASSERT_EQUAL_INT(1, count(s, "ANT1GROUPA"));

    /* A session that fails before the query never asked it. */
    s = sim_init();
    snprintf(s->reject_prefix, sizeof(s->reject_prefix), "CONFIG SIGNALGROUP");
    s->reject_n = 10;
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_apply(&s->ex, &p, NULL, &id, 115200, &r));
    TEST_ASSERT_FALSE(r.bands_asked);
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_UNKNOWN, r.bands.state);
    TEST_ASSERT_EQUAL_INT(0, count(s, "ANT1GROUPA"));
}

TEST_CASE("exec: the receiver's own position: either log, refused, or a log not read", "[um98x_exec]")
{
    static const char body[] = "SOL_COMPUTED,SINGLE,45.1000000,-75.2000000,80.000,-34.000,WGS84,1.200,1.100,2.300,"
                               "\"0\",0.000,0.000,40,32";
    sim_t *s = sim_init();
    configured(s);
    s->basepos = body;
    um98x_own_fix_t f;
    TEST_ASSERT_EQUAL(UM98X_OK, um98x_exec_own_fix(&s->ex, UM98X_CMD_BASEPOS, &f));
    TEST_ASSERT_TRUE(f.computed);
    TEST_ASSERT_EQUAL_UINT8(UM98X_OWN_POS_SINGLE, f.pos_type);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 45.1, f.lat_deg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -34.0, f.separation_m);
    TEST_ASSERT_EQUAL_UINT8(32, f.sats_used);
    TEST_ASSERT_TRUE(um98x_own_fix_usable(&f));
    TEST_ASSERT_EQUAL_INT(1, count(s, "BASEPOSA"));

    /* No such command: refused at once, not resent. */
    TEST_ASSERT_EQUAL(UM98X_E_REJECTED, um98x_exec_own_fix(&s->ex, UM98X_CMD_SPPNAV, &f));
    TEST_ASSERT_EQUAL_INT(1, count(s, "SPPNAVA"));
    TEST_ASSERT_FALSE(f.computed);

    /* A log whose body this firmware does not read. */
    s->sppnav = "SOL_COMPUTED,SINGLE,45.1";
    TEST_ASSERT_EQUAL(UM98X_E_FAILED, um98x_exec_own_fix(&s->ex, UM98X_CMD_SPPNAV, &f));
    TEST_ASSERT_EQUAL_INT(2, count(s, "SPPNAVA"));

    /* Only the two position logs. */
    TEST_ASSERT_EQUAL(UM98X_E_ARG, um98x_exec_own_fix(&s->ex, "BESTNAVA", &f));
    TEST_ASSERT_EQUAL_INT(0, count(s, "BESTNAVA"));
}
