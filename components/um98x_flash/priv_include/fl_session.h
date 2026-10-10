/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * One receiver flash session as a sequence of protocol steps (pure logic,
 * host-tested end to end against a simulated receiver loader).
 *
 * The journal (um98x_flash_journal_t) is written through a callback before
 * every action that starts a new state, so after a reset the receiver may be
 * on either side of that action. A failed write before "transferring" ends
 * the session before the point of no return; the "transferring" write must
 * succeed before the first frame; later failed writes do not stop anything
 * (the stored state stays on the conservative side).
 *
 * Entry: up to FL_ENTRY_ATTEMPTS attempts per session (a failed handshake is
 * one). Each: quiesce, the loader baud (verified; restored and the session
 * ended when it cannot be verified), the reset and the stop key, the menu.
 * With the 115200 fallback built in, an attempt that follows one which never
 * saw the loader prompt may run at 115200 (and skips the baud change); once
 * the prompt was seen, the remaining attempts stay at the loader baud.
 * Before the point of no return every failure leaves the loader with "6"
 * and the application is confirmed; the session then ends through the abort
 * path (the station re-applies its profile and closes the journal).
 *
 * After the first frame (the point of no return): a transfer failure is
 * recorded ("failed" with its class) before anything else, the loader prompt
 * is awaited (one "\r\n" after FL_POST_FAIL_NUDGE_MS of silence), and a
 * transport failure is retried from the menu with the same RAM copy while
 * the journal has had fewer than FL_TRANSFERS_MAX transfers. "6" is never
 * sent after a failure past that point. If the receiver left the loader and
 * its application answers with the journalled model, the session enters the
 * loader again (same package). Everything else stops in "needs-user" and
 * nothing more is sent.
 *
 * After the transfer, the outcome table decides: success (also late) leaves
 * the loader and confirms the application with its version; a hard error is
 * retried only when it is a transport report; a prompt without the success
 * text, a silent receiver, an application that does not start: needs-user.
 *
 * PROVISIONAL, to be confirmed on hardware: the whole receiver side.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fl_io.h"
#include "fl_proto.h"
#include "fl_rx.h"
#include "flash_journal.h"
#include "gnss_types.h"
#include "xmodem_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FL_ENTRY_ATTEMPTS    3u
#define FL_TRANSFERS_MAX     3u    /**< per journal: the user's first plus two automatic retries */
#define FL_ACK_PERSIST_EVERY 64u   /**< journal the last acknowledged block this often */

typedef enum {
    FL_SESS_DONE = 0,       /**< the application answered after a transfer (out.ver): re-apply, compare, close */
    FL_SESS_ABORTED,        /**< ended before the point of no return, application confirmed: re-apply, check, close */
    FL_SESS_NEEDS_USER,     /**< the journal now waits for the user (out.reason); the receiver is left as it is */
    FL_SESS_NOT_STARTED,    /**< the first journal write failed: nothing was sent, no journal exists */
} fl_sess_result_t;

typedef struct {
    void *ctx;
    /** Persist the journal; false = the write failed. */
    bool (*journal_put)(void *ctx, const um98x_flash_journal_t *j);
    /** Recompute the RAM copy's SHA-256 and compare with the journal's. */
    bool (*copy_ok)(void *ctx);
    /** Recovery gate before a transfer started from recovery: the pinned package checked again into RAM. */
    bool (*recovery_prepare)(void *ctx);
    /** The user cancelled (honoured only before the reset). */
    bool (*cancelled)(void *ctx);
    /** Progress snapshot (phase, bytes, counters). */
    void (*progress)(void *ctx, const um98x_flash_progress_t *p);
    /** Unix time for the journal, 0 without a trusted clock. */
    int64_t (*unix_now)(void *ctx);
    /** Bench: stop sending after this many acknowledged blocks (0 = never). */
    uint32_t stop_after_blocks;
} fl_sess_hooks_t;

typedef struct {
    fl_sess_result_t result;
    uint8_t failure;          /**< um98x_flash_failure_t of the last failure */
    char reason[96];          /**< what happened, for the log and the UI */
    gnss_versiona_t ver;      /**< FL_SESS_DONE / FL_SESS_ABORTED: the application's version log */
    uint32_t app_baud_found;
    bool ponr;                /**< a data frame went out under this journal (now or before) */
    bool outcome_valid;
    fl_outcome_t outcome;     /**< of the last transfer */
    xm_result_t xm;           /**< of the last transfer */
    uint32_t handshake_ms;
    uint8_t xm_mode;          /**< xm_mode_t of the last transfer */
    uint32_t transfer_ms;     /**< first frame to the last ACK */
    uint32_t success_ms;      /**< last EOT to the success text */
    uint32_t prompt_ms;       /**< last EOT to the prompt */
    fl_entry_stats_t entry;   /**< of the last entry attempt */
} fl_sess_out_t;

typedef struct {
    /* set by the caller */
    const fl_io_t *io;
    const uint8_t *pkg;       /**< the verified RAM copy */
    uint32_t pkg_len;
    uint32_t loader_baud;     /**< UM98X_FLASH_BAUD */
    bool fallback_115200;     /**< build option */
    um98x_flash_journal_t j;  /**< the journal (fields of the session; state written by the session) */
    fl_sess_hooks_t hooks;
    /* work state */
    fl_lis_t lis;
    fl_lis_t post;
    fl_rx_t rx;
    xm_state_t xm;
    um98x_flash_progress_t prog;
    bool ever_prompt;         /**< the loader prompt was seen in this session */
    bool at_prompt;           /**< it is the last thing known */
    uint32_t user_transfers;  /**< transfers the user allowed beyond the automatic cap */
    uint64_t transfer_mark;
    uint32_t t_first_frame;
    fl_sess_out_t out;
} fl_sess_t;

/** Prepare a session object (zeroes the work state, keeps nothing). */
void fl_sess_init(fl_sess_t *s, const fl_io_t *io, const uint8_t *pkg, uint32_t len, uint32_t loader_baud,
                  bool fallback_115200, const fl_sess_hooks_t *hooks);

/**
 * A new session: s->j holds the identity fields (SHA-256, size, expected
 * firmware and build, pre-flash model, firmware and build, application
 * baud); the receiver runs its application at that baud. Writes "prepared"
 * first (FL_SESS_NOT_STARTED when that fails).
 */
fl_sess_result_t fl_sess_run(fl_sess_t *s);

/**
 * The user's "retry flash (same package)" from needs-user (one more
 * transfer, beyond the automatic cap). Without a RAM copy (s->pkg NULL) the
 * pinned package goes through the recovery gate (hooks.recovery_prepare)
 * first; if it fails, needs-user again and nothing is sent. Then back to the
 * loader prompt (listen, one "\r\n"), or, with the application answering
 * with the journalled model, a full loader entry; then the menu and the
 * transfer.
 */
fl_sess_result_t fl_sess_user_retry(fl_sess_t *s);

/** The user's "restart receiver": leave the loader ("6") and the exit checks. */
fl_sess_result_t fl_sess_user_restart(fl_sess_t *s);

/**
 * Boot-time recovery (actions enabled): run the plan of the recovery table
 * for the open journal in s->j. race_prompt: the early boot race saw the
 * loader prompt (plans that race).
 */
fl_sess_result_t fl_sess_recover(fl_sess_t *s, fl_plan_t plan, bool race_prompt);

#ifdef __cplusplus
}
#endif
