/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fl_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Internal: an attempt failed before the point of no return, the
 * application is back and another attempt may follow. */
#define A_RETRY (-1)

#define BENCH_ABORT_WATCH_MS 120000u   /* bench: how long the receiver is watched after a deliberate stop */

static int at_prompt_menu(fl_sess_t *s, bool confirm);
static int entry_attempt(fl_sess_t *s, uint32_t boot);

/* ---- journal and progress ---------------------------------------------------- */

static bool main_state(uint8_t st)
{
    return st >= UM98X_JOURNAL_PREPARED && st <= UM98X_JOURNAL_VERIFYING;
}

static void progress(fl_sess_t *s, um98x_flash_phase_t phase)
{
    s->prog.phase = phase;
    s->prog.transfers = s->j.transfers;
    s->prog.entry_attempts = s->j.entry_attempts;
    s->prog.failure_class = s->j.failure_class;
    s->prog.journal_state = s->j.state;
    if (s->hooks.progress != NULL) {
        s->hooks.progress(s->hooks.ctx, &s->prog);
    }
}

static bool put(fl_sess_t *s, uint8_t state)
{
    s->j.state = state;
    if (main_state(state)) {
        s->j.phase = state;
    }
    s->j.changed_unix = s->hooks.unix_now != NULL ? s->hooks.unix_now(s->hooks.ctx) : 0;
    bool ok = s->hooks.journal_put == NULL || s->hooks.journal_put(s->hooks.ctx, &s->j);
    s->prog.journal_state = state;
    if (!ok) {
        fl_notef(s->io, "journal write (%s) failed", flash_journal_state_name(state));
    }
    return ok;
}

static void reason(fl_sess_t *s, const char *fmt, va_list ap)
{
    vsnprintf(s->out.reason, sizeof(s->out.reason), fmt, ap);
    fl_notef(s->io, "%s", s->out.reason);
}

static int needs_user(fl_sess_t *s, uint8_t failure, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int needs_user(fl_sess_t *s, uint8_t failure, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    reason(s, fmt, ap);
    va_end(ap);
    s->out.failure = failure;
    s->j.failure_class = failure;
    (void)put(s, UM98X_JOURNAL_NEEDS_USER);
    progress(s, UM98X_FLASH_NEEDS_USER);
    s->out.result = FL_SESS_NEEDS_USER;
    return FL_SESS_NEEDS_USER;
}

static int ended(fl_sess_t *s, fl_sess_result_t r, const gnss_versiona_t *ver, uint32_t baud, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static int ended(fl_sess_t *s, fl_sess_result_t r, const gnss_versiona_t *ver, uint32_t baud, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    reason(s, fmt, ap);
    va_end(ap);
    if (ver != NULL) {
        s->out.ver = *ver;
    }
    s->out.app_baud_found = baud;
    s->out.result = r;
    progress(s, UM98X_FLASH_FINISHING);
    return r;
}

static bool ponr(const fl_sess_t *s)
{
    return s->j.transfers > 0 || s->out.ponr;
}

static uint32_t transfer_cap(const fl_sess_t *s)
{
    return FL_TRANSFERS_MAX + s->user_transfers;
}

static bool model_ok(const fl_sess_t *s, const gnss_versiona_t *v)
{
    return strcmp(v->model, s->j.pre_model) == 0;
}

/* ---- the application after a pre-PoNR exit --------------------------------------- */

/* The session ends before the point of no return: the application must answer
 * at its baud (the station re-applies the profile and closes the journal).
 * reset: the receiver was reset since it last answered, so it is asked only
 * once it talks again (it may be in its loader); otherwise it never left its
 * application and is simply asked (the quiesce stopped its output, so there
 * would be nothing to listen to). */
static int finish_pre(fl_sess_t *s, uint8_t failure, const char *why, bool reset)
{
    s->out.failure = failure;
    s->j.failure_class = failure;
    gnss_versiona_t v;
    bool ok = false;
    if (fl_lis_set_baud(&s->lis, s->j.app_baud) == 0) {
        if (reset) {
            ok = fl_app_check(&s->lis, s->j.app_baud, FL_PRE_EXIT_APP_MS, &v);
        } else {
            for (int i = 0; i < 3 && !ok; i++) {
                ok = fl_query_version(&s->lis, &v);
            }
        }
    }
    if (ok) {
        return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud, "update not done: %s; receiver unchanged (%s)", why,
                     v.firmware);
    }
    return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "update not done (%s) and the receiver does not answer", why);
}

/* Leave the loader with "6" before the point of no return. */
static bool leave_early(fl_sess_t *s, gnss_versiona_t *v)
{
    fl_exit_t ex;
    fl_exit_result_t r = fl_leave_loader_early(&s->lis, s->j.boot_baud, s->j.app_baud, &ex);
    if (r == FL_EXIT_APP) {
        *v = ex.ver;
        return true;
    }
    return false;
}

/* An entry attempt failed before the point of no return. at_loader: the
 * loader may be listening (leave it with "6"); left: the application already
 * answers. Returns A_RETRY when another attempt may follow. */
static int attempt_failed(fl_sess_t *s, bool at_loader, bool left, const char *why)
{
    s->out.failure = UM98X_FLASH_FAIL_ENTRY;
    s->j.failure_class = UM98X_FLASH_FAIL_ENTRY;
    fl_notef(s->io, "entry attempt %u failed: %s", (unsigned)s->j.entry_attempts, why);
    gnss_versiona_t v;
    bool app = false;
    if (at_loader && !left) {
        app = leave_early(s, &v);
    } else {
        (void)fl_lis_set_baud(&s->lis, s->j.app_baud);
        app = fl_app_check(&s->lis, s->j.app_baud, FL_APP_WAIT_MS, &v);
        if (!app) {
            /* Not back: maybe the loader answers at its baud (listen only). */
            (void)fl_lis_set_baud(&s->lis, s->j.boot_baud);
            uint64_t m = fl_lis_mark(&s->lis);
            fl_lis_listen(&s->lis, FL_PROMPT_MS);
            if (fl_lis_seen_after(&s->lis, FL_S_PROMPT, m)) {
                app = leave_early(s, &v);
            }
        }
    }
    if (!app) {
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER,
                          "the receiver did not come back after a failed loader entry (%s); power-cycle it", why);
    }
    if (!model_ok(s, &v)) {
        return needs_user(s, UM98X_FLASH_FAIL_ENTRY, "the receiver now reports model \"%s\", not \"%s\"", v.model,
                          s->j.pre_model);
    }
    if (s->j.entry_attempts >= FL_ENTRY_ATTEMPTS) {
        return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud,
                     "update not done: the loader was not reached in %u attempts (last: %s); receiver unchanged",
                     (unsigned)s->j.entry_attempts, why);
    }
    return A_RETRY;
}

/* ---- after the point of no return ------------------------------------------------- */

static bool transfers_left(const fl_sess_t *s)
{
    return s->j.transfers < transfer_cap(s);
}

/* The RAM copy a transfer sends: this run's, or the pinned package checked
 * again into RAM through the recovery gate (a user retry and a recovery run
 * start without one). */
static bool copy_ready(fl_sess_t *s)
{
    return s->pkg != NULL || (s->hooks.recovery_prepare != NULL && s->hooks.recovery_prepare(s->hooks.ctx));
}

/* Retry from the loader prompt with the same package (recovery gate when the
 * RAM copy is not this run's). */
static int retry_from_prompt(fl_sess_t *s)
{
    if (!transfers_left(s)) {
        return needs_user(s, s->j.failure_class,
                          "%u transfers under this session: another one needs your confirmation",
                          (unsigned)s->j.transfers);
    }
    if (!copy_ready(s)) {
        return needs_user(s, s->j.failure_class, "the staged package is not available for a retry");
    }
    fl_notef(s->io, "sending the same package again (transfer %u of at most %u)", (unsigned)(s->j.transfers + 1u),
             (unsigned)transfer_cap(s));
    return at_prompt_menu(s, false);
}

/* A failure after the point of no return: record it first, back to the prompt,
 * retry only transport failures within the caps. Never "6". */
static int post_fail(fl_sess_t *s, uint8_t failure, bool retryable, bool left, const char *why)
{
    s->out.failure = failure;
    s->j.failure_class = failure;
    (void)put(s, UM98X_JOURNAL_FAILED);
    progress(s, UM98X_FLASH_FAILED);
    fl_notef(s->io, "transfer failed after the point of no return: %s", why);
    fl_prompt_result_t pr = left ? FL_PROMPT_LEFT
                                 : fl_wait_prompt(&s->lis, s->transfer_mark, FL_POST_FAIL_PROMPT_MS, FL_POST_FAIL_NUDGE_MS);
    if (pr == FL_PROMPT_SEEN) {
        s->at_prompt = true;
        if (!retryable) {
            return needs_user(s, failure, "%s; the receiver waits at its loader prompt", why);
        }
        return retry_from_prompt(s);
    }
    if (pr == FL_PROMPT_LEFT) {
        /* It left the loader. With its application answering (journalled
         * model), enter the loader again with the same package. */
        gnss_versiona_t v;
        if (!fl_app_check(&s->lis, s->j.app_baud, FL_APPCHECK_MS, &v)) {
            return needs_user(s, failure, "%s; the receiver left its loader and does not answer", why);
        }
        if (!model_ok(s, &v) || !retryable || !transfers_left(s) || s->j.entry_attempts >= FL_ENTRY_ATTEMPTS) {
            return needs_user(s, failure, "%s; the receiver runs its application again (%s)", why, v.firmware);
        }
        if (!copy_ready(s)) {
            return needs_user(s, failure, "%s; the staged package is not available for a retry", why);
        }
        s->j.entry_attempts++;
        return entry_attempt(s, s->j.boot_baud);
    }
    return needs_user(s, failure, "%s; no loader prompt afterwards", why);
}

/* Leave the loader after a good transfer and confirm the application. */
static int leave_and_confirm(fl_sess_t *s)
{
    progress(s, UM98X_FLASH_FINISHING);
    (void)put(s, UM98X_JOURNAL_VERIFYING);
    fl_exit_t ex;
    fl_exit_result_t r = fl_leave_loader(&s->lis, s->j.boot_baud, s->j.app_baud, &ex);
    switch (r) {
    case FL_EXIT_APP:
        return ended(s, ponr(s) ? FL_SESS_DONE : FL_SESS_ABORTED, &ex.ver, ex.app_baud_found,
                     "receiver application back: %s", ex.ver.firmware);
    case FL_EXIT_NO_APP_PROMPT:
        return needs_user(s, UM98X_FLASH_FAIL_APP_NOT_STARTING,
                          "the loader answers but the application does not start");
    case FL_EXIT_SILENT:
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "the receiver is silent after the update; power-cycle it");
    default:
        return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error while leaving the loader");
    }
}

static uint8_t error_class(fl_str_t e)
{
    switch (e) {
    case FL_S_ERR_PKG_HEADER:
    case FL_S_ERR_SUM32:
    case FL_S_ERR_DECOMP:
    case FL_S_ERR_LZOP:
    case FL_S_ERR_SIG:
    case FL_S_ERR_SIG2:
    case FL_S_ERR_AUTH:
        return UM98X_FLASH_FAIL_PACKAGE;
    case FL_S_ERR_ABORTED:
    case FL_S_ERR_XMODEM:
        return UM98X_FLASH_FAIL_TRANSPORT;
    default:
        return UM98X_FLASH_FAIL_RECEIVER;
    }
}

/* The post-transfer outcome and what follows from it. */
static int after_outcome(fl_sess_t *s, const fl_post_t *p)
{
    s->out.outcome_valid = true;
    s->out.outcome = p->outcome;
    s->out.success_ms = p->success_ms;
    s->out.prompt_ms = p->prompt_ms;
    s->lis.baud = s->post.baud;   /* the post-transfer listener may have switched the host */
    switch (p->outcome) {
    case FL_OUT_S:
    case FL_OUT_L:
        return leave_and_confirm(s);
    case FL_OUT_E: {
        uint8_t cls = error_class(p->error);
        char why[64];
        snprintf(why, sizeof(why), "the receiver reported \"%s\"", fl_str(p->error));
        if (cls == UM98X_FLASH_FAIL_TRANSPORT) {
            s->transfer_mark = p->prompt_after_error ? s->transfer_mark : fl_lis_mark(&s->lis);
            return post_fail(s, cls, true, false, why);
        }
        s->j.failure_class = cls;
        (void)put(s, UM98X_JOURNAL_FAILED);
        return needs_user(s, cls, "%s: not sent again automatically", why);
    }
    case FL_OUT_U:
        return needs_user(s, UM98X_FLASH_FAIL_UNCONFIRMED,
                          "the loader prompt came back without the success text: restart the receiver and check its "
                          "version, or send the same package again");
    case FL_OUT_N_APP: {
        gnss_versiona_t v;
        if (fl_app_check(&s->lis, s->j.app_baud, FL_APPCHECK_MS, &v)) {
            (void)put(s, UM98X_JOURNAL_VERIFYING);
            return ended(s, FL_SESS_DONE, &v, s->j.app_baud, "no report from the loader; the application answers: %s",
                         v.firmware);
        }
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER,
                          "the receiver gave no report and its application does not answer; power-cycle it, then "
                          "continue");
    }
    default:
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "the receiver is silent; power-cycle it, then continue");
    }
}

/* ---- the transfer ---------------------------------------------------------------- */

static bool before_first_frame(void *ctx, xm_mode_t mode)
{
    fl_sess_t *s = ctx;
    s->j.transfers++;
    s->j.last_acked_block = 0;
    s->j.failure_class = UM98X_FLASH_FAIL_NONE;
    if (!put(s, UM98X_JOURNAL_TRANSFERRING)) {
        s->j.transfers--;
        return false;   /* the point of no return is not crossed without its record */
    }
    s->out.ponr = true;
    s->out.xm_mode = (uint8_t)mode;
    s->t_first_frame = fl_now(s->io);
    s->prog.frames_acked = 0;
    s->prog.bytes_sent = 0;
    progress(s, UM98X_FLASH_TRANSFERRING);
    return true;
}

static void on_acked(void *ctx, uint32_t blocks, uint32_t bytes, uint32_t retries)
{
    fl_sess_t *s = ctx;
    s->prog.frames_acked = blocks;
    s->prog.bytes_sent = bytes;
    s->prog.retries = retries;
    s->prog.frames_sent = s->xm.frames_sent;
    if (blocks % FL_ACK_PERSIST_EVERY == 0) {
        s->j.last_acked_block = (uint16_t)(blocks > UINT16_MAX ? UINT16_MAX : blocks);
        (void)put(s, UM98X_JOURNAL_TRANSFERRING);   /* a failure here does not stop the transfer */
    }
    progress(s, UM98X_FLASH_TRANSFERRING);
}

static void before_eot(void *ctx)
{
    fl_sess_t *s = ctx;
    s->j.last_acked_block = (uint16_t)(s->xm.blocks_acked > UINT16_MAX ? UINT16_MAX : s->xm.blocks_acked);
    (void)put(s, UM98X_JOURNAL_EOT_SENT);
    progress(s, UM98X_FLASH_COMMITTING);
}

static int transfer(fl_sess_t *s)
{
    bool was_ponr = ponr(s);
    if (!transfers_left(s)) {
        return needs_user(s, s->j.failure_class, "%u transfers under this session: another one needs your confirmation",
                          (unsigned)s->j.transfers);
    }
    s->prog.frames_total = xm_blocks(s->pkg_len);
    s->prog.bytes_total = s->pkg_len;
    fl_lis_init(&s->post, s->io, s->lis.baud);
    s->post.tap = fl_lis_tap;
    s->post.tap_ctx = &s->lis;
    xm_job_t job = {
        .data = s->pkg,
        .len = s->pkg_len,
        .ctx = s,
        .before_first_frame = before_first_frame,
        .on_acked = on_acked,
        .before_eot = before_eot,
        .post_tap = fl_lis_tap,
        .post_ctx = &s->post,
        .stop_after = s->hooks.stop_after_blocks,
    };
    s->rx.tap = fl_lis_tap;
    s->rx.tap_ctx = &s->lis;
    xm_result_t x = xm_send(s->io, &s->rx, &job, &s->xm);
    s->rx.tap = NULL;
    s->out.xm = x;
    s->out.handshake_ms = s->xm.handshake_ms;
    s->prog.frames_sent = s->xm.frames_sent;
    if (s->xm.first_frame_sent) {
        s->out.transfer_ms = fl_since(s->io, s->t_first_frame);
    }
    switch (x) {
    case XM_OK:
        fl_notef(s->io, "transfer done: %u frames, %u retries, %u EOT%s", (unsigned)s->xm.blocks,
                 (unsigned)s->xm.retries, (unsigned)s->xm.eots, s->xm.eot_acked ? " acknowledged" : "");
        progress(s, UM98X_FLASH_COMMITTING);
        {
            fl_post_t p;
            fl_post_wait(&s->post, s->xm.last_eot_ms, s->j.app_baud, &p);
            return after_outcome(s, &p);
        }
    case XM_HANDSHAKE_FAILED: {
        if (was_ponr) {
            return post_fail(s, UM98X_FLASH_FAIL_TRANSPORT, true, false, "the receiver did not start the transfer");
        }
        fl_prompt_result_t pr = fl_after_handshake(&s->lis, s->transfer_mark);
        return attempt_failed(s, pr != FL_PROMPT_LEFT, pr == FL_PROMPT_LEFT, "the receiver did not start the transfer");
    }
    case XM_NOT_STARTED:
    case XM_REFUSED: {
        static const uint8_t can2[2] = { FL_CAN, FL_CAN };
        (void)fl_write(s->io, can2, sizeof(can2));
        if (was_ponr) {
            return needs_user(s, UM98X_FLASH_FAIL_JOURNAL, "the journal could not be written before the transfer");
        }
        fl_prompt_result_t pr = fl_after_handshake(&s->lis, s->transfer_mark);
        gnss_versiona_t v;
        if (pr != FL_PROMPT_LEFT && leave_early(s, &v)) {
            return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud,
                         "update not done: the journal could not be written before the first frame");
        }
        return finish_pre(s, UM98X_FLASH_FAIL_JOURNAL, "the journal could not be written before the first frame", true);
    }
    case XM_STOPPED: {
        /* Bench: watch what the receiver does when frames stop (listen only). */
        uint32_t t0 = fl_now(s->io);
        uint64_t m = fl_lis_mark(&s->lis);
        uint32_t first = UINT32_MAX;
        while (fl_since(s->io, t0) < BENCH_ABORT_WATCH_MS && !fl_lis_seen_after(&s->lis, FL_S_PROMPT, m)) {
            if (fl_lis_read(&s->lis, FL_SLICE_MS, NULL, 0) > 0 && first == UINT32_MAX) {
                first = fl_since(s->io, t0);
            }
        }
        fl_notef(s->io, "after stopping: first receiver byte %s%u ms, prompt %s after %u ms",
                 first == UINT32_MAX ? "none in " : "after ", (unsigned)(first == UINT32_MAX ? BENCH_ABORT_WATCH_MS : first),
                 fl_lis_seen_after(&s->lis, FL_S_PROMPT, m) ? "back" : "not back", (unsigned)fl_since(s->io, t0));
        return post_fail(s, UM98X_FLASH_FAIL_TRANSPORT, true, false, "stopped on purpose (bench)");
    }
    case XM_BAUD_MISMATCH:
        return post_fail(s, UM98X_FLASH_FAIL_TRANSPORT, true, true, xm_result_name(x));
    default:
        return post_fail(s, UM98X_FLASH_FAIL_TRANSPORT, true, false, xm_result_name(x));
    }
}

/* ---- the menu ----------------------------------------------------------------------- */

static bool menu_before_item(void *ctx)
{
    fl_sess_t *s = ctx;
    bool ok = put(s, UM98X_JOURNAL_MENU2_SENT);
    return ok || ponr(s);   /* after the point of no return a failed write does not stop anything */
}

static bool menu_copy_ok(void *ctx)
{
    fl_sess_t *s = ctx;
    return s->hooks.copy_ok == NULL || s->hooks.copy_ok(s->hooks.ctx);
}

static int at_prompt_menu(fl_sess_t *s, bool confirm)
{
    progress(s, UM98X_FLASH_ENTERING);
    fl_rx_init(&s->rx, s->io);
    s->rx.tap = fl_lis_tap;
    s->rx.tap_ctx = &s->lis;
    fl_menu_job_t job = { .confirm_prompt = confirm, .ctx = s, .before_item = menu_before_item, .copy_ok = menu_copy_ok };
    fl_menu_stats_t st;
    fl_menu_result_t r = fl_menu_download(&s->lis, &s->rx, &job, &st);
    s->rx.tap = NULL;
    s->transfer_mark = fl_lis_mark(&s->lis);
    if (r == FL_MENU_READY) {
        return transfer(s);
    }
    bool post = ponr(s);
    switch (r) {
    case FL_MENU_FAILED:
        if (post) {
            return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "the loader menu failed (%s); the receiver waits at its loader",
                              st.text);
        }
        return attempt_failed(s, true, false, st.text);
    case FL_MENU_SUBMENU: {
        if (post) {
            return needs_user(s, UM98X_FLASH_FAIL_ENTRY, "the loader asked which firmware to update; left the question");
        }
        gnss_versiona_t v;
        if (leave_early(s, &v)) {
            return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud,
                         "update not done: the loader asked which firmware to update (not supported yet)");
        }
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "the loader asked which firmware to update, and the receiver "
                                                         "did not come back");
    }
    case FL_MENU_COPY_CHANGED:
    case FL_MENU_JOURNAL: {
        const char *why = r == FL_MENU_JOURNAL ? "the journal could not be written" : "the package copy in RAM changed";
        if (post) {
            return needs_user(s, r == FL_MENU_JOURNAL ? UM98X_FLASH_FAIL_JOURNAL : UM98X_FLASH_FAIL_PACKAGE, "%s", why);
        }
        gnss_versiona_t v;
        if (leave_early(s, &v)) {
            return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud, "update not done: %s", why);
        }
        return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "update stopped (%s) and the receiver did not come back", why);
    }
    default:
        return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error at the loader menu");
    }
}

/* ---- entry -------------------------------------------------------------------------- */

/* One entry attempt (quiesce, loader baud, reset, stop key), then the menu. The application runs at the
 * application baud when it starts. */
static int entry_attempt(fl_sess_t *s, uint32_t boot)
{
    bool post = ponr(s);
    s->j.boot_baud = boot;
    progress(s, UM98X_FLASH_ENTERING);
    if (!post && s->hooks.cancelled != NULL && s->hooks.cancelled(s->hooks.ctx)) {
        return finish_pre(s, UM98X_FLASH_FAIL_NONE, "cancelled by the user", false);
    }
    if (!put(s, post ? UM98X_JOURNAL_FAILED : UM98X_JOURNAL_PREPARED) && !post) {
        return finish_pre(s, UM98X_FLASH_FAIL_JOURNAL, "the journal could not be written", false);
    }
    if (fl_lis_set_baud(&s->lis, s->j.app_baud) != 0 || fl_quiesce(&s->lis) != 0) {
        return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error while quiescing the receiver");
    }
    bool raised = false;
    if (s->j.app_baud != boot) {
        if (!post && !put(s, UM98X_JOURNAL_BAUD_CHANGING)) {
            return finish_pre(s, UM98X_FLASH_FAIL_JOURNAL, "the journal could not be written", false);
        }
        switch (fl_raise_baud(&s->lis, s->j.app_baud, boot)) {
        case FL_BAUD_OK:
            raised = true;
            break;
        case FL_BAUD_RESTORED:
            if (post) {
                return needs_user(s, UM98X_FLASH_FAIL_ENTRY, "the loader baud could not be verified");
            }
            return finish_pre(s, UM98X_FLASH_FAIL_ENTRY, "the loader baud could not be verified (COM1 restored)", false);
        case FL_BAUD_STRANDED:
            return needs_user(s, UM98X_FLASH_FAIL_ENTRY,
                              "receiver COM1 is at an unsaved baud; a receiver power cycle restores its saved baud");
        default:
            return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error while changing the baud");
        }
    }
    if (fl_pre_reset_quiet(&s->lis) != 0) {
        return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error before the reset");
    }
    bool cancel = !post && s->hooks.cancelled != NULL && s->hooks.cancelled(s->hooks.ctx);
    if (cancel || (!put(s, UM98X_JOURNAL_RESET_SENT) && !post)) {
        const char *why = cancel ? "cancelled by the user" : "the journal could not be written";
        if (raised) {
            fl_baud_result_t br = fl_restore_baud(&s->lis, s->j.app_baud, boot);
            if (br == FL_BAUD_STRANDED) {
                return needs_user(s, UM98X_FLASH_FAIL_ENTRY,
                                  "%s; receiver COM1 is at an unsaved baud (a power cycle restores it)", why);
            }
        }
        return finish_pre(s, cancel ? UM98X_FLASH_FAIL_NONE : UM98X_FLASH_FAIL_JOURNAL, why, false);
    }
    fl_entry_result_t e = fl_reset_and_stop(&s->lis, boot, &s->out.entry);
    if (e == FL_ENTRY_IO) {
        return needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error during the reset");
    }
    if (e != FL_ENTRY_PROMPT) {
        const char *why = e == FL_ENTRY_APP ? "the application restarted instead of the loader"
                          : e == FL_ENTRY_LATE ? "no loader prompt within 8 s of output"
                                               : "no loader prompt within 30 s";
        if (post) {
            return needs_user(s, UM98X_FLASH_FAIL_ENTRY, "%s (after the point of no return)", why);
        }
        return attempt_failed(s, false, e == FL_ENTRY_APP, why);
    }
    s->ever_prompt = true;
    s->at_prompt = true;
    return at_prompt_menu(s, true);
}

static int attempts(fl_sess_t *s)
{
    for (;;) {
        if (s->j.entry_attempts >= FL_ENTRY_ATTEMPTS) {
            return finish_pre(s, UM98X_FLASH_FAIL_ENTRY, "the loader was not reached", true);
        }
        uint32_t boot = s->loader_baud;
        if (s->fallback_115200 && s->j.entry_attempts > 0 && !s->ever_prompt) {
            boot = 115200u;
            fl_notef(s->io, "fallback attempt at 115200 baud");
        }
        s->j.entry_attempts++;
        int r = entry_attempt(s, boot);
        if (r != A_RETRY) {
            return r;
        }
    }
}

/* ---- public ----------------------------------------------------------------------------- */

void fl_sess_init(fl_sess_t *s, const fl_io_t *io, const uint8_t *pkg, uint32_t len, uint32_t loader_baud,
                  bool fallback_115200, const fl_sess_hooks_t *hooks)
{
    um98x_flash_journal_t j = s->j;
    memset(s, 0, sizeof(*s));
    s->j = j;
    s->io = io;
    s->pkg = pkg;
    s->pkg_len = len;
    s->loader_baud = loader_baud;
    s->fallback_115200 = fallback_115200;
    if (hooks != NULL) {
        s->hooks = *hooks;
    }
    fl_lis_init(&s->lis, io, s->j.app_baud != 0 ? s->j.app_baud : FL_BAUD_APP_DEFAULT);
    fl_rx_init(&s->rx, io);
    s->prog.bytes_total = len;
    s->prog.frames_total = len > 0 ? xm_blocks(len) : 0;
}

fl_sess_result_t fl_sess_run(fl_sess_t *s)
{
    s->j.version = 1;
    s->j.transfers = 0;
    s->j.entry_attempts = 0;
    s->j.last_acked_block = 0;
    s->j.failure_class = UM98X_FLASH_FAIL_NONE;
    s->j.boot_baud = s->loader_baud;
    if (!put(s, UM98X_JOURNAL_PREPARED)) {
        s->out.result = FL_SESS_NOT_STARTED;
        snprintf(s->out.reason, sizeof(s->out.reason), "the journal could not be opened: nothing sent");
        return FL_SESS_NOT_STARTED;
    }
    return (fl_sess_result_t)attempts(s);
}

fl_sess_result_t fl_sess_user_retry(fl_sess_t *s)
{
    if (s->j.transfers >= FL_TRANSFERS_MAX) {
        s->user_transfers = (uint32_t)s->j.transfers + 1u - FL_TRANSFERS_MAX;
    }
    s->j.entry_attempts = 0;
    uint32_t boot = s->j.boot_baud != 0 ? s->j.boot_baud : s->loader_baud;
    s->j.boot_baud = boot;
    progress(s, UM98X_FLASH_ENTERING);
    /* The retry has no RAM copy of its own: the pinned package goes through
     * the recovery gate first, before anything is sent, so every path below
     * (the prompt or the application, before or after the point of no
     * return) reaches the menu with the copy it sends. */
    if (!copy_ready(s)) {
        return (fl_sess_result_t)needs_user(s, s->j.failure_class,
                                            "the staged package is not available for a retry; nothing was sent");
    }
    /* Where is the receiver? The loader prompt first (listen, one "\r\n"). */
    if (fl_lis_set_baud(&s->lis, boot) != 0) {
        return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error");
    }
    uint64_t m = fl_lis_mark(&s->lis);
    fl_prompt_result_t pr = fl_wait_prompt(&s->lis, m, FL_PROMPT_MS, FL_NUDGE_MS);
    if (pr == FL_PROMPT_SEEN) {
        s->at_prompt = true;
        s->ever_prompt = true;
        if (ponr(s)) {
            return (fl_sess_result_t)retry_from_prompt(s);
        }
        s->j.entry_attempts = 1;
        int r = at_prompt_menu(s, true);
        return (fl_sess_result_t)(r == A_RETRY ? attempts(s) : r);
    }
    gnss_versiona_t v;
    if (!fl_app_check(&s->lis, s->j.app_baud, FL_APPCHECK_MS, &v)) {
        return (fl_sess_result_t)needs_user(s, s->j.failure_class, "the receiver was not found at its loader or its "
                                                                   "application; power-cycle it, then try again");
    }
    if (!model_ok(s, &v)) {
        return (fl_sess_result_t)needs_user(s, s->j.failure_class, "the receiver now reports model \"%s\"", v.model);
    }
    if (ponr(s)) {
        if (!transfers_left(s)) {
            return (fl_sess_result_t)needs_user(s, s->j.failure_class, "no transfer left");
        }
        s->j.entry_attempts = 1;
        return (fl_sess_result_t)entry_attempt(s, boot);
    }
    return (fl_sess_result_t)attempts(s);
}

fl_sess_result_t fl_sess_user_restart(fl_sess_t *s)
{
    uint32_t boot = s->j.boot_baud != 0 ? s->j.boot_baud : s->loader_baud;
    s->j.boot_baud = boot;
    if (fl_lis_set_baud(&s->lis, boot) != 0) {
        return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error");
    }
    return (fl_sess_result_t)leave_and_confirm(s);
}

/* APP-CHECK of the recovery table, then the exit checks (the station's part). */
static int recover_app_check(fl_sess_t *s, uint32_t listen_ms, const char *none_why)
{
    gnss_versiona_t v;
    if (fl_app_check(&s->lis, s->j.app_baud, listen_ms, &v)) {
        if (ponr(s)) {
            (void)put(s, UM98X_JOURNAL_VERIFYING);
        }
        return ended(s, ponr(s) ? FL_SESS_DONE : FL_SESS_ABORTED, &v, s->j.app_baud, "receiver application answers: %s",
                     v.firmware);
    }
    return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "%s", none_why);
}

/* At the prompt during recovery: retry (after the point of no return) or leave. */
static int recover_at_prompt(fl_sess_t *s)
{
    s->at_prompt = true;
    if (ponr(s)) {
        bool retryable = s->j.state == UM98X_JOURNAL_TRANSFERRING || s->j.failure_class == UM98X_FLASH_FAIL_TRANSPORT ||
                         s->j.failure_class == UM98X_FLASH_FAIL_NONE;
        if (!retryable) {
            return needs_user(s, s->j.failure_class, "at the loader prompt after a failure that is not retried");
        }
        if (s->j.state != UM98X_JOURNAL_FAILED) {
            if (s->j.failure_class == UM98X_FLASH_FAIL_NONE) {
                s->j.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
            }
            (void)put(s, UM98X_JOURNAL_FAILED);
        }
        return retry_from_prompt(s);
    }
    gnss_versiona_t v;
    if (leave_early(s, &v)) {
        return ended(s, FL_SESS_ABORTED, &v, s->j.app_baud, "interrupted update ended at the loader; receiver unchanged");
    }
    return needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER, "left the loader, but the application does not answer");
}

/* Listen-only at the loader baud: start bytes are answered with CAN CAN. */
static fl_prompt_result_t loader_listen(fl_sess_t *s, uint32_t ms, bool *start_bytes)
{
    fl_rx_init(&s->rx, s->io);
    s->rx.tap = fl_lis_tap;
    s->rx.tap_ctx = &s->lis;
    uint64_t m = fl_lis_mark(&s->lis);
    uint32_t t0 = fl_now(s->io);
    *start_bytes = false;
    fl_lis_evidence_reset(&s->lis);
    for (;;) {
        if (fl_lis_seen_after(&s->lis, FL_S_PROMPT, m)) {
            s->rx.tap = NULL;
            return FL_PROMPT_SEEN;
        }
        if (fl_lis_app_evidence(&s->lis)) {
            s->rx.tap = NULL;
            return FL_PROMPT_LEFT;
        }
        uint32_t spent = fl_since(s->io, t0);
        if (spent >= ms) {
            s->rx.tap = NULL;
            return FL_PROMPT_TIMEOUT;
        }
        fl_ev_t ev = fl_rx_next(&s->rx, t0 + ms);
        if (ev == FL_EV_C || ev == FL_EV_NAK) {
            static const uint8_t can2[2] = { FL_CAN, FL_CAN };
            *start_bytes = true;
            fl_notef(s->io, "the loader waits for a transfer: CAN CAN");
            (void)fl_write(s->io, can2, sizeof(can2));
            s->rx.tap = NULL;
            fl_prompt_result_t pr = fl_wait_prompt(&s->lis, m, FL_PROMPT_MS, FL_NUDGE_MS);
            return pr;
        }
        if (ev == FL_EV_MISMATCH) {
            s->rx.tap = NULL;
            return FL_PROMPT_LEFT;
        }
    }
}

fl_sess_result_t fl_sess_recover(fl_sess_t *s, fl_plan_t plan, bool race_prompt)
{
    uint32_t boot = s->j.boot_baud != 0 ? s->j.boot_baud : s->loader_baud;
    s->j.boot_baud = boot;
    s->out.ponr = s->j.transfers > 0 || s->j.phase >= UM98X_JOURNAL_TRANSFERRING;
    fl_notef(s->io, "recovery of an interrupted update (%s): %s", flash_journal_state_name(s->j.state),
             flash_journal_plan_text(plan));
    progress(s, UM98X_FLASH_ENTERING);
    switch (plan) {
    case FL_PLAN_RESTORE_CHECK: {
        fl_baud_result_t br = fl_restore_baud(&s->lis, s->j.app_baud, boot);
        if (br == FL_BAUD_STRANDED) {
            return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_ENTRY,
                                                "receiver COM1 is at an unsaved baud; a receiver power cycle restores "
                                                "its saved baud");
        }
        if (br == FL_BAUD_RESTORED) {
            /* The restore's version query answered at the application baud. */
            return (fl_sess_result_t)ended(s, FL_SESS_ABORTED, &s->lis.ver, s->j.app_baud,
                                           "interrupted update ended before the loader; receiver unchanged (%s)",
                                           s->lis.ver.firmware);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_MS, "the receiver does not answer");
    }
    case FL_PLAN_APP_CHECK:
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_MS, "the receiver does not answer");
    case FL_PLAN_LOADER_PEEK: {
        bool sb;
        if (fl_lis_set_baud(&s->lis, boot) != 0) {
            return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error");
        }
        fl_prompt_result_t pr = loader_listen(s, 3000, &sb);
        if (pr == FL_PROMPT_TIMEOUT) {
            uint64_t m = fl_lis_mark(&s->lis);
            (void)fl_write_str(s->io, "\r\n");
            fl_lis_listen(&s->lis, 3000);
            pr = fl_lis_seen_after(&s->lis, FL_S_PROMPT, m) ? FL_PROMPT_SEEN : FL_PROMPT_TIMEOUT;
        }
        if (pr == FL_PROMPT_SEEN) {
            return (fl_sess_result_t)recover_at_prompt(s);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_MS, "the receiver does not answer");
    }
    case FL_PLAN_RACE_LEAVE:
        if (race_prompt) {
            return (fl_sess_result_t)recover_at_prompt(s);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_MS, "the receiver does not answer");
    case FL_PLAN_LISTEN_TRANSFER: {
        bool sb;
        if (fl_lis_set_baud(&s->lis, boot) != 0) {
            return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error");
        }
        /* T2 only while a transfer may still run; a failed one (also an
         * entry state after a transfer: a re-entry that was cut short) left
         * the receiver out of the transfer already. */
        uint32_t ms = flash_journal_effective_state(&s->j) == UM98X_JOURNAL_FAILED ? FL_APPCHECK_MS : FL_T2_MS;
        fl_prompt_result_t pr = loader_listen(s, ms, &sb);
        if (pr == FL_PROMPT_TIMEOUT) {
            uint64_t m = fl_lis_mark(&s->lis);
            (void)fl_write_str(s->io, "\r\n");
            fl_lis_listen(&s->lis, FL_POST_NUDGE_WAIT_MS);
            pr = fl_lis_seen_after(&s->lis, FL_S_PROMPT, m) ? FL_PROMPT_SEEN : FL_PROMPT_TIMEOUT;
        }
        if (pr == FL_PROMPT_SEEN) {
            return (fl_sess_result_t)recover_at_prompt(s);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_MS, "the receiver does not answer");
    }
    case FL_PLAN_RACE_RETRY:
        if (race_prompt) {
            return (fl_sess_result_t)recover_at_prompt(s);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_LONG_MS,
                                                   "the receiver does not answer; power-cycle it again");
    case FL_PLAN_LISTEN_EOT: {
        if (fl_lis_set_baud(&s->lis, boot) != 0) {
            return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_TRANSPORT, "link error");
        }
        fl_lis_init(&s->post, s->io, boot);
        s->post.tap = fl_lis_tap;
        s->post.tap_ctx = &s->lis;
        s->transfer_mark = fl_lis_mark(&s->lis);
        fl_post_t p;
        fl_post_wait(&s->post, fl_now(s->io), s->j.app_baud, &p);
        return (fl_sess_result_t)after_outcome(s, &p);
    }
    case FL_PLAN_VERIFY_CHECK: {
        gnss_versiona_t v;
        if (fl_app_check(&s->lis, s->j.app_baud, FL_APPCHECK_LONG_MS, &v)) {
            return (fl_sess_result_t)ended(s, FL_SESS_DONE, &v, s->j.app_baud, "receiver application answers: %s",
                                           v.firmware);
        }
        (void)fl_lis_set_baud(&s->lis, boot);
        uint64_t m = fl_lis_mark(&s->lis);
        if (fl_wait_prompt(&s->lis, m, FL_APPCHECK_MS, FL_NUDGE_MS) == FL_PROMPT_SEEN) {
            return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_APP_NOT_STARTING,
                                                "the loader answers but the application does not start");
        }
        return (fl_sess_result_t)needs_user(s, UM98X_FLASH_FAIL_NO_ANSWER,
                                            "the receiver is silent after the update; power-cycle it");
    }
    case FL_PLAN_RACE_VERIFY:
        if (race_prompt) {
            return (fl_sess_result_t)leave_and_confirm(s);
        }
        return (fl_sess_result_t)recover_app_check(s, FL_APPCHECK_LONG_MS,
                                                   "the receiver does not answer; power-cycle it again");
    case FL_PLAN_RACE_STOP:
        s->out.result = FL_SESS_NEEDS_USER;
        snprintf(s->out.reason, sizeof(s->out.reason), "%s",
                 race_prompt ? "at the loader prompt, waiting for the user" : "the loader prompt was not reached");
        return FL_SESS_NEEDS_USER;
    case FL_PLAN_CHECK_WAIT: {
        gnss_versiona_t v;
        bool app = fl_app_check(&s->lis, s->j.app_baud, FL_APPCHECK_MS, &v);
        s->out.result = FL_SESS_NEEDS_USER;
        snprintf(s->out.reason, sizeof(s->out.reason), "%s%s", app ? "the application answers: " : "the receiver does not answer",
                 app ? v.firmware : "");
        return FL_SESS_NEEDS_USER;
    }
    default:
        s->out.result = FL_SESS_NEEDS_USER;
        snprintf(s->out.reason, sizeof(s->out.reason), "waiting for the user");
        return FL_SESS_NEEDS_USER;
    }
}
