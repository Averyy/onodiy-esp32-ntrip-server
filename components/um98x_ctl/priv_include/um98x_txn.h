/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * One command in flight (pure logic, no IDF includes, injected clock,
 * host-testable).
 *
 * A transaction covers every send of one command: it says when to write,
 * consumes the reply lines that belong to it and decides when the command
 * is done. All times are milliseconds of a monotonic clock supplied by the
 * caller; "end of the write" is the time passed to um98x_txn_sent().
 *
 * Kinds and their rules (values PROVISIONAL, to be confirmed on hardware):
 *  - COMMAND: done on its acknowledgement. Ack timeout 3000 ms from the end
 *    of the write (5000 ms for SAVECONFIG, set by the caller). On a timeout
 *    it is resent 1000 ms after the timeout expired, up to 4 sends in total;
 *    on a negative reply ("PARSING FAILED ...") it is resent once, 100 ms
 *    later, and a second negative reply is final (MODE and CONFIG alike;
 *    the version kinds below keep their own schedule and only record the
 *    reason, finishing REJECTED instead of TIMEOUT when no log came). A late
 *    acknowledgement of an earlier send completes it (every send carries the
 *    same text). With restart_ends set (a command that restarts the
 *    receiver), the restart banner also ends it (status RESTARTED).
 *  - MODE: the base-mode query. Done on the base-mode log (the ack comes
 *    first and does not complete it); timeouts and retries as COMMAND.
 *  - CONFIG: the configuration dump. The ack, then one line per item with
 *    no end marker: done 500 ms after the last item line, and at the latest
 *    3000 ms after the ack. No item line within 3000 ms counts as a timeout
 *    (resent as COMMAND). A resend starts the read-back over.
 *  - VERSION: the version query, sent with a leading CR LF. One try = one
 *    write and a 2000 ms window from the end of the write; the next try
 *    starts 1000 ms after the window ended; up to 3 tries (max_sends). A
 *    CRC-valid version log completes it whenever it arrives, also late. The
 *    acknowledgement alone does not ("not ready", see ack_seen).
 *  - VERSION_RESTART: the version query after a known receiver restart: a
 *    write every 1000 ms, until a version log arrives or until end_ms
 *    (restart evidence + 20 s).
 *  - PROBE: detection probe: the version query with a leading CR LF, done
 *    on a version log, on a negative acknowledgement or on the receiver's
 *    restarting answer (a receiver is at this baud); 2 sends, 2000 ms
 *    each. After an accepting acknowledgement the version log that follows
 *    it has UM98X_PROBE_LOG_MS, then the probe is done all the same (OK:
 *    the receiver answered). Waiting for it gives the caller the identity
 *    and keeps the log line whole: the caller releases its lease right
 *    after the probe, and a release drops the input still pending and
 *    resets the framer, which cut the log that was arriving (every boot
 *    that detected the receiver discarded those bytes, one version log).
 *  - GROUPS: the signal-group query (ANT1GROUPA, one-shot: no rate). Done
 *    on the main antenna's signal-group log; the ack comes first. After an
 *    accepting ack the log has 1000 ms, then the query ends as a timeout
 *    without a resend (the receiver knows the command: its answer was not
 *    read). A negative reply is final at once (a firmware without the
 *    query refuses every send alike). No ack: 2 sends, 3000 ms each, 1000
 *    ms apart. Never fatal to a session.
 *
 * The receiver's restarting answer ("The Board is resetting,...") carries
 * no echo: it answers whatever was sent while the receiver restarts. It is
 * taken by the transaction in flight: it ends a PROBE (OK) and a command
 * that restarts the receiver (restart_ends: RESTARTED, the restart is under
 * way); every other kind only notes it (restarting) and keeps its
 * schedule, so the command goes out again after the restart.
 *
 * The first write waits for not_before (pacing: 100 ms after the previous
 * reply line).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_framing.h"
#include "um98x_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_ACK_TIMEOUT_MS            3000
#define UM98X_SAVE_TIMEOUT_MS           5000
#define UM98X_RETRY_GAP_MS              1000
#define UM98X_MAX_SENDS                 4
#define UM98X_MAX_REJECTS               2
#define UM98X_PACING_MS                 100
#define UM98X_CONFIG_QUIET_MS           500
#define UM98X_CONFIG_MAX_MS             3000
#define UM98X_VERSION_WINDOW_MS         2000
#define UM98X_VERSION_GAP_MS            1000
#define UM98X_VERSION_TRIES             3
#define UM98X_VERSION_RESTART_PERIOD_MS 1000
#define UM98X_VERSION_RESTART_SPAN_MS   20000
#define UM98X_PROBE_WINDOW_MS           2000
#define UM98X_PROBE_SENDS               2
#define UM98X_PROBE_LOG_MS              1000
#define UM98X_GROUPS_SENDS              2
#define UM98X_GROUPS_LOG_MS             1000
#define UM98X_GROUPS_BODY_MAX           96

#define UM98X_NEVER INT64_MAX

typedef enum {
    UM98X_TXN_COMMAND = 0,
    UM98X_TXN_MODE,
    UM98X_TXN_CONFIG,
    UM98X_TXN_VERSION,
    UM98X_TXN_VERSION_RESTART,
    UM98X_TXN_PROBE,
    UM98X_TXN_GROUPS,
} um98x_txn_kind_t;

typedef enum {
    UM98X_TXN_PENDING = 0,
    UM98X_TXN_OK,          /**< accepted / data complete */
    UM98X_TXN_RESTARTED,   /**< a restart banner ended a command that restarts the receiver */
    UM98X_TXN_REJECTED,    /**< negative reply, final */
    UM98X_TXN_TIMEOUT,     /**< nothing (complete) after the last send */
    UM98X_TXN_LINK_ERROR,  /**< the write failed */
} um98x_txn_status_t;

typedef enum {
    UM98X_TXN_ACT_SEND = 0, /**< write now, then um98x_txn_sent() */
    UM98X_TXN_ACT_WAIT,     /**< wait for a line or until *wake_ms */
    UM98X_TXN_ACT_DONE,
} um98x_txn_action_t;

typedef struct {
    um98x_txn_kind_t kind;
    char cmd[UM98X_CMD_MAX];
    uint32_t timeout_ms;        /**< per-send reply timeout (COMMAND, MODE, CONFIG) */
    uint8_t max_sends;
    bool restart_ends;          /**< the restart banner ends it (COMMAND) */
    um98x_txn_status_t status;
    uint8_t sends;
    uint8_t rejects;
    bool ack_seen;              /**< an acknowledgement for it arrived */
    bool restarting;            /**< the receiver answered with its restarting notice */
    int64_t first_sent_ms;      /**< end of the first write, UM98X_NEVER before it */
    int64_t next_send_ms;       /**< next write due, UM98X_NEVER = none planned */
    int64_t deadline_ms;        /**< end of the current reply window, UM98X_NEVER = none */
    int64_t end_ms;             /**< VERSION_RESTART: give up at */
    int64_t ack_ms;             /**< CONFIG: acknowledgement (or first item) time */
    int64_t last_item_ms;       /**< CONFIG: last item line */
    int64_t done_ms;            /**< when it finished */
    char reason[UM98X_REASON_MAX]; /**< negative reply text */
    um98x_config_readback_t config; /**< CONFIG: parsed items */
    char mode_body[UM98X_MODE_BODY_MAX]; /**< MODE: log body */
    char groups_body[UM98X_GROUPS_BODY_MAX]; /**< GROUPS: log body (cut to fit) */
    gnss_versiona_t version;    /**< VERSION, VERSION_RESTART, PROBE: parsed log */
    bool version_valid;
    char *text;                 /**< optional: reply line(s), '\n'-separated */
    size_t text_cap;
    size_t text_len;
} um98x_txn_t;

/**
 * Start a transaction. cmd must already be normalised (no CR/LF). text may
 * be NULL. Default timeout and send count for the kind; override with the
 * fields before the first poll.
 */
void um98x_txn_begin(um98x_txn_t *t, um98x_txn_kind_t kind, const char *cmd, int64_t not_before_ms, char *text,
                     size_t text_cap);

/** VERSION_RESTART: end of the polling (restart evidence + 20 s). */
void um98x_txn_set_end(um98x_txn_t *t, int64_t end_ms);

/** What to do now. *wake_ms is set for WAIT (UM98X_NEVER cannot happen: every state has a deadline). */
um98x_txn_action_t um98x_txn_poll(um98x_txn_t *t, int64_t now_ms, int64_t *wake_ms);

/** End a pending transaction now (status TIMEOUT), e.g. when detection must stop sending. */
void um98x_txn_abort(um98x_txn_t *t, int64_t now_ms);

/** True if the write starts with an extra CR LF (version kinds). */
bool um98x_txn_leading_crlf(const um98x_txn_t *t);

/** The write finished at now_ms (end of the write); ok = false if it failed. */
void um98x_txn_sent(um98x_txn_t *t, int64_t now_ms, bool ok);

/**
 * Offer a classified line. @return true if it belonged to this transaction
 * (consumed), false otherwise (another echo, unrelated line).
 */
bool um98x_txn_on_line(um98x_txn_t *t, const char *line, size_t len, const um98x_line_t *l, int64_t now_ms);

#ifdef __cplusplus
}
#endif
