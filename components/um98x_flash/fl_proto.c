/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fl_proto.h"

#include <stdio.h>
#include <string.h>

static const char *const k_str[FL_S_COUNT] = {
    [FL_S_PROMPT] = "boot>",
    [FL_S_DEVICENAME] = "$devicename",
    [FL_S_READY] = "download to",
    [FL_S_BEGIN] = "begin to update flash",
    [FL_S_INVALID] = "Invalid input",
    [FL_S_TOO_LONG] = "input is too long",
    [FL_S_INPUT_TIMEOUT] = "Get input timeout",
    [FL_S_SUBMENU] = "Choose firmware for update",
    [FL_S_ABORTED] = "## Binary download aborted",
    [FL_S_RESETTING] = "resetting the cpu",
    [FL_S_BACKUP_OK] = "backup succeed",
    [FL_S_RECOVER_WARN] = "recover warinnig",
    [FL_S_UART_BAUD] = "uart baudrate:",
    [FL_S_TIMEOUT_CFG] = "Timeout:",
    [FL_S_DEFAULT_ACTION] = "Default action:",
    [FL_S_LAST_MENU] = "efuse from uart",
    [FL_S_GGA] = "GGA",
    [FL_S_GSV] = "GSV",
    [FL_S_RMC] = "RMC",
    [FL_S_ERR_PKG_HEADER] = "wrong pkg header",
    [FL_S_ERR_SUM32] = "sum32 check err",
    [FL_S_ERR_DECOMP] = "decompression failed",
    [FL_S_ERR_LZOP] = "lzop_decompress_block Error",
    [FL_S_ERR_SIG] = "signature failed",
    [FL_S_ERR_SIG2] = "Signature Check Failed",
    [FL_S_ERR_AUTH] = "authcode check err",
    [FL_S_ERR_BACKUP] = "backup fail",
    [FL_S_ERR_LOAD] = "fail to load",
    [FL_S_ERR_BOOTCFG] = "fail to update boot config",
    [FL_S_ERR_VERSION] = "version is different",
    [FL_S_ERR_ABORTED] = "## Binary download aborted",
    [FL_S_ERR_XMODEM] = "xmodem error",
};

_Static_assert(FL_S_COUNT <= FL_WATCH_MAX, "every receiver text needs a watch");

static const uint32_t k_bauds[] = { 115200u, 460800u, 921600u };

const char *fl_str(fl_str_t s)
{
    return (unsigned)s < FL_S_COUNT ? k_str[s] : "?";
}

/* ---- listener ------------------------------------------------------------------ */

static void on_unit(const gnss_frame_t *f, void *ctx)
{
    fl_lis_t *lis = ctx;
    if (f->kind == GNSS_FRAME_RTCM3) {
        lis->rtcm++;
    } else if (f->kind == GNSS_FRAME_TEXT && f->len > 0) {
        if (f->data[0] == '$') {
            lis->sentences++;
        } else if (f->len >= 9 && memcmp(f->data, "#VERSIONA", 9) == 0) {
            gnss_versiona_t v;
            if (gnss_versiona_parse((const char *)f->data, f->len, &v)) {
                lis->ver = v;
                lis->ver_ok = true;
            }
        }
    }
}

void fl_lis_evidence_reset(fl_lis_t *lis)
{
    lis->rtcm = 0;
    lis->sentences = 0;
    lis->mismatch = 0;
    lis->err_base = lis->io->line_errors != NULL ? lis->io->line_errors(lis->io->ctx) : 0;
    lis->ver_ok = false;
    lis->bytes = 0;
    lis->any_rx = false;
    lis->text_bytes = 0;
    /* "$devicename" is application evidence only from here on. */
    lis->win.w[FL_S_DEVICENAME].seen = false;
    lis->win.w[FL_S_DEVICENAME].last_end = 0;
}

void fl_lis_init(fl_lis_t *lis, const fl_io_t *io, uint32_t baud)
{
    memset(lis, 0, sizeof(*lis));
    lis->io = io;
    lis->baud = baud;
    fl_win_init(&lis->win);
    /* One watch per text, index = fl_str_t (the two "aborted" texts are the
     * same string, watched twice: before and after the transfer). */
    for (unsigned i = 0; i < FL_S_COUNT; i++) {
        lis->win.w[i].s = k_str[i];
    }
    lis->win.nw = FL_S_COUNT;
    gnss_framer_init(&lis->framer, on_unit, lis);
    lis->last_rx_ms = fl_now(io);
    fl_lis_evidence_reset(lis);
}

/* Printable, not blank, and not 0x78 (which is also a byte of the
 * mismatch pattern, so garbage from another baud never counts as text). */
static bool printable_text(uint8_t b)
{
    return b > 0x20 && b <= 0x7E && b != 0x78;
}

void fl_lis_feed(fl_lis_t *lis, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    fl_win_append(&lis->win, data, len);
    gnss_framer_feed(&lis->framer, data, len);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00 || b == 0x78 || b == 0x80 || b == 0xF8) {
            lis->mismatch++;
        }
        if (printable_text(b)) {
            lis->text_bytes++;
        }
    }
    lis->bytes += (uint32_t)len;
    lis->any_rx = true;
    lis->last_rx_ms = fl_now(lis->io);
}

void fl_lis_tap(void *ctx, const uint8_t *data, size_t len)
{
    fl_lis_feed((fl_lis_t *)ctx, data, len);
}

size_t fl_lis_read(fl_lis_t *lis, uint32_t wait_ms, uint8_t *copy, size_t copy_cap)
{
    uint8_t buf[256];
    uint8_t *dst = (copy != NULL && copy_cap > 0) ? copy : buf;
    size_t cap = (copy != NULL && copy_cap > 0) ? copy_cap : sizeof(buf);
    if (wait_ms > FL_SLICE_MS) {
        wait_ms = FL_SLICE_MS;
    }
    size_t n = lis->io->read(lis->io->ctx, dst, cap, wait_ms);
    if (n > 0) {
        if (lis->tap != NULL) {
            lis->tap(lis->tap_ctx, dst, n);
        }
        fl_lis_feed(lis, dst, n);
    }
    return n;
}

void fl_lis_listen(fl_lis_t *lis, uint32_t ms)
{
    uint32_t t0 = fl_now(lis->io);
    for (;;) {
        uint32_t spent = fl_since(lis->io, t0);
        if (spent >= ms) {
            return;
        }
        (void)fl_lis_read(lis, ms - spent, NULL, 0);
    }
}

uint64_t fl_lis_mark(const fl_lis_t *lis)
{
    return lis->win.total;
}

bool fl_lis_seen_after(const fl_lis_t *lis, fl_str_t s, uint64_t mark)
{
    return fl_win_seen_after(&lis->win, (int)s, mark);
}

bool fl_lis_seen(const fl_lis_t *lis, fl_str_t s)
{
    return fl_win_seen(&lis->win, (int)s);
}

static uint32_t lis_mismatch(const fl_lis_t *lis)
{
    uint32_t errs = 0;
    if (lis->io->line_errors != NULL) {
        errs = lis->io->line_errors(lis->io->ctx) - lis->err_base;
    }
    return lis->mismatch + errs;
}

bool fl_lis_app_evidence(const fl_lis_t *lis)
{
    return lis->rtcm > 0 || lis->sentences > 0 || fl_win_seen(&lis->win, FL_S_DEVICENAME);
}

int fl_lis_set_baud(fl_lis_t *lis, uint32_t baud)
{
    int r = lis->io->set_baud(lis->io->ctx, baud);
    if (r == 0) {
        lis->baud = baud;
    }
    gnss_framer_reset(&lis->framer);
    fl_lis_evidence_reset(lis);
    return r;
}

bool fl_lis_wait_app(fl_lis_t *lis, uint32_t ms)
{
    uint32_t t0 = fl_now(lis->io);
    for (;;) {
        if (fl_lis_app_evidence(lis)) {
            return true;
        }
        uint32_t spent = fl_since(lis->io, t0);
        if (spent >= ms) {
            return false;
        }
        (void)fl_lis_read(lis, ms - spent, NULL, 0);
    }
}

bool fl_query_version(fl_lis_t *lis, gnss_versiona_t *out)
{
    uint64_t m = fl_lis_mark(lis);
    lis->ver_ok = false;
    if (fl_write_str(lis->io, "VERSIONA\r\n") != 0) {
        return false;
    }
    uint32_t t0 = fl_now(lis->io);
    for (;;) {
        if (lis->ver_ok) {
            if (out != NULL) {
                *out = lis->ver;
            }
            return true;
        }
        if (fl_lis_seen_after(lis, FL_S_PROMPT, m)) {
            return false;   /* a loader answered: never query further */
        }
        uint32_t spent = fl_since(lis->io, t0);
        if (spent >= FL_VER_WAIT_MS) {
            return false;
        }
        (void)fl_lis_read(lis, FL_VER_WAIT_MS - spent, NULL, 0);
    }
}

/* Wait for the application, then query its version (every FL_VER_RETRY_MS) until total_ms. */
static bool confirm_app(fl_lis_t *lis, uint32_t total_ms, gnss_versiona_t *out)
{
    uint32_t t0 = fl_now(lis->io);
    if (!fl_lis_wait_app(lis, total_ms)) {
        return false;
    }
    for (;;) {
        uint32_t tq = fl_now(lis->io);
        if (fl_query_version(lis, out)) {
            return true;
        }
        if (fl_since(lis->io, t0) >= total_ms) {
            return false;
        }
        uint32_t used = fl_since(lis->io, tq);
        if (used < FL_VER_RETRY_MS) {
            fl_lis_listen(lis, FL_VER_RETRY_MS - used);
        }
        if (fl_since(lis->io, t0) >= total_ms) {
            return false;
        }
    }
}

/* ---- identify ---------------------------------------------------------------------- */

static unsigned candidates(uint32_t first, uint32_t *out)
{
    unsigned n = 0;
    out[n++] = first != 0 ? first : FL_BAUD_APP_DEFAULT;
    for (unsigned i = 0; i < sizeof(k_bauds) / sizeof(k_bauds[0]); i++) {
        bool dup = false;
        for (unsigned j = 0; j < n; j++) {
            dup = dup || out[j] == k_bauds[i];
        }
        if (!dup) {
            out[n++] = k_bauds[i];
        }
    }
    return n;
}

fl_id_result_t fl_identify(fl_lis_t *lis, uint32_t stored_baud, bool loader_key_allowed, fl_ident_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t cand[4];
    unsigned n = candidates(stored_baud, cand);
    uint64_t start = fl_lis_mark(lis);
    for (unsigned i = 0; i < n; i++) {
        if (fl_lis_set_baud(lis, cand[i]) != 0) {
            return FL_ID_IO;
        }
        unsigned tries = (i == 0) ? 2u : 1u;
        for (unsigned t = 0; t < tries; t++) {
            gnss_versiona_t v;
            bool ok = fl_query_version(lis, &v);
            if (fl_lis_seen_after(lis, FL_S_PROMPT, start)) {
                /* The receiver sits at its loader without a session: no more queries anywhere. */
                out->loader_baud = cand[i];
                fl_notef(lis->io, "loader prompt at %u baud while identifying the receiver", (unsigned)cand[i]);
                if (loader_key_allowed) {
                    out->loader_key_sent = (fl_write_str(lis->io, "6\r\n") == 0);
                    (void)fl_lis_set_baud(lis, cand[0]);
                    out->app_back = fl_lis_wait_app(lis, FL_LOADER_RESTART_MS);
                } else {
                    (void)fl_lis_set_baud(lis, cand[0]);
                }
                return FL_ID_AT_LOADER;
            }
            if (ok) {
                out->app_baud = cand[i];
                out->ver = v;
                fl_notef(lis->io, "receiver %s %s at %u baud", v.model, v.firmware, (unsigned)cand[i]);
                return FL_ID_OK;
            }
        }
    }
    (void)fl_lis_set_baud(lis, cand[0]);
    return FL_ID_NO_ANSWER;
}

/* ---- quiesce, loader baud ---------------------------------------------------------- */

int fl_quiesce(fl_lis_t *lis)
{
    for (int i = 0; i < 3; i++) {
        if (fl_write_str(lis->io, "\r\nunlog\r\n") != 0) {
            return -1;
        }
        fl_lis_listen(lis, FL_QUIESCE_GAP_MS);
    }
    return 0;
}

static int send_config_com1(fl_lis_t *lis, uint32_t baud)
{
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "config com1 %u\r\n", (unsigned)baud);
    return fl_write_str(lis->io, cmd);
}

fl_baud_result_t fl_restore_baud(fl_lis_t *lis, uint32_t app_baud, uint32_t loader_baud)
{
    /* At the loader baud: back to the application baud, never saved. */
    if (fl_lis_set_baud(lis, loader_baud) != 0 || send_config_com1(lis, app_baud) != 0) {
        return FL_BAUD_IO;
    }
    fl_lis_listen(lis, FL_BAUD_SETTLE_MS);
    if (fl_lis_set_baud(lis, app_baud) != 0) {
        return FL_BAUD_IO;
    }
    if (fl_query_version(lis, NULL)) {
        fl_notef(lis->io, "COM1 restored to %u baud", (unsigned)app_baud);
        return FL_BAUD_RESTORED;
    }
    /* Once more at each of the two bauds, then ask at both. */
    const uint32_t both[2] = { app_baud, loader_baud };
    for (int i = 0; i < 2; i++) {
        if (fl_lis_set_baud(lis, both[i]) != 0 || send_config_com1(lis, app_baud) != 0) {
            return FL_BAUD_IO;
        }
        fl_lis_listen(lis, FL_BAUD_SETTLE_MS);
    }
    for (int i = 0; i < 2; i++) {
        if (fl_lis_set_baud(lis, both[i]) != 0) {
            return FL_BAUD_IO;
        }
        if (fl_query_version(lis, NULL)) {
            if (i == 0) {
                fl_notef(lis->io, "COM1 restored to %u baud", (unsigned)app_baud);
                return FL_BAUD_RESTORED;
            }
            break;   /* it answers only at the loader baud: not restored */
        }
    }
    (void)fl_lis_set_baud(lis, app_baud);
    fl_notef(lis->io, "receiver COM1 is at an unsaved baud; a receiver power cycle restores its saved baud");
    return FL_BAUD_STRANDED;
}

fl_baud_result_t fl_raise_baud(fl_lis_t *lis, uint32_t app_baud, uint32_t loader_baud)
{
    if (send_config_com1(lis, loader_baud) != 0) {
        return FL_BAUD_IO;
    }
    fl_lis_listen(lis, FL_BAUD_SETTLE_MS);
    if (fl_lis_set_baud(lis, loader_baud) != 0) {
        return FL_BAUD_IO;
    }
    lis->io->flush(lis->io->ctx);
    uint64_t m = fl_lis_mark(lis);
    if (fl_write_str(lis->io, "gpgga 1\r\n$cfgmsg,0,0,1\r\n") != 0) {
        return FL_BAUD_IO;
    }
    fl_lis_listen(lis, FL_VERIFY_MS);
    if (fl_lis_seen_after(lis, FL_S_GGA, m) || fl_lis_seen_after(lis, FL_S_GSV, m) ||
        fl_lis_seen_after(lis, FL_S_RMC, m)) {
        fl_notef(lis->io, "COM1 at %u baud, verified", (unsigned)loader_baud);
        return FL_BAUD_OK;
    }
    fl_notef(lis->io, "no sentence at %u baud after the change: restoring", (unsigned)loader_baud);
    return fl_restore_baud(lis, app_baud, loader_baud);
}

int fl_pre_reset_quiet(fl_lis_t *lis)
{
    if (fl_write_str(lis->io, "\r\nunlog\r\n") != 0) {
        return -1;
    }
    fl_sleep(lis->io, FL_PRE_RESET_MS);
    lis->io->flush(lis->io->ctx);
    return 0;
}

/* ---- reset and stop key ------------------------------------------------------------- */

size_t fl_stop_key_burst(uint32_t baud)
{
    if (baud <= 115200u) {
        return 8;
    }
    if (baud <= 460800u) {
        return 32;
    }
    return 64;
}

static fl_entry_result_t stop_key(fl_lis_t *lis, uint32_t t0, uint32_t max_ms, bool late_rule,
                                  fl_entry_stats_t *st)
{
    static const uint8_t key[64] = {
        'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@',
        'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@',
        'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@',
        'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@', 'T', '@',
    };
    size_t burst = fl_stop_key_burst(lis->baud);
    uint64_t m = fl_lis_mark(lis);
    for (;;) {
        if (fl_write(lis->io, key, burst) != 0) {
            return FL_ENTRY_IO;
        }
        st->bursts++;
        st->key_bytes += (uint32_t)burst;
        lis->io->sleep_ms(lis->io->ctx, FL_STOPKEY_GAP_MS);
        for (;;) {
            size_t n = fl_lis_read(lis, 0, NULL, 0);
            uint32_t el = (uint32_t)(fl_now(lis->io) - t0);
            if (fl_lis_seen_after(lis, FL_S_PROMPT, m)) {
                st->prompt_ms = (uint32_t)(fl_now(lis->io) - t0);
                st->baud_text = fl_lis_seen_after(lis, FL_S_UART_BAUD, m);
                st->timeout_text = fl_lis_seen_after(lis, FL_S_TIMEOUT_CFG, m);
                st->action_text = fl_lis_seen_after(lis, FL_S_DEFAULT_ACTION, m);
                st->last_menu_line = fl_lis_seen_after(lis, FL_S_LAST_MENU, m);
                fl_notef(lis->io, "loader prompt %u ms after the reset (%u stop-key bytes)", (unsigned)st->prompt_ms,
                         (unsigned)st->key_bytes);
                return FL_ENTRY_PROMPT;
            }
            if (n == 0) {
                break;
            }
            if (late_rule && el >= FL_ENTRY_LATE_MS) {
                fl_notef(lis->io, "data %u ms after the reset without the loader prompt", (unsigned)el);
                return FL_ENTRY_LATE;
            }
            if (el >= max_ms) {
                break;   /* a receiver that keeps talking still ends at the limit */
            }
        }
        if (fl_lis_seen_after(lis, FL_S_DEVICENAME, m)) {
            fl_notef(lis->io, "the application restarted instead of the loader");
            return FL_ENTRY_APP;
        }
        if ((uint32_t)(fl_now(lis->io) - t0) >= max_ms) {
            fl_notef(lis->io, "no loader prompt within %u ms", (unsigned)max_ms);
            return FL_ENTRY_TIMEOUT;
        }
    }
}

fl_entry_result_t fl_reset_and_stop(fl_lis_t *lis, uint32_t loader_baud, fl_entry_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    (void)loader_baud;
    static const char reset_cmd[] = "\r\nreset\r\nreset\r\n";
    _Static_assert(sizeof(reset_cmd) - 1 == 16, "the reset write is 16 bytes");
    if (fl_write(lis->io, reset_cmd, sizeof(reset_cmd) - 1) != 0) {
        return FL_ENTRY_IO;
    }
    st->t0_ms = fl_now(lis->io);   /* the write has drained */
    lis->io->flush(lis->io->ctx);  /* the application's reply to the reset is not needed */
    return stop_key(lis, st->t0_ms, FL_ENTRY_MAX_MS, true, st);
}

fl_entry_result_t fl_stop_key_stream(fl_lis_t *lis, uint32_t max_ms, bool late_rule, fl_entry_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->t0_ms = fl_now(lis->io);
    return stop_key(lis, st->t0_ms, max_ms, late_rule, st);
}

/* ---- menu ------------------------------------------------------------------------------ */

fl_prompt_result_t fl_wait_prompt(fl_lis_t *lis, uint64_t mark, uint32_t max_ms, uint32_t nudge_ms)
{
    uint32_t t0 = fl_now(lis->io);
    uint32_t last = t0;
    bool nudged = false;
    fl_lis_evidence_reset(lis);
    for (;;) {
        if (fl_lis_seen_after(lis, FL_S_PROMPT, mark)) {
            return FL_PROMPT_SEEN;
        }
        if (fl_lis_app_evidence(lis) || lis_mismatch(lis) >= FL_MISMATCH_LISTEN) {
            fl_notef(lis->io, "the receiver left its loader (application output or another baud)");
            return FL_PROMPT_LEFT;
        }
        uint32_t spent = fl_since(lis->io, t0);
        if (spent >= max_ms) {
            return FL_PROMPT_TIMEOUT;
        }
        if (nudge_ms > 0 && !nudged && fl_since(lis->io, last) >= nudge_ms) {
            nudged = true;
            (void)fl_write_str(lis->io, "\r\n");
        }
        uint32_t wait = max_ms - spent;
        if (nudge_ms > 0 && !nudged) {
            uint32_t to_nudge = nudge_ms - fl_since(lis->io, last);
            wait = to_nudge < wait ? (to_nudge > 0 ? to_nudge : 1) : wait;
        }
        if (fl_lis_read(lis, wait, NULL, 0) > 0) {
            last = fl_now(lis->io);
        }
    }
}

fl_prompt_result_t fl_after_handshake(fl_lis_t *lis, uint64_t mark)
{
    return fl_wait_prompt(lis, mark, FL_PROMPT_MS, FL_NUDGE_MS);
}

static void keep(char *dst, size_t cap, const char *s)
{
    snprintf(dst, cap, "%s", s);
}

/* Wait for the prompt after a menu error text (ending at err_end). */
static bool prompt_after(fl_lis_t *lis, uint64_t err_end)
{
    return fl_wait_prompt(lis, err_end, FL_PROMPT_MS, 0) == FL_PROMPT_SEEN;
}

/* Until the receiver has been quiet for FL_MENU_QUIET_MS (at most FL_MENU_QUIET_MAX_MS). */
static void wait_quiet(fl_lis_t *lis)
{
    uint32_t t0 = fl_now(lis->io);
    uint32_t last = t0;
    while (fl_since(lis->io, last) < FL_MENU_QUIET_MS && fl_since(lis->io, t0) < FL_MENU_QUIET_MAX_MS) {
        if (fl_lis_read(lis, FL_MENU_QUIET_MS, NULL, 0) > 0) {
            last = fl_now(lis->io);
        }
    }
}

fl_menu_result_t fl_menu_download(fl_lis_t *lis, fl_rx_t *rx, const fl_menu_job_t *job, fl_menu_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    if (job->confirm_prompt) {
        /* Quiet first, then a prompt that answers our own "\r\n". */
        wait_quiet(lis);
        uint64_t m = fl_lis_mark(lis);
        if (fl_write_str(lis->io, "\r\n") != 0) {
            return FL_MENU_IO;
        }
        fl_prompt_result_t pr = fl_wait_prompt(lis, m, FL_PROMPT_MS, 0);
        if (pr != FL_PROMPT_SEEN) {
            keep(st->text, sizeof(st->text), pr == FL_PROMPT_LEFT ? "the receiver left its loader" : "no prompt");
            fl_notef(lis->io, "no loader prompt after \"\\r\\n\"");
            return FL_MENU_FAILED;
        }
    }
    if (job->before_item != NULL && !job->before_item(job->ctx)) {
        st->at_prompt = true;
        return FL_MENU_JOURNAL;
    }
    if (job->copy_ok != NULL && !job->copy_ok(job->ctx)) {
        st->at_prompt = true;
        fl_notef(lis->io, "the package copy in RAM changed: nothing sent");
        return FL_MENU_COPY_CHANGED;
    }
    /* The rest of the prompt line first, so "nothing since the item" means
     * nothing the item caused. */
    wait_quiet(lis);
    uint64_t m = fl_lis_mark(lis);
    if (fl_write_str(lis->io, "\r\n2\r\n") != 0) {
        return FL_MENU_IO;
    }
    st->sends = 1;
    uint32_t t_last = fl_now(lis->io);
    unsigned invalid = 0;
    bool ready = false;
    uint64_t ready_end = 0;
    uint64_t prompt_end = 0;
    uint32_t t_prompt = 0;
    for (;;) {
        uint8_t buf[256];
        uint64_t base = fl_lis_mark(lis);
        size_t n = fl_lis_read(lis, 100, buf, sizeof(buf));
        if (!ready && fl_lis_seen_after(lis, FL_S_READY, m)) {
            ready = true;
            ready_end = lis->win.w[FL_S_READY].last_end;
        }
        if (ready) {
            for (size_t i = 0; i < n; i++) {
                if (base + i >= ready_end && buf[i] == '\n') {
                    /* What came after the line in the same read is the start of the handshake. */
                    fl_rx_push(rx, buf + i + 1, n - i - 1);
                    fl_notef(lis->io, "ready for the download after %u menu item(s)", (unsigned)st->sends);
                    return FL_MENU_READY;
                }
            }
        } else {
            if (!st->begin_seen && fl_lis_seen_after(lis, FL_S_BEGIN, m)) {
                st->begin_seen = true;
                fl_notef(lis->io, "loader: begin to update flash");
            }
            if (fl_lis_seen_after(lis, FL_S_SUBMENU, m)) {
                keep(st->text, sizeof(st->text), fl_str(FL_S_SUBMENU));
                uint64_t mq = fl_lis_mark(lis);
                fl_notef(lis->io, "firmware submenu: leaving it with \"q\"");
                if (fl_write_str(lis->io, "q\r\n") != 0) {
                    return FL_MENU_IO;
                }
                st->at_prompt = prompt_after(lis, mq);
                return FL_MENU_SUBMENU;
            }
            if (fl_lis_seen_after(lis, FL_S_INVALID, m) || fl_lis_seen_after(lis, FL_S_TOO_LONG, m)) {
                invalid++;
                fl_str_t which = fl_lis_seen_after(lis, FL_S_INVALID, m) ? FL_S_INVALID : FL_S_TOO_LONG;
                keep(st->text, sizeof(st->text), fl_str(which));
                uint64_t err_end = lis->win.w[which].last_end;
                bool at = prompt_after(lis, err_end);
                if (invalid >= 2 || !at) {
                    st->at_prompt = at;
                    fl_notef(lis->io, "menu item refused (%s)%s", fl_str(which), invalid >= 2 ? " twice" : "");
                    return FL_MENU_FAILED;
                }
                m = fl_lis_mark(lis);
                if (fl_write_str(lis->io, "\r\n2\r\n") != 0) {
                    return FL_MENU_IO;
                }
                st->sends++;
                t_last = fl_now(lis->io);
                continue;
            }
            if (fl_lis_seen_after(lis, FL_S_INPUT_TIMEOUT, m) || fl_lis_seen_after(lis, FL_S_ABORTED, m)) {
                fl_str_t which = fl_lis_seen_after(lis, FL_S_INPUT_TIMEOUT, m) ? FL_S_INPUT_TIMEOUT : FL_S_ABORTED;
                keep(st->text, sizeof(st->text), fl_str(which));
                st->at_prompt = prompt_after(lis, lis->win.w[which].last_end);
                fl_notef(lis->io, "menu item failed: %s", fl_str(which));
                return FL_MENU_FAILED;
            }
            if (fl_lis_seen_after(lis, FL_S_PROMPT, m)) {
                /* The leading "\r\n" of the item may print a prompt of its
                 * own: only a prompt that stays the last thing said for
                 * FL_MENU_SETTLE_MS means the item did nothing. */
                uint64_t pe = lis->win.w[FL_S_PROMPT].last_end;
                if (pe != prompt_end) {
                    prompt_end = pe;
                    t_prompt = fl_now(lis->io);
                }
                if (fl_lis_mark(lis) <= prompt_end + 1u && fl_since(lis->io, t_prompt) >= FL_MENU_SETTLE_MS) {
                    keep(st->text, sizeof(st->text), "prompt without the ready line");
                    st->at_prompt = true;
                    fl_notef(lis->io, "loader prompt without the ready line");
                    return FL_MENU_FAILED;
                }
            }
            /* Resent only while the receiver has said nothing at all. */
            if (fl_lis_mark(lis) == m && fl_since(lis->io, t_last) >= FL_MENU_RESEND_MS && st->sends < FL_MENU_SENDS) {
                m = fl_lis_mark(lis);
                if (fl_write_str(lis->io, "\r\n2\r\n") != 0) {
                    return FL_MENU_IO;
                }
                st->sends++;
                t_last = fl_now(lis->io);
                continue;
            }
        }
        if (fl_since(lis->io, t_last) >= FL_READY_MS) {
            static const uint8_t can2[2] = { FL_CAN, FL_CAN };
            keep(st->text, sizeof(st->text), "no ready line");
            fl_notef(lis->io, "no ready line within %u ms: CAN CAN", (unsigned)FL_READY_MS);
            uint64_t mc = fl_lis_mark(lis);
            (void)fl_write(lis->io, can2, sizeof(can2));
            st->at_prompt = fl_wait_prompt(lis, mc, FL_PROMPT_MS, FL_NUDGE_MS) == FL_PROMPT_SEEN;
            return FL_MENU_FAILED;
        }
    }
}

/* ---- after the transfer ---------------------------------------------------------------- */

bool fl_error_retryable(fl_str_t s)
{
    return s == FL_S_ERR_ABORTED || s == FL_S_ERR_XMODEM;
}

/* The hard error seen first in the stream, FL_S_COUNT none. */
static fl_str_t first_error(const fl_lis_t *lis)
{
    fl_str_t best = FL_S_COUNT;
    uint64_t at = UINT64_MAX;
    for (int s = FL_S_ERR_FIRST; s <= FL_S_ERR_LAST; s++) {
        const fl_watch_t *w = &lis->win.w[s];
        if (w->seen && w->first_end < at) {
            at = w->first_end;
            best = (fl_str_t)s;
        }
    }
    return best;
}

void fl_post_wait(fl_lis_t *post, uint32_t t_eot, uint32_t app_baud, fl_post_t *out)
{
    memset(out, 0, sizeof(*out));
    out->error = FL_S_COUNT;
    const fl_io_t *io = post->io;
    uint32_t t_err = 0;
    for (;;) {
        uint32_t el = (uint32_t)(fl_now(io) - t_eot);
        if (out->success_ms == 0 && fl_lis_seen(post, FL_S_BACKUP_OK)) {
            out->success_ms = el;
            out->success_this_boot = true;
            fl_notef(io, "loader: backup succeed after %u ms", (unsigned)el);
        }
        if (!out->recover_warning && fl_lis_seen(post, FL_S_RECOVER_WARN)) {
            out->recover_warning = true;
            fl_notef(io, "loader: recovery warning (not an error)");
        }
        fl_str_t err = first_error(post);
        bool prompt = fl_lis_seen(post, FL_S_PROMPT);
        if (prompt && out->prompt_ms == 0) {
            out->prompt_ms = el;
        }
        if (err != FL_S_COUNT) {
            if (out->error == FL_S_COUNT) {
                out->error = err;
                out->error_retryable = fl_error_retryable(err);
                t_err = fl_now(io);
                fl_notef(io, "loader reported \"%s\" %u ms after the transfer", fl_str(err), (unsigned)el);
            }
            if (prompt && post->win.w[FL_S_PROMPT].last_end > post->win.w[err].first_end) {
                out->prompt_after_error = true;
                out->outcome = FL_OUT_E;
                return;
            }
            if (el >= FL_T1_MS && fl_since(io, t_err) >= FL_PROMPT_MS) {
                out->outcome = FL_OUT_E;
                return;
            }
        } else if (prompt) {
            bool ok = fl_lis_seen(post, FL_S_BACKUP_OK) &&
                      post->win.w[FL_S_BACKUP_OK].first_end < post->win.w[FL_S_PROMPT].first_end;
            out->outcome = ok ? (out->prompt_ms <= FL_T1_MS ? FL_OUT_S : FL_OUT_L) : FL_OUT_U;
            fl_notef(io, "after the transfer: %s (prompt after %u ms)", ok ? "success" : "prompt without the success text",
                     (unsigned)out->prompt_ms);
            return;
        }
        if (el >= FL_T2_MS) {
            if (out->error != FL_S_COUNT) {
                out->outcome = FL_OUT_E;
                return;
            }
            if (post->text_bytes > 0) {
                out->outcome = FL_OUT_U;
                fl_notef(io, "after the transfer: text but no prompt by %u ms", (unsigned)FL_T2_MS);
                return;
            }
            /* No answer at all: one nudge at the loader baud, then look for the application. */
            uint64_t m = fl_lis_mark(post);
            fl_notef(io, "nothing from the receiver for %u ms: one \"\\r\\n\"", (unsigned)FL_T2_MS);
            (void)fl_write_str(io, "\r\n");
            fl_lis_listen(post, FL_POST_NUDGE_WAIT_MS);
            if (fl_lis_seen_after(post, FL_S_PROMPT, m)) {
                out->outcome = FL_OUT_U;
                return;
            }
            (void)fl_lis_set_baud(post, app_baud);
            out->outcome = fl_lis_wait_app(post, FL_APPCHECK_MS) ? FL_OUT_N_APP : FL_OUT_N_SILENT;
            return;
        }
        (void)fl_lis_read(post, FL_T2_MS - el, NULL, 0);
    }
}

/* ---- leaving the loader ---------------------------------------------------------------- */

static bool wait_text(fl_lis_t *lis, fl_str_t s, uint64_t mark, uint32_t ms)
{
    uint32_t t0 = fl_now(lis->io);
    for (;;) {
        if (fl_lis_seen_after(lis, s, mark)) {
            return true;
        }
        uint32_t spent = fl_since(lis->io, t0);
        if (spent >= ms) {
            return false;
        }
        (void)fl_lis_read(lis, ms - spent, NULL, 0);
    }
}

static fl_exit_result_t scan_and_listen(fl_lis_t *lis, uint32_t loader_baud, uint32_t app_baud, fl_exit_t *out)
{
    uint32_t cand[4];
    unsigned n = candidates(app_baud, cand);
    for (unsigned i = 0; i < n; i++) {
        if (fl_lis_set_baud(lis, cand[i]) != 0) {
            return FL_EXIT_IO;
        }
        if (fl_lis_wait_app(lis, FL_SCAN_LISTEN_MS) && fl_query_version(lis, &out->ver)) {
            out->app_baud_found = cand[i];
            fl_notef(lis->io, "application answers at %u baud: %s", (unsigned)cand[i], out->ver.firmware);
            return FL_EXIT_APP;
        }
    }
    if (fl_lis_set_baud(lis, loader_baud) != 0) {
        return FL_EXIT_IO;
    }
    /* An idle loader prints nothing: one "\r\n" after a few seconds of
     * silence (harmless to an application, the prompt's own key). */
    uint64_t m = fl_lis_mark(lis);
    fl_prompt_result_t pr = fl_wait_prompt(lis, m, FL_APPCHECK_MS, FL_NUDGE_MS);
    if (pr == FL_PROMPT_SEEN) {
        fl_notef(lis->io, "only the loader answers: the application does not start");
        return FL_EXIT_NO_APP_PROMPT;
    }
    if (pr == FL_PROMPT_LEFT && fl_query_version(lis, &out->ver)) {
        out->app_baud_found = loader_baud;
        fl_notef(lis->io, "application answers at %u baud: %s", (unsigned)loader_baud, out->ver.firmware);
        return FL_EXIT_APP;
    }
    (void)fl_lis_set_baud(lis, app_baud);
    fl_notef(lis->io, "the receiver is silent");
    return FL_EXIT_SILENT;
}

fl_exit_result_t fl_leave_loader(fl_lis_t *lis, uint32_t loader_baud, uint32_t app_baud, fl_exit_t *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t m = fl_lis_mark(lis);
    if (fl_write_str(lis->io, "6\r\n6\r\n6\r\n") != 0) {
        return FL_EXIT_IO;
    }
    out->resetting_seen = wait_text(lis, FL_S_RESETTING, m, FL_RESETTING_MS);
    if (!out->resetting_seen && fl_lis_seen_after(lis, FL_S_PROMPT, m)) {
        uint64_t m2 = fl_lis_mark(lis);
        if (fl_write_str(lis->io, "6\r\n") != 0) {
            return FL_EXIT_IO;
        }
        out->resetting_seen = wait_text(lis, FL_S_RESETTING, m2, FL_RESETTING_MS);
    }
    fl_notef(lis->io, "leaving the loader: %s", out->resetting_seen ? "resetting the cpu" : "no reset text, going on");
    if (fl_lis_set_baud(lis, app_baud) != 0) {
        return FL_EXIT_IO;
    }
    uint32_t t0 = fl_now(lis->io);
    bool app = fl_lis_wait_app(lis, FL_APP_WAIT_MS);
    uint32_t t_ver = fl_now(lis->io);
    while (fl_since(lis->io, t_ver) < FL_VER_TOTAL_MS) {
        if (!app) {
            uint32_t left = FL_VER_TOTAL_MS - fl_since(lis->io, t_ver);
            app = fl_lis_wait_app(lis, left < FL_VER_RETRY_MS ? left : FL_VER_RETRY_MS);
            continue;
        }
        uint32_t tq = fl_now(lis->io);
        if (fl_query_version(lis, &out->ver)) {
            out->app_baud_found = app_baud;
            fl_notef(lis->io, "application back after %u ms: %s", (unsigned)fl_since(lis->io, t0), out->ver.firmware);
            return FL_EXIT_APP;
        }
        uint32_t used = fl_since(lis->io, tq);
        if (used < FL_VER_RETRY_MS) {
            fl_lis_listen(lis, FL_VER_RETRY_MS - used);
        }
    }
    return scan_and_listen(lis, loader_baud, app_baud, out);
}

fl_exit_result_t fl_leave_loader_early(fl_lis_t *lis, uint32_t loader_baud, uint32_t app_baud, fl_exit_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int round = 0; round < 2; round++) {
        uint64_t m = fl_lis_mark(lis);
        if (round == 1 && fl_write_str(lis->io, "\r\n") != 0) {
            return FL_EXIT_IO;
        }
        if (fl_write_str(lis->io, "6\r\n") != 0) {
            return FL_EXIT_IO;
        }
        out->resetting_seen = wait_text(lis, FL_S_RESETTING, m, FL_RESETTING_MS) || out->resetting_seen;
        if (fl_lis_set_baud(lis, app_baud) != 0) {
            return FL_EXIT_IO;
        }
        if (confirm_app(lis, FL_PRE_EXIT_APP_MS, &out->ver)) {
            out->app_baud_found = app_baud;
            fl_notef(lis->io, "application back: %s", out->ver.firmware);
            return FL_EXIT_APP;
        }
        if (round == 1) {
            break;
        }
        /* Not back: is the loader still there? Listen (an idle loader says
         * nothing: one "\r\n" after a few seconds of silence). */
        if (fl_lis_set_baud(lis, loader_baud) != 0) {
            return FL_EXIT_IO;
        }
        uint64_t ml = fl_lis_mark(lis);
        if (fl_wait_prompt(lis, ml, FL_PROMPT_MS, FL_NUDGE_MS) != FL_PROMPT_SEEN) {
            break;
        }
        fl_notef(lis->io, "still at the loader prompt: restart key once more");
    }
    (void)fl_lis_set_baud(lis, app_baud);
    fl_notef(lis->io, "the application did not come back");
    return FL_EXIT_SILENT;
}

bool fl_app_check(fl_lis_t *lis, uint32_t app_baud, uint32_t listen_ms, gnss_versiona_t *out)
{
    if (fl_lis_set_baud(lis, app_baud) != 0) {
        return false;
    }
    if (!fl_lis_wait_app(lis, listen_ms)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (fl_query_version(lis, out)) {
            return true;
        }
    }
    return false;
}

/* ---- bench observation -------------------------------------------------------------------- */

void fl_watch_start_bytes(fl_lis_t *lis, fl_rx_t *rx, uint32_t ms, fl_startbytes_t *out)
{
    memset(out, 0, sizeof(*out));
    out->first_ms = UINT32_MAX;
    uint32_t t0 = fl_now(lis->io);
    uint32_t prev = 0;
    fl_ev_t prev_kind = FL_EV_TIMEOUT;
    fl_tap_t old_tap = rx->tap;
    void *old_ctx = rx->tap_ctx;
    rx->tap = fl_lis_tap;
    rx->tap_ctx = lis;
    for (;;) {
        fl_ev_t ev = fl_rx_next(rx, t0 + ms);
        if (ev == FL_EV_TIMEOUT) {
            break;
        }
        uint32_t at = fl_since(lis->io, t0);
        if (ev == FL_EV_C || ev == FL_EV_NAK) {
            if (ev == FL_EV_C) {
                out->c_count++;
            } else {
                out->nak_count++;
            }
            if (out->first_ms == UINT32_MAX) {
                out->first_ms = at;
            } else if (out->interval_ms == 0 && ev == prev_kind) {
                out->interval_ms = at - prev;
            }
            fl_notef(lis->io, "start byte %s at %u ms", ev == FL_EV_C ? "C" : "NAK", (unsigned)at);
            prev = at;
            prev_kind = ev;
        } else if (ev == FL_EV_TEXT) {
            out->lines++;
        } else {
            out->other_bytes++;
        }
    }
    rx->tap = old_tap;
    rx->tap_ctx = old_ctx;
}
