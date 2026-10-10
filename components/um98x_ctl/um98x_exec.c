/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_exec.h"

#include <stdio.h>
#include <string.h>

#include "um98x_base.h"
#include "um98x_profile.h"

/* Loader menu item that restarts the receiver's application. */
#define LOADER_RESTART_KEY "6"

static void lock(um98x_exec_t *ex)
{
    ex->port->lock(ex->port->ctx);
}

static void unlock(um98x_exec_t *ex)
{
    ex->port->unlock(ex->port->ctx);
}

static int64_t now_ms(um98x_exec_t *ex)
{
    return ex->port->now_ms(ex->port->ctx);
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

static bool word_is(const char *s, const char *word)
{
    size_t n = strlen(word);
    if (strlen(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\0') {
            return false;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != prefix[i]) {
            return false;
        }
    }
    return true;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- channel ------------------------------------------------------------ */

void um98x_chan_init(um98x_chan_t *ch)
{
    memset(ch, 0, sizeof(*ch));
    ch->last_reply_ms = UM98X_NONE_MS;
    ch->last_answer_ms = UM98X_NONE_MS;
    ch->last_version_write_ms = UM98X_NONE_MS;
    ch->last_banner_ms = UM98X_NONE_MS;
    ch->last_restart_ms = UM98X_NONE_MS;
}

unsigned um98x_chan_dispatch(um98x_chan_t *ch, const char *line, size_t len, const um98x_line_t *l, int64_t now_ms)
{
    if (l->kind == UM98X_LINE_OTHER) {
        ch->stats.other++;
        return 0;
    }
    if (l->kind == UM98X_LINE_BAD) {
        ch->stats.bad++;
        return 0;
    }
    ch->stats.lines++;
    ch->last_answer_ms = now_ms;   /* the receiver answers */

    unsigned ev = 0;
    bool consumed = ch->txn_active && um98x_txn_on_line(&ch->txn, line, len, l, now_ms);
    if (consumed) {
        ch->stats.matched++;
        ch->last_reply_ms = now_ms;
        ev |= UM98X_CHAN_WAKE;
    }

    if (l->kind == UM98X_LINE_BANNER) {
        ch->stats.banners++;
        ch->last_banner_ms = now_ms;
        ev |= UM98X_CHAN_WAKE;   /* a restart wait may be watching for it */
        if (!consumed && !ch->expect_restart) {
            ch->stats.restarts++;
            ch->last_restart_ms = now_ms;
            ev |= UM98X_CHAN_RESTART;
        }
        return ev;
    }
    if (consumed) {
        return ev;
    }
    /* A version log nobody waits for: a late reply to our own query if we
     * wrote one recently, otherwise the receiver announced a restart. */
    if (l->kind == UM98X_LINE_VERSION && !ch->expect_restart &&
        (ch->last_version_write_ms == UM98X_NONE_MS ||
         now_ms - ch->last_version_write_ms > UM98X_UNSOLICITED_VERSION_MS)) {
        ch->stats.restarts++;
        ch->last_restart_ms = now_ms;
        return ev | UM98X_CHAN_RESTART;
    }
    ch->stats.unmatched++;
    return ev;
}

/* ---- transactions ------------------------------------------------------- */

um98x_res_t um98x_res_from_txn(um98x_txn_status_t st)
{
    switch (st) {
    case UM98X_TXN_OK:
    case UM98X_TXN_RESTARTED:
        return UM98X_OK;
    case UM98X_TXN_REJECTED:
        return UM98X_E_REJECTED;
    case UM98X_TXN_LINK_ERROR:
        return UM98X_E_LINK;
    default:
        return UM98X_E_TIMEOUT;
    }
}

um98x_txn_status_t um98x_exec_txn(um98x_exec_t *ex, um98x_txn_kind_t kind, const char *cmd,
                                  const um98x_txn_opts_t *opts)
{
    const um98x_port_t *pt = ex->port;
    um98x_chan_t *ch = ex->ch;

    lock(ex);
    int64_t now = now_ms(ex);
    /* Pacing: the next command goes out 100 ms after the receiver's last
     * reply line (acknowledgement, data, banner). */
    int64_t not_before = (ch->last_answer_ms == UM98X_NONE_MS) ? now : ch->last_answer_ms + UM98X_PACING_MS;
    um98x_txn_begin(&ch->txn, kind, cmd, not_before, opts != NULL ? opts->text : NULL,
                    opts != NULL ? opts->text_cap : 0);
    if (opts != NULL) {
        if (opts->timeout_ms != 0) {
            ch->txn.timeout_ms = opts->timeout_ms;
        }
        if (opts->max_sends != 0) {
            ch->txn.max_sends = opts->max_sends;
        }
        ch->txn.restart_ends = opts->restart_ends;
        if (kind == UM98X_TXN_VERSION_RESTART) {
            um98x_txn_set_end(&ch->txn, opts->end_ms);
        }
    }
    bool crlf = um98x_txn_leading_crlf(&ch->txn);
    ch->txn_active = true;
    unlock(ex);

    char out[UM98X_CMD_MAX + 4];
    int n = snprintf(out, sizeof(out), "%s%s\r\n", crlf ? "\r\n" : "", ch->txn.cmd);
    bool fits = n > 0 && (size_t)n < sizeof(out);

    for (;;) {
        int64_t wake = 0;
        /* Detection must stop sending as soon as the loader prompt shows. */
        bool stop = (kind == UM98X_TXN_PROBE) && pt->prompt_seen(pt->ctx);
        now = now_ms(ex);
        lock(ex);
        um98x_txn_action_t act = um98x_txn_poll(&ch->txn, now, &wake);
        if (act == UM98X_TXN_ACT_SEND && stop) {
            um98x_txn_abort(&ch->txn, now);
            act = UM98X_TXN_ACT_DONE;
        }
        if (act == UM98X_TXN_ACT_DONE) {
            ch->txn_active = false;
            um98x_txn_status_t st = ch->txn.status;
            unlock(ex);
            return st;
        }
        unlock(ex);

        if (act == UM98X_TXN_ACT_SEND) {
            bool ok = fits && pt->write(pt->ctx, out, (size_t)n);
            int64_t t = now_ms(ex);
            lock(ex);
            um98x_txn_sent(&ch->txn, t, ok);
            if (crlf) {
                ch->last_version_write_ms = t;
            }
            unlock(ex);
            continue;
        }
        pt->wait(pt->ctx, wake);
    }
}

um98x_res_t um98x_exec_command(um98x_exec_t *ex, const char *cmd, uint32_t timeout_ms, char *text, size_t cap)
{
    if (cmd == NULL || cmd[0] == '\0') {
        return UM98X_E_ARG;
    }
    um98x_txn_kind_t kind = UM98X_TXN_COMMAND;
    if (word_is(cmd, UM98X_CMD_VERSION)) {
        kind = UM98X_TXN_VERSION;
    } else if (word_is(cmd, UM98X_CMD_MODE)) {
        kind = UM98X_TXN_MODE;
    } else if (word_is(cmd, UM98X_CMD_CONFIG)) {
        kind = UM98X_TXN_CONFIG;
    }
    um98x_txn_opts_t o = {
        .timeout_ms = (kind == UM98X_TXN_VERSION) ? 0 : timeout_ms,   /* the version query keeps its own timing */
        .restart_ends = starts_with_ci(cmd, "CONFIG SIGNALGROUP") || word_is(cmd, UM98X_CMD_RESET),
        .text = text,
        .text_cap = cap,
    };
    return um98x_res_from_txn(um98x_exec_txn(ex, kind, cmd, &o));
}

static um98x_res_t version_query(um98x_exec_t *ex, um98x_txn_kind_t kind, uint8_t tries, int64_t end_ms,
                                 gnss_versiona_t *out, bool *ack_seen)
{
    um98x_txn_opts_t o = { .max_sends = tries, .end_ms = end_ms };
    um98x_txn_status_t st = um98x_exec_txn(ex, kind, UM98X_CMD_VERSION, &o);
    lock(ex);
    if (ack_seen != NULL) {
        *ack_seen = ex->ch->txn.ack_seen;
    }
    if (out != NULL) {
        if (st == UM98X_TXN_OK && ex->ch->txn.version_valid) {
            *out = ex->ch->txn.version;
        } else {
            memset(out, 0, sizeof(*out));
        }
    }
    unlock(ex);
    return um98x_res_from_txn(st);
}

um98x_res_t um98x_exec_version(um98x_exec_t *ex, bool after_restart, int64_t end_ms, gnss_versiona_t *out,
                               bool *ack_seen)
{
    return version_query(ex, after_restart ? UM98X_TXN_VERSION_RESTART : UM98X_TXN_VERSION, 0, end_ms, out,
                         ack_seen);
}

um98x_res_t um98x_exec_query_mode(um98x_exec_t *ex, char *body, size_t cap)
{
    um98x_txn_status_t st = um98x_exec_txn(ex, UM98X_TXN_MODE, UM98X_CMD_MODE, NULL);
    if (body != NULL && cap > 0) {
        lock(ex);
        copy_str(body, cap, st == UM98X_TXN_OK ? ex->ch->txn.mode_body : "");
        unlock(ex);
    }
    return um98x_res_from_txn(st);
}

/* ---- raw writes and waits ----------------------------------------------- */

/* One line without a transaction (no reply expected or awaited). */
static bool raw_line(um98x_exec_t *ex, const char *text)
{
    char out[UM98X_CMD_MAX + 4];
    int n = snprintf(out, sizeof(out), "%s\r\n", text);
    return n > 0 && (size_t)n < sizeof(out) && ex->port->write(ex->port->ctx, out, (size_t)n);
}

static void sleep_until(um98x_exec_t *ex, int64_t until_ms)
{
    for (;;) {
        int64_t now = now_ms(ex);
        if (now >= until_ms) {
            return;
        }
        ex->port->wait(ex->port->ctx, until_ms);
    }
}

static void sleep_ms(um98x_exec_t *ex, int64_t ms)
{
    sleep_until(ex, now_ms(ex) + ms);
}

/* Pacing for writes outside a transaction: 100 ms after the receiver's
 * last reply line. */
static void pace(um98x_exec_t *ex)
{
    lock(ex);
    int64_t last = ex->ch->last_answer_ms;
    unlock(ex);
    if (last != UM98X_NONE_MS) {
        sleep_until(ex, last + UM98X_PACING_MS);
    }
}

static uint64_t banner_count(um98x_exec_t *ex)
{
    lock(ex);
    uint64_t b = ex->ch->stats.banners;
    unlock(ex);
    return b;
}

static bool wait_banner(um98x_exec_t *ex, uint64_t before, int64_t until_ms)
{
    for (;;) {
        if (banner_count(ex) != before) {
            return true;
        }
        if (now_ms(ex) >= until_ms) {
            return false;
        }
        ex->port->wait(ex->port->ctx, until_ms);
    }
}

static void set_expect_restart(um98x_exec_t *ex, bool on)
{
    lock(ex);
    ex->ch->expect_restart = on;
    unlock(ex);
}

/* ---- detection ---------------------------------------------------------- */

um98x_res_t um98x_exec_detect(um98x_exec_t *ex, const uint32_t *cands, size_t n, um98x_detect_out_t *out)
{
    const um98x_port_t *pt = ex->port;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < n; i++) {
        if (pt->prompt_seen(pt->ctx)) {
            return UM98X_E_PROMPT;
        }
        uint32_t baud = cands[i];
        if (pt->get_baud(pt->ctx) != baud && !pt->set_baud(pt->ctx, baud)) {
            return UM98X_E_LINK;
        }

        /* Listen first: a streaming receiver is found without sending anything. */
        um98x_link_counts_t c0;
        um98x_link_counts_t c;
        pt->counts(pt->ctx, &c0);
        int64_t until = now_ms(ex) + UM98X_LISTEN_MS;
        bool found = false;
        for (;;) {
            pt->counts(pt->ctx, &c);
            if (c.rx_bytes != c0.rx_bytes) {
                out->saw_bytes = true;
            }
            if (c.rtcm_frames - c0.rtcm_frames >= 1 || c.text_lines - c0.text_lines >= 2) {
                found = true;
                break;
            }
            if (pt->prompt_seen(pt->ctx)) {
                return UM98X_E_PROMPT;
            }
            int64_t now = now_ms(ex);
            if (now >= until) {
                break;
            }
            pt->wait(pt->ctx, min64(until, now + UM98X_LISTEN_POLL_MS));
        }
        if (found) {
            out->baud = baud;
            out->by_listen = true;
            return UM98X_OK;
        }

        /* Probe: the version query; any acknowledgement or the log will do. */
        um98x_txn_status_t st = um98x_exec_txn(ex, UM98X_TXN_PROBE, UM98X_CMD_VERSION, NULL);
        pt->counts(pt->ctx, &c);
        if (c.rx_bytes != c0.rx_bytes) {
            out->saw_bytes = true;
        }
        if (pt->prompt_seen(pt->ctx)) {
            return UM98X_E_PROMPT;
        }
        if (st == UM98X_TXN_OK) {
            out->baud = baud;
            lock(ex);
            if (ex->ch->txn.version_valid) {
                out->version_valid = true;
                out->version = ex->ch->txn.version;
            }
            unlock(ex);
            return UM98X_OK;
        }
        if (st == UM98X_TXN_LINK_ERROR) {
            return UM98X_E_LINK;
        }
    }
    return UM98X_E_NOT_FOUND;
}

um98x_res_t um98x_exec_leave_loader(um98x_exec_t *ex, uint32_t timeout_ms)
{
    const um98x_port_t *pt = ex->port;
    um98x_link_counts_t c0;
    um98x_link_counts_t c;
    pt->counts(pt->ctx, &c0);
    uint64_t banners0 = banner_count(ex);
    if (!raw_line(ex, LOADER_RESTART_KEY)) {
        return UM98X_E_LINK;
    }
    int64_t until = now_ms(ex) + timeout_ms;
    for (;;) {
        pt->counts(pt->ctx, &c);
        if (c.rtcm_frames - c0.rtcm_frames >= 1 || c.text_lines - c0.text_lines >= 2 ||
            banner_count(ex) != banners0) {
            return UM98X_OK;
        }
        int64_t now = now_ms(ex);
        if (now >= until) {
            return UM98X_E_TIMEOUT;
        }
        pt->wait(pt->ctx, min64(until, now + UM98X_LISTEN_POLL_MS));
    }
}

/* ---- apply session ------------------------------------------------------ */

typedef struct {
    um98x_exec_t *ex;
    const um98x_base_profile_t *p;
    um98x_apply_result_t *r;
    uint32_t stored_baud;  /* last working baud (settings): head of a detection inside the session */
    uint32_t saved_baud;   /* COM1 baud the receiver comes back at after a restart */
    bool accepted;         /* something was configured: roll back on failure */
    um98x_readback_t rb;
    bool settings_read;    /* a dump was read in this session */
    um98x_rx_settings_t settings;   /* from the latest dump */
} session_t;

/* Record the first failure of the session. */
static void fail(session_t *s, const char *cmd, const char *reason)
{
    if (s->r->failed_cmd[0] != '\0' || s->r->reason[0] != '\0') {
        return;
    }
    copy_str(s->r->failed_cmd, sizeof(s->r->failed_cmd), cmd);
    copy_str(s->r->reason, sizeof(s->r->reason), reason);
}

static void fail_txn(session_t *s, const char *cmd, um98x_txn_status_t st)
{
    const char *reason;
    char buf[UM98X_REASON_MAX];
    switch (st) {
    case UM98X_TXN_REJECTED:
        lock(s->ex);
        copy_str(buf, sizeof(buf), s->ex->ch->txn.reason[0] != '\0' ? s->ex->ch->txn.reason : "rejected");
        unlock(s->ex);
        reason = buf;
        break;
    case UM98X_TXN_LINK_ERROR:
        reason = "link error";
        break;
    default:
        reason = "timeout";
        break;
    }
    fail(s, cmd, reason);
}

/* Text for a step that did not succeed: the receiver's reply, or why not. */
static void txn_reason(um98x_exec_t *ex, um98x_txn_status_t st, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    switch (st) {
    case UM98X_TXN_OK:
        out[0] = '\0';
        break;
    case UM98X_TXN_REJECTED:
        lock(ex);
        copy_str(out, cap, ex->ch->txn.reason[0] != '\0' ? ex->ch->txn.reason : "rejected");
        unlock(ex);
        break;
    case UM98X_TXN_RESTARTED:
        copy_str(out, cap, "receiver restarted");
        break;
    case UM98X_TXN_LINK_ERROR:
        copy_str(out, cap, "link error");
        break;
    default:
        copy_str(out, cap, "timeout");
        break;
    }
}

/* One configuring command; record = a failure is the session's failure. */
static um98x_res_t step(session_t *s, um98x_txn_kind_t kind, const char *cmd, bool restart_ends, bool record,
                        um98x_txn_status_t *st_out)
{
    um98x_txn_opts_t o = { .restart_ends = restart_ends };
    um98x_txn_status_t st = um98x_exec_txn(s->ex, kind, cmd, &o);
    if (st_out != NULL) {
        *st_out = st;
    }
    um98x_res_t res = um98x_res_from_txn(st);
    if (res != UM98X_OK && record) {
        fail_txn(s, cmd, st);
    }
    return res;
}

static int64_t txn_done_ms(um98x_exec_t *ex)
{
    lock(ex);
    int64_t t = ex->ch->txn.done_ms;
    unlock(ex);
    return t;
}

/* CONFIG dump (fatal) and base-mode log (not fatal: unread = different). */
static um98x_res_t read_back(session_t *s)
{
    memset(&s->rb, 0, sizeof(s->rb));
    if (step(s, UM98X_TXN_CONFIG, UM98X_CMD_CONFIG, false, true, NULL) != UM98X_OK) {
        return UM98X_E_FAILED;
    }
    lock(s->ex);
    s->rb.config_ok = true;
    s->rb.config = s->ex->ch->txn.config;
    unlock(s->ex);
    s->settings = s->rb.config.rx;   /* the latest dump wins */
    s->settings_read = true;

    um98x_txn_status_t st = um98x_exec_txn(s->ex, UM98X_TXN_MODE, UM98X_CMD_MODE, NULL);
    if (st == UM98X_TXN_OK) {
        lock(s->ex);
        s->rb.mode_ok = true;
        copy_str(s->rb.mode_body, sizeof(s->rb.mode_body), s->ex->ch->txn.mode_body);
        unlock(s->ex);
    } else if (st == UM98X_TXN_LINK_ERROR) {
        fail_txn(s, UM98X_CMD_MODE, st);
        return UM98X_E_FAILED;
    }
    return UM98X_OK;
}

static um98x_res_t find_receiver(session_t *s, uint32_t first_baud, bool full, uint32_t *found)
{
    uint32_t cands[UM98X_DETECT_MAX];
    size_t n = um98x_detect_candidates(first_baud, full, cands, UM98X_DETECT_MAX);
    um98x_detect_out_t d;
    um98x_res_t res = um98x_exec_detect(s->ex, cands, n, &d);
    if (res == UM98X_OK) {
        *found = d.baud;
        s->r->found_baud = d.baud;
        s->r->answer_baud = d.baud;
    }
    return res;
}

/* CONFIG SIGNALGROUP: the receiver restarts (and saves the value itself). */
static um98x_res_t signal_group(session_t *s)
{
    um98x_exec_t *ex = s->ex;
    const um98x_step_t sg = { UM98X_STEP_SIGNALGROUP, true, 0, 0 };
    char cmd[UM98X_CMD_MAX];
    um98x_step_command(s->p, &sg, cmd, sizeof(cmd));

    set_expect_restart(ex, true);
    uint64_t banners0 = banner_count(ex);
    um98x_txn_status_t st;
    (void)step(s, UM98X_TXN_COMMAND, cmd, true, false, &st);
    if (st == UM98X_TXN_REJECTED || st == UM98X_TXN_LINK_ERROR) {
        fail_txn(s, cmd, st);
        set_expect_restart(ex, false);
        return UM98X_E_FAILED;
    }
    s->accepted = true;
    s->r->signal_group_sent = true;

    /* Banner (at most 15 s after the acknowledgement), then the version
     * polled every second until 20 s after the restart evidence. No
     * acknowledgement at all (it may be lost in the restart itself, and a
     * receiver back at another baud answers none of the resends): the same
     * as a restart nothing was heard of, timed from the first send. */
    int64_t t_ack = txn_done_ms(ex);
    if (st == UM98X_TXN_TIMEOUT) {
        lock(ex);
        t_ack = ex->ch->txn.first_sent_ms;
        unlock(ex);
    }
    bool banner = (st == UM98X_TXN_RESTARTED) || wait_banner(ex, banners0, t_ack + UM98X_RESTART_BANNER_MS);
    int64_t end = t_ack + UM98X_RESTART_LOST_MS;
    if (banner) {
        lock(ex);
        end = ex->ch->last_banner_ms + UM98X_VERSION_RESTART_SPAN_MS;
        unlock(ex);
    }
    um98x_res_t res = um98x_exec_version(ex, true, end, NULL, NULL);
    set_expect_restart(ex, false);
    if (res != UM98X_OK) {
        /* It came back at its saved baud, which this session was not
         * running at: look for it without leaving the session. */
        uint32_t baud = 0;
        if (find_receiver(s, s->stored_baud, true, &baud) != UM98X_OK) {
            fail(s, cmd, "receiver lost after signal-group reset");
            s->r->receiver_lost = true;
            return UM98X_E_LOST;
        }
        s->saved_baud = baud;
    }

    /* The restart brought the saved outputs back: quiet the port again,
     * then the read-back must show the new signal group. */
    if (step(s, UM98X_TXN_COMMAND, UM98X_CMD_UNLOG_COM1, false, true, NULL) != UM98X_OK) {
        return UM98X_E_FAILED;
    }
    if (read_back(s) != UM98X_OK) {
        return UM98X_E_FAILED;
    }
    if (!um98x_signal_group_matches(s->p, &s->rb)) {
        fail(s, cmd, "signal group rejected");
        return UM98X_E_FAILED;
    }
    return UM98X_OK;
}

static um98x_res_t version_tries(um98x_exec_t *ex, uint8_t tries)
{
    return version_query(ex, UM98X_TXN_VERSION, tries, 0, NULL, NULL);
}

/* Host to `baud`, a moment to settle, then the version within its tries. */
static bool answers_at(um98x_exec_t *ex, uint32_t baud)
{
    const um98x_port_t *pt = ex->port;
    if (pt->get_baud(pt->ctx) != baud && !pt->set_baud(pt->ctx, baud)) {
        return false;
    }
    sleep_ms(ex, UM98X_BAUD_VERIFY_DELAY_MS);
    return version_tries(ex, UM98X_BAUD_VERIFY_TRIES) == UM98X_OK;
}

/* COM1 is the link itself, so its acknowledgement cannot be relied on: verify
 * at the new baud instead. Silent there: look at the old baud first. A
 * receiver that answers there never switched, so the command goes out once
 * more before the step fails; only when both bauds are silent is it told to
 * go back (at the new baud). Never saved on the failure path. */
static um98x_res_t baud_change(session_t *s)
{
    um98x_exec_t *ex = s->ex;
    const um98x_port_t *pt = ex->port;
    uint32_t old = pt->get_baud(pt->ctx);
    char cmd[UM98X_CMD_MAX];
    snprintf(cmd, sizeof(cmd), "CONFIG COM1 %u", (unsigned)s->p->com1_baud);

    for (unsigned attempt = 0; attempt < UM98X_BAUD_ATTEMPTS; attempt++) {
        pace(ex);
        if (!raw_line(ex, cmd)) {
            fail(s, cmd, "link error");
            return UM98X_E_FAILED;
        }
        s->accepted = true;   /* the receiver may have switched */
        sleep_ms(ex, UM98X_BAUD_SETTLE_MS);
        if (!pt->set_baud(pt->ctx, s->p->com1_baud)) {
            fail(s, cmd, "host baud switch failed");
            return UM98X_E_FAILED;
        }
        sleep_ms(ex, UM98X_BAUD_VERIFY_DELAY_MS);
        if (version_tries(ex, UM98X_BAUD_VERIFY_TRIES) == UM98X_OK) {
            s->r->baud_changed = true;
            return UM98X_OK;
        }
        if (!answers_at(ex, old)) {
            break;   /* silent at both bauds */
        }
        /* Still at the old baud: the command did not take. */
        if (attempt + 1 >= UM98X_BAUD_ATTEMPTS) {
            fail(s, cmd, "receiver stays at the old baud");
            return UM98X_E_FAILED;
        }
    }
    fail(s, cmd, "no answer at the new baud");

    /* Back to the old baud, sent at the new one. */
    char back[UM98X_CMD_MAX];
    snprintf(back, sizeof(back), "CONFIG COM1 %u", (unsigned)old);
    if (pt->get_baud(pt->ctx) != s->p->com1_baud) {
        (void)pt->set_baud(pt->ctx, s->p->com1_baud);
    }
    pace(ex);
    (void)raw_line(ex, back);
    sleep_ms(ex, UM98X_BAUD_SETTLE_MS);
    if (answers_at(ex, old)) {
        return UM98X_E_FAILED;
    }
    uint32_t baud = 0;
    if (find_receiver(s, old, false, &baud) == UM98X_OK) {
        return UM98X_E_FAILED;
    }
    s->r->receiver_lost = true;
    return UM98X_E_LOST;
}

/*
 * RESET, written once, then the restart timeline: listen only (nothing is
 * sent) until the restart banner or t0 + 15 s, then the version in restart
 * mode (every second) until banner + 20 s, or t0 + 20 s without a banner.
 * t0 is the end of the RESET write. listen_baud: the host switches there
 * right after the write (0 = stays). *sent: the RESET went out. The caller
 * marks the restart as its own (expect_restart).
 */
static um98x_res_t reset_wait(um98x_exec_t *ex, uint32_t listen_baud, bool *sent)
{
    const um98x_port_t *pt = ex->port;
    *sent = false;
    uint64_t banners0 = banner_count(ex);
    pace(ex);
    if (!pt->write(pt->ctx, "\r\n", 2) || !raw_line(ex, UM98X_CMD_RESET)) {
        return UM98X_E_LINK;
    }
    *sent = true;
    int64_t t0 = now_ms(ex);
    if (listen_baud != 0 && pt->get_baud(pt->ctx) != listen_baud) {
        (void)pt->set_baud(pt->ctx, listen_baud);
    }
    int64_t end = t0 + UM98X_RESTART_LOST_MS;
    if (wait_banner(ex, banners0, t0 + UM98X_RESTART_BANNER_MS)) {
        lock(ex);
        end = ex->ch->last_banner_ms + UM98X_VERSION_RESTART_SPAN_MS;
        unlock(ex);
    }
    return um98x_exec_version(ex, true, end, NULL, NULL);
}

/* Undo an unsaved session: a plain restart brings back the saved
 * configuration (and the saved baud), with the same timeline as the
 * recovery RESET; nothing at the saved baud: 10 s more at the baud used
 * before the RESET. */
static void rollback(session_t *s)
{
    um98x_exec_t *ex = s->ex;
    const um98x_port_t *pt = ex->port;
    set_expect_restart(ex, true);
    uint32_t before = pt->get_baud(pt->ctx);
    s->r->rolled_back = true;
    bool sent = false;
    um98x_res_t res = reset_wait(ex, s->saved_baud, &sent);
    if (sent && res != UM98X_OK) {
        /* Whether a plain restart drops an unsaved baud is not known:
         * listen and ask at the baud used before the RESET as well. */
        if (pt->get_baud(pt->ctx) != before) {
            (void)pt->set_baud(pt->ctx, before);
        }
        res = um98x_exec_version(ex, true, now_ms(ex) + UM98X_ROLLBACK_PREVIOUS_MS, NULL, NULL);
    }
    s->r->rollback_answered = (res == UM98X_OK);
    /* After the RESET only an answer says where the receiver is. */
    s->r->answer_baud = (res == UM98X_OK) ? pt->get_baud(pt->ctx) : 0;
    set_expect_restart(ex, false);
}

static void unsupported(session_t *s, const char *model)
{
    char why[UM98X_APPLY_REASON_MAX];
    snprintf(why, sizeof(why), "unsupported model %.32s", model);
    fail(s, "", why);
}

/* A restart banner since banners0 (the start of the configuring steps): the
 * receiver came back with its saved configuration, so whatever was sent
 * unsaved (base mode, antenna, outputs) is gone and a save now would store
 * the old base mode with our outputs. The session fails; after the B1C/B2a
 * command (b1c_cmd set) with that command and reason, as its outcome shows. */
static bool restarted_since(session_t *s, uint64_t banners0, const char *b1c_cmd)
{
    if (banner_count(s->ex) == banners0) {
        return false;
    }
    if (b1c_cmd[0] != '\0') {
        s->r->b1cb2a_ok = false;
        copy_str(s->r->b1cb2a_reply, sizeof(s->r->b1cb2a_reply), "receiver restarted");
        fail(s, b1c_cmd, "receiver restarted after RTCMB1CB2A");
    } else {
        fail(s, "", "receiver restarted during the session");
    }
    return true;
}

static um98x_res_t run_plan(session_t *s, const um98x_apply_opts_t *opts, gnss_versiona_t *identity, bool queried)
{
    um98x_exec_t *ex = s->ex;
    const um98x_port_t *pt = ex->port;
    bool force_mode = opts != NULL && opts->force_mode;

    /* Quiet port first (the station's caster pause covers the gap). */
    if (step(s, UM98X_TXN_COMMAND, UM98X_CMD_UNLOG_COM1, false, true, NULL) != UM98X_OK) {
        return UM98X_E_FAILED;
    }
    s->accepted = true;   /* the output is silenced now */

    /* The applied state records this session's firmware: an identity known
     * from earlier may predate a receiver update. Read on the quiet port. */
    if (!queried) {
        gnss_versiona_t v;
        if (um98x_exec_version(ex, false, 0, &v, NULL) != UM98X_OK) {
            fail(s, UM98X_CMD_VERSION, "no version log");
            return UM98X_E_FAILED;
        }
        *identity = v;
        if (!um98x_model_supported(v.model)) {
            unsupported(s, v.model);
            return UM98X_E_FAILED;
        }
    }
    copy_str(s->r->firmware, sizeof(s->r->firmware), identity->firmware);

    if (read_back(s) != UM98X_OK) {
        return UM98X_E_FAILED;
    }

    um98x_plan_t plan;
    um98x_plan_build(s->p, &s->rb, pt->get_baud(pt->ctx), force_mode, &plan);
    if (plan.count > 0 && plan.step[0].kind == UM98X_STEP_SIGNALGROUP) {
        um98x_res_t res = signal_group(s);
        if (res != UM98X_OK) {
            return res;
        }
        um98x_plan_build(s->p, &s->rb, pt->get_baud(pt->ctx), force_mode, &plan);
    }

    /* No restart may come between here and the SAVECONFIG ack (see
     * restarted_since). Whether the B1C/B2a command restarts the receiver is
     * not known (PROVISIONAL, to be confirmed on hardware); its ack may come
     * before the banner ("OK, then a receiver reset"), so the banner count
     * is compared again before and after the save. */
    const uint64_t plan_banners = banner_count(ex);
    char b1c_cmd[UM98X_CMD_MAX] = "";   /* the B1C/B2a command once sent */

    for (size_t i = 0; i < plan.count; i++) {
        const um98x_step_t *st = &plan.step[i];
        char cmd[UM98X_CMD_MAX];
        if (um98x_step_command(s->p, st, cmd, sizeof(cmd)) == 0) {
            fail(s, "", "command does not fit");
            return UM98X_E_FAILED;
        }
        um98x_res_t res;
        switch ((um98x_step_kind_t)st->kind) {
        case UM98X_STEP_SIGNALGROUP:
            fail(s, cmd, "signal group rejected");
            return UM98X_E_FAILED;
        case UM98X_STEP_BAUD:
            res = baud_change(s);
            if (res != UM98X_OK) {
                return res;
            }
            break;
        case UM98X_STEP_MODE:
            if (step(s, UM98X_TXN_COMMAND, cmd, false, true, NULL) != UM98X_OK) {
                return UM98X_E_FAILED;
            }
            s->r->mode_sent = true;
            s->r->mode_ack_ms = txn_done_ms(ex);
            break;
        case UM98X_STEP_SAVE:
            if (restarted_since(s, plan_banners, b1c_cmd)) {
                return UM98X_E_FAILED;   /* nothing saved: the rollback brings back the saved state */
            }
            if (step(s, UM98X_TXN_COMMAND, cmd, false, true, NULL) != UM98X_OK) {
                return UM98X_E_FAILED;
            }
            if (restarted_since(s, plan_banners, b1c_cmd)) {
                /* The banner came while SAVECONFIG was in flight: what it
                 * stored may be the restarted state. Not counted as applied. */
                return UM98X_E_FAILED;
            }
            s->r->saved = true;
            break;
        case UM98X_STEP_ANTENNA: {
            /* Not fatal: a rejection is recorded and shown, the session goes on. */
            um98x_txn_status_t tst;
            (void)step(s, UM98X_TXN_COMMAND, cmd, false, false, &tst);
            s->r->antenna_sent = true;
            s->r->antenna_ok = (tst == UM98X_TXN_OK);
            txn_reason(ex, tst, s->r->antenna_reply, sizeof(s->r->antenna_reply));
            break;
        }
        case UM98X_STEP_B1CB2A: {
            /* Not fatal either. A restart banner while it is in flight, or
             * after its ack until SAVECONFIG is acknowledged (the check
             * around the save), ends the session as failed (rollback as for
             * any failure). */
            um98x_txn_status_t tst;
            copy_str(b1c_cmd, sizeof(b1c_cmd), cmd);
            (void)step(s, UM98X_TXN_COMMAND, cmd, true, false, &tst);
            s->r->b1cb2a_sent = true;
            s->r->b1cb2a_ok = (tst == UM98X_TXN_OK);
            txn_reason(ex, tst, s->r->b1cb2a_reply, sizeof(s->r->b1cb2a_reply));
            if (tst == UM98X_TXN_RESTARTED) {
                fail(s, cmd, "receiver restarted after RTCMB1CB2A");
                return UM98X_E_FAILED;
            }
            break;
        }
        default:   /* RTCM outputs and the satellite view */
            res = step(s, UM98X_TXN_COMMAND, cmd, false, st->fatal, NULL);
            if (st->kind == UM98X_STEP_RTCM && st->msg == UM98X_RTCM_OPTIONAL) {
                s->r->rtcm1230_ok = (res == UM98X_OK);
            }
            if (res != UM98X_OK && st->fatal) {
                return UM98X_E_FAILED;
            }
            break;
        }
    }
    return UM98X_OK;
}

um98x_res_t um98x_exec_apply(um98x_exec_t *ex, const um98x_base_profile_t *p, const um98x_apply_opts_t *opts,
                             gnss_versiona_t *identity, uint32_t stored_baud, um98x_apply_result_t *r)
{
    if (ex == NULL || r == NULL || identity == NULL) {
        return UM98X_E_ARG;
    }
    memset(r, 0, sizeof(*r));
    const um98x_port_t *pt = ex->port;
    r->baud = pt->get_baud(pt->ctx);
    if (!um98x_profile_valid(p)) {
        copy_str(r->reason, sizeof(r->reason), "invalid profile");
        return UM98X_E_ARG;
    }
    session_t s = { .ex = ex, .p = p, .r = r, .stored_baud = stored_baud, .saved_baud = r->baud, .accepted = false };

    /* Terminate whatever partial line the receiver may hold. */
    if (!pt->write(pt->ctx, "\r\n", 2)) {
        fail(&s, "", "link error");
        return UM98X_E_LINK;
    }

    /* Identity: only the supported model is configured. Unknown: asked
     * before anything is configured. */
    bool queried = false;
    if (identity->model[0] == '\0') {
        um98x_res_t res = um98x_exec_version(ex, false, 0, identity, NULL);
        if (res != UM98X_OK) {
            fail(&s, UM98X_CMD_VERSION, res == UM98X_E_LINK ? "link error" : "no version log");
            return res == UM98X_E_LINK ? UM98X_E_LINK : UM98X_E_TIMEOUT;
        }
        queried = true;
    }
    if (!um98x_model_supported(identity->model)) {
        unsupported(&s, identity->model);
        return UM98X_E_UNSUPPORTED;
    }

    um98x_res_t res = run_plan(&s, opts, identity, queried);
    if (res != UM98X_OK && res != UM98X_E_LOST && s.accepted) {
        rollback(&s);
    }
    r->baud = pt->get_baud(pt->ctx);
    if (s.settings_read) {
        r->settings_read = true;
        r->settings = s.settings;
        if (r->saved && r->antenna_ok) {
            /* The receiver now holds the profile's antenna. */
            um98x_rx_settings_t *x = &r->settings;
            size_t nm = um98x_antenna_str_len(p->antenna_model);
            size_t ns = um98x_antenna_str_len(p->antenna_serial);
            memset(x->antenna_model, 0, sizeof(x->antenna_model));
            memset(x->antenna_serial, 0, sizeof(x->antenna_serial));
            memcpy(x->antenna_model, p->antenna_model, nm);
            memcpy(x->antenna_serial, p->antenna_serial, ns);
            x->antenna_setup_id = p->antenna_setup_id;
            x->antenna_known = true;
        }
        if (r->saved && r->b1cb2a_ok) {
            r->settings.b1cb2a = UM98X_SETTING_ON;
        }
    }
    if (res == UM98X_OK || res == UM98X_E_LOST) {
        return res;
    }
    return UM98X_E_FAILED;
}

um98x_res_t um98x_exec_reset(um98x_exec_t *ex)
{
    /* Our own restart: its banner and the version log after it are not
     * restart evidence. */
    set_expect_restart(ex, true);
    bool sent = false;
    um98x_res_t res = reset_wait(ex, 0, &sent);
    set_expect_restart(ex, false);
    if (!sent) {
        return UM98X_E_LINK;
    }
    return res == UM98X_OK ? UM98X_OK : UM98X_E_TIMEOUT;
}

um98x_res_t um98x_exec_restore_rtcm(um98x_exec_t *ex, const um98x_base_profile_t *p, um98x_apply_result_t *r)
{
    if (ex == NULL || r == NULL) {
        return UM98X_E_ARG;
    }
    memset(r, 0, sizeof(*r));
    const um98x_port_t *pt = ex->port;
    r->baud = pt->get_baud(pt->ctx);
    um98x_plan_t plan;
    if (!um98x_plan_rtcm_only(p, &plan)) {
        copy_str(r->reason, sizeof(r->reason), "invalid profile");
        return UM98X_E_ARG;
    }
    session_t s = { .ex = ex, .p = p, .r = r, .stored_baud = r->baud, .saved_baud = r->baud, .accepted = false };
    if (!pt->write(pt->ctx, "\r\n", 2)) {
        fail(&s, "", "link error");
        return UM98X_E_LINK;
    }
    um98x_res_t out = UM98X_OK;
    for (size_t i = 0; i < plan.count; i++) {
        const um98x_step_t *st = &plan.step[i];
        char cmd[UM98X_CMD_MAX];
        if (um98x_step_command(p, st, cmd, sizeof(cmd)) == 0) {
            continue;
        }
        um98x_res_t res = step(&s, UM98X_TXN_COMMAND, cmd, false, st->fatal, NULL);
        if (st->msg == UM98X_RTCM_OPTIONAL) {
            r->rtcm1230_ok = (res == UM98X_OK);
        }
        if (res != UM98X_OK && st->fatal) {
            out = UM98X_E_FAILED;   /* keep going: every output that works helps */
        }
    }
    return out;
}

um98x_res_t um98x_exec_sat_view(um98x_exec_t *ex, bool on, uint16_t period_s, char *reason, size_t cap)
{
    if (reason != NULL && cap > 0) {
        reason[0] = '\0';
    }
    char cmd[UM98X_CMD_MAX];
    if (ex == NULL || um98x_sat_view_command(on, period_s, cmd, sizeof(cmd)) == 0) {
        return UM98X_E_ARG;
    }
    /* Terminate whatever partial line the receiver may hold. */
    if (!ex->port->write(ex->port->ctx, "\r\n", 2)) {
        if (reason != NULL && cap > 0) {
            copy_str(reason, cap, "link error");
        }
        return UM98X_E_LINK;
    }
    /* Fewer sends than a configuring step: the lease holds RTCM back, and
     * one toggle without an answer must stay under a caster's stall limit. */
    um98x_txn_opts_t o = { .max_sends = UM98X_SAT_VIEW_SENDS };
    um98x_txn_status_t st = um98x_exec_txn(ex, UM98X_TXN_COMMAND, cmd, &o);
    txn_reason(ex, st, reason, cap);
    return um98x_res_from_txn(st);
}
