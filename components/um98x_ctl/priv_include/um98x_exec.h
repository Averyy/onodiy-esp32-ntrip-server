/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Command channel and session executor (pure logic, no IDF includes,
 * host-testable against a simulated receiver).
 *
 * The channel is the state shared between the receive path and the task
 * that runs commands: the transaction in flight, the restart evidence and
 * the counters. Received lines are offered with um98x_chan_dispatch() (in the
 * firmware from the GNSS RX task, under the port's lock); the executor runs
 * transactions, detection and apply sessions through a port: write, host
 * baud, clock, wait, link counters, all behind callbacks, so the same code
 * runs on the device and in the host tests.
 *
 * Restart evidence: a restart banner, unless our own restart (signal group,
 * RESET rollback) is under way or the banner ended the command that caused
 * it; or a CRC-valid version log nobody waits for when no version query was
 * written in the preceding 10 s (measured from the end of that write), so a
 * late reply to our own retried query never counts.
 *
 * Every line of the command layer (acknowledgement, dump item, banner, log)
 * that passes its checksum counts as "the receiver answers"
 * (last_answer_ms), also while a lease is held.
 *
 * Detection (one pass over a candidate list): per baud, switch the host
 * baud, listen 1500 ms (at least one CRC-valid RTCM frame or two
 * checksum-valid text lines = found, nothing sent); otherwise probe with the
 * version query (an acknowledgement or a version log = found). A loader
 * prompt reported by the port stops the pass at once and nothing more is
 * sent. Bytes at any baud during the pass are reported (saw_bytes).
 *
 * Apply session (profile -> receiver; see um98x_plan.h for the order and the
 * only-on-difference rules):
 *  - identity first: an unknown identity is queried before anything is
 *    configured, and only the supported model is configured (nothing is
 *    sent otherwise); a known one is confirmed by a version query right
 *    after UNLOG COM1, so the applied state always records the firmware of
 *    this session;
 *  - CONFIG SIGNALGROUP restarts the receiver: wait up to 15 s for its
 *    banner, then poll the version every second until 20 s after the
 *    restart evidence. Nothing within 20 s of the acknowledgement: the
 *    receiver came back at its saved baud, so detection runs inside the same
 *    session (short list, then full list) and the session continues there;
 *    not found = the receiver is lost (nothing more can be sent). No
 *    acknowledgement at all (the restart may swallow it, and a receiver back
 *    at another baud answers no resend) is handled the same way, timed from
 *    the first send; only a negative reply fails the step. Then UNLOG COM1
 *    again (the restart restored the saved outputs) and the read-back again:
 *    the signal group must now match;
 *  - baud change: CONFIG COM1 <new> without waiting for its acknowledgement
 *    (the link itself changes), 200 ms, host baud, 100 ms, then a version
 *    log within 2000 ms (one retry). Failure: CONFIG COM1 <old> at the new
 *    baud, host back, version again; still nothing: short-list detection.
 *    Never saved on this path;
 *  - a fatal failure stops the session (no SAVECONFIG). If any command was
 *    accepted, RESET (a plain restart to the saved configuration): the host
 *    goes back to the saved baud, then version polling for 20 s; nothing
 *    there: 10 s more at the baud used before the RESET. answer_baud is
 *    where the receiver last answered (a detection inside the session, then
 *    the rollback: its answer, or unknown without one).
 *
 * Receiver reset (um98x_exec_reset, a recovery step): RESET written once,
 * never resent (a resend would reset it again), with the restart marked as
 * our own until its version log arrives, so neither its banner nor that log
 * counts as an unsolicited restart.
 *
 * Status: timings and texts PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_framing.h"
#include "gnss_types.h"
#include "um98x_apply.h"
#include "um98x_plan.h"
#include "um98x_reply.h"
#include "um98x_txn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_NONE_MS                 INT64_MIN  /**< "never happened" */
#define UM98X_UNSOLICITED_VERSION_MS  10000      /**< own version write this recent: a version log is a late reply */
#define UM98X_LISTEN_MS               1500       /**< detection: listen per baud before probing */
#define UM98X_LISTEN_POLL_MS          100
#define UM98X_RESTART_BANNER_MS       15000      /**< signal group: wait for the restart banner */
#define UM98X_RESTART_LOST_MS         20000      /**< signal group: nothing within this of the ack -> detection */
#define UM98X_BAUD_SETTLE_MS          200        /**< after CONFIG COM1 <new>, before the host switches */
#define UM98X_BAUD_VERIFY_DELAY_MS    100        /**< after the host switch, before the version query */
#define UM98X_BAUD_VERIFY_TRIES       2
#define UM98X_ROLLBACK_SAVED_MS       20000      /**< after RESET: version polling at the saved baud */
#define UM98X_ROLLBACK_PREVIOUS_MS    10000      /**< then at the baud used before the RESET */
#define UM98X_LOADER_WAIT_MS          15000      /**< after the loader's restart key: wait for the application */

typedef struct {
    uint64_t lines;       /**< checksum-valid command-layer lines (acks, dump items, banners, logs) */
    uint64_t matched;     /**< consumed by the request in flight */
    uint64_t unmatched;   /**< nothing waiting for them, or an acknowledgement of another command */
    uint64_t bad;         /**< command-layer prefix with a wrong checksum or form */
    uint64_t other;       /**< NMEA and other lines (not for this layer) */
    uint64_t banners;     /**< restart banners */
    uint64_t restarts;    /**< unsolicited restart evidence */
} um98x_chan_stats_t;

typedef struct {
    um98x_txn_t txn;
    bool txn_active;
    bool expect_restart;          /**< our own receiver restart is under way */
    int64_t last_reply_ms;        /**< last line consumed by a request */
    int64_t last_answer_ms;       /**< last checksum-valid command-layer line (liveness; pacing: next write 100 ms later) */
    int64_t last_version_write_ms;/**< end of our last version-query write */
    int64_t last_banner_ms;
    int64_t last_restart_ms;      /**< last unsolicited restart evidence */
    um98x_chan_stats_t stats;
} um98x_chan_t;

/** um98x_chan_dispatch() result bits. */
#define UM98X_CHAN_WAKE    0x1u   /**< the waiting executor should look (consumed line or banner) */
#define UM98X_CHAN_RESTART 0x2u   /**< unsolicited restart evidence */

void um98x_chan_init(um98x_chan_t *ch);

/** Offer one classified line (caller holds the lock). @return UM98X_CHAN_* bits. */
unsigned um98x_chan_dispatch(um98x_chan_t *ch, const char *line, size_t len, const um98x_line_t *l, int64_t now_ms);

typedef struct {
    uint64_t rtcm_frames;  /**< CRC-valid RTCM frames framed (monotonic) */
    uint64_t text_lines;   /**< checksum-valid text lines framed (monotonic) */
    uint64_t rx_bytes;     /**< bytes received (monotonic) */
} um98x_link_counts_t;

/** I/O for the executor. Every callback is required. */
typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    int64_t (*now_ms)(void *ctx);
    /** Write bytes; return once they have left the host (end of the write). False on a link error. */
    bool (*write)(void *ctx, const char *data, size_t len);
    /** Switch the host baud (with its input discard and framer reset). */
    bool (*set_baud)(void *ctx, uint32_t baud);
    uint32_t (*get_baud)(void *ctx);
    /** Block until woken by a line (UM98X_CHAN_WAKE) or until until_ms; may return early. Feeds the caller's watchdog. */
    void (*wait)(void *ctx, int64_t until_ms);
    void (*counts)(void *ctx, um98x_link_counts_t *out);
    /** The loader prompt detector fired since the current detection started. */
    bool (*prompt_seen)(void *ctx);
} um98x_port_t;

typedef struct {
    um98x_chan_t *ch;
    const um98x_port_t *port;
} um98x_exec_t;

typedef enum {
    UM98X_OK = 0,
    UM98X_E_TIMEOUT,       /**< no (complete) answer */
    UM98X_E_REJECTED,      /**< negative reply, final */
    UM98X_E_LINK,          /**< write or host baud switch failed */
    UM98X_E_ARG,
    UM98X_E_UNSUPPORTED,   /**< receiver model not configured by this layer: nothing sent */
    UM98X_E_FAILED,        /**< a fatal apply step failed (details in the result) */
    UM98X_E_LOST,          /**< the receiver could not be found again */
    UM98X_E_PROMPT,        /**< loader prompt seen during detection: probing stopped */
    UM98X_E_NOT_FOUND,     /**< detection found nothing */
} um98x_res_t;

typedef struct {
    uint32_t timeout_ms;   /**< 0 = the kind's default */
    uint8_t max_sends;     /**< 0 = the kind's default */
    bool restart_ends;     /**< the restart banner ends the command */
    int64_t end_ms;        /**< VERSION_RESTART: stop polling at */
    char *text;            /**< optional reply line(s) */
    size_t text_cap;
} um98x_txn_opts_t;

/**
 * Run one transaction to the end. The finished transaction (outputs) stays
 * in ex->ch->txn until the next one starts.
 */
um98x_txn_status_t um98x_exec_txn(um98x_exec_t *ex, um98x_txn_kind_t kind, const char *cmd,
                                  const um98x_txn_opts_t *opts);

/** Map a finished transaction status to a result. */
um98x_res_t um98x_res_from_txn(um98x_txn_status_t st);

/**
 * A caller's command (normalised): the version query, the base-mode query
 * and the dump complete on their data; anything else on its
 * acknowledgement (CONFIG SIGNALGROUP and RESET also on the restart banner).
 * timeout_ms 0 = default.
 */
um98x_res_t um98x_exec_command(um98x_exec_t *ex, const char *cmd, uint32_t timeout_ms, char *text, size_t cap);

/**
 * Version query. after_restart: poll every second until end_ms; else up to
 * 3 tries (2000 ms windows, 1000 ms apart). *ack_seen tells "not ready"
 * (acknowledged without the log) from "no answer"; may be NULL.
 */
um98x_res_t um98x_exec_version(um98x_exec_t *ex, bool after_restart, int64_t end_ms, gnss_versiona_t *out,
                               bool *ack_seen);

/** Base-mode query; body = the log body (may be empty if the log has none). */
um98x_res_t um98x_exec_query_mode(um98x_exec_t *ex, char *body, size_t cap);

typedef struct {
    uint32_t baud;              /**< where the receiver answered */
    bool saw_bytes;             /**< bytes arrived at some baud during the pass */
    bool by_listen;             /**< found by listening alone (nothing sent) */
    bool version_valid;         /**< the probe got a version log */
    gnss_versiona_t version;
} um98x_detect_out_t;

/** One detection pass over the candidates (see above). OK, E_NOT_FOUND, E_PROMPT or E_LINK. */
um98x_res_t um98x_exec_detect(um98x_exec_t *ex, const uint32_t *cands, size_t n, um98x_detect_out_t *out);

/**
 * The receiver sits at its loader: send its restart key "6" once at the
 * current baud and wait up to timeout_ms for the application (two text
 * lines, an RTCM frame or the banner).
 */
um98x_res_t um98x_exec_leave_loader(um98x_exec_t *ex, uint32_t timeout_ms);

/**
 * Apply a profile in one session (see above).
 * @param identity     in: this boot's version log (an empty model = unknown:
 *                     queried first); out: the identity the session used.
 *                     Required.
 * @param stored_baud  gnss/baud (head of the detection list if one is needed)
 * @return OK (saved), E_UNSUPPORTED or E_TIMEOUT (identity; nothing
 *         configuring sent), E_FAILED (see r; rolled back if anything had
 *         been accepted), E_LOST, E_LINK, E_ARG.
 */
um98x_res_t um98x_exec_apply(um98x_exec_t *ex, const um98x_base_profile_t *p, const um98x_apply_opts_t *opts,
                             gnss_versiona_t *identity, uint32_t stored_baud, um98x_apply_result_t *r);

/**
 * The receiver's RESET (see "Receiver reset" above): banner up to 15 s, then
 * the version polled every second until 20 s after it (or after the write).
 * @return OK (it answered after the restart), E_TIMEOUT (no answer; the
 *         RESET went out), E_LINK (the write failed: nothing was sent).
 */
um98x_res_t um98x_exec_reset(um98x_exec_t *ex);

/**
 * Last resort after a failed session: the profile's RTCM outputs only (no
 * base mode, no baud change, no save), normal reply handling.
 * @return OK if every non-optional message was accepted, else E_FAILED (r
 *         names the first failure). r->rtcm1230_ok as in an apply.
 */
um98x_res_t um98x_exec_restore_rtcm(um98x_exec_t *ex, const um98x_base_profile_t *p, um98x_apply_result_t *r);

#ifdef __cplusplus
}
#endif
