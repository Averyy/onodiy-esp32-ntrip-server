/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * UM98x command/config interface (Unicore N4 ASCII command set, public manual).
 *
 * Sends one command at a time over the GNSS link, matches the reply line(s),
 * and builds the station's base-station configuration from a profile:
 * signal group, COM1 baud, base mode (survey-in or fixed), RTCM3 message set
 * and rates on COM1, then one save to the receiver's non-volatile config.
 *
 * Replies arrive as text lines through um98x_ctl_on_line(), which the station
 * registers as the GNSS link's line sink. Only framer-validated lines count,
 * and each kind is checked again under its own checksum rule; raw buffers
 * are never searched for reply text. Lines that arrive while nothing waits
 * for them (or before the write of the command that waits) are counted and
 * dropped; the UART driver and the framer are never flushed for a command.
 *
 * Watchdog: the blocking calls (detect, leave_loader, query_version,
 * query_mode, command, apply_profile, restore_output) run on the caller's
 * task, the station supervisor, which is on the task watchdog. Every wait
 * blocks in slices of at most 1 s and feeds the caller's task watchdog (if
 * the task is subscribed): a full baud scan takes about 44-50 s, one command
 * up to 15 s (SAVECONFIG 23 s) with its retries, and an apply with a
 * signal-group restart up to about 35 s more. Calls are serialised: a second
 * caller gets ESP_ERR_TIMEOUT after 1 s instead of waiting for a session.
 *
 * Who may send what:
 *  - With a COMMAND lease (station supervisor only): anything.
 *  - With lease == NULL while streaming: four read-only queries, VERSIONA
 *    (identity), MODE (base mode and its parameters = status), and the
 *    one-shot own-position logs BASEPOSA and SPPNAVA (the fixed-position
 *    check, um98x_fixcheck.h; one line each). The base position itself comes
 *    from the 1005 stream. The configuration dump CONFIG needs a COMMAND lease (several KB of text
 *    between epochs). um98x_ctl_command() rejects everything else without a
 *    lease with ESP_ERR_INVALID_STATE, so this layer never reconfigures the
 *    receiver behind the supervisor's back. Raw writes through
 *    gnss_link_write(NULL, ...) bypass that check; that only the station
 *    configures the receiver is a convention for them (see gnss_link.h).
 *  - The link's current mode decides as well: lease-free queries only in
 *    STREAM (not while anyone holds a lease), leased calls only in COMMAND;
 *    in RAW (the flasher owns the bytes) and with the link off every call
 *    returns ESP_ERR_INVALID_STATE without sending anything.
 *
 * Replies (one command in flight): ack timeout 3000 ms (SAVECONFIG 5000 ms)
 * from the end of the write; on a timeout up to 3 resends (4 sends), each
 * 1000 ms after the timeout; on a negative reply ("PARSING FAILED ...", or
 * "PARSING FAILD ..." as older builds spell it) one resend, then final; a
 * late acknowledgement of an earlier send completes the command; the next
 * command goes out 100 ms after the previous reply line. Queries with data
 * (VERSIONA, MODE, CONFIG) complete on their data, not on the ack; the
 * CONFIG dump has no end marker and completes 500 ms after its last line
 * (at most 3000 ms after the ack).
 *
 * Boot is stream-first: the station opens the link at the stored baud in
 * STREAM mode and publishes at once; it queries the version without a lease
 * and takes a COMMAND lease only if the version or applied profile does not
 * match. An ESP32-only reboot therefore costs no receiver session at all.
 *
 * Identity: VERSIONA, sent as "\r\nVERSIONA\r\n"; completed by a CRC-valid
 * version log (the acknowledgement alone does not complete it), parsed with
 * gnss_versiona_parse() (shared with the flasher). One try is one write plus
 * a 2000 ms window from the end of the write; up to 3 tries, 1000 ms apart.
 * After a known receiver restart: tries every 1000 ms with 1000 ms windows
 * for up to 20 s. Only model "UM982" (exact) gets configured or flashed.
 * Every successful identity is persisted (gnss/model, gnss/fw, written only
 * when they change).
 *
 * Restart evidence: the restart banner (unless our own restart is under
 * way), the receiver's restarting answer ("The Board is resetting,...", its
 * reply to a command sent while it restarts; same terms), or a version log
 * nobody asked for (no VERSIONA written in the preceding 10 s); one restart
 * counts once. It is counted and reported through the callback set with
 * um98x_ctl_set_restart_cb(); the station posts its receiver-restarted
 * event from there. Between a restarting answer and the banner the stats
 * say the receiver is restarting (um98x_ctl_stats_t.restarting): it
 * answers, it is not silent.
 *
 * Receiver uptime: every version log's header is read for the receiver's
 * time since its last restart, which its millisecond field counts while it
 * has no time (gnss_versiona_uptime_ms; um98x_ctl_stats_t.rx_uptime_ms with
 * the log's age). After a power-on reset the station compares it with its
 * own uptime to tell an ESP32-only reset from a power cut.
 *
 * Signal groups: every apply session asks once, after its signal-group
 * step, which signals the running group configures on the main antenna
 * input (ANT1GROUPA, one-shot: a rate would start a periodic log), and
 * reports it in the result (um98x_bands.h). A receiver firmware without
 * the query refuses it: unknown, never a failure.
 *
 * Base position: survey-in restarts on every reconfigure, receiver reset or
 * power cut and lands somewhere else each time (metre-level scatter), which
 * corrupts absolute positions for rovers on casters that trust 1005. After
 * the first good survey (or a longer station-side average) the station locks
 * the position (um98x_ctl_lock_position -> settings gnss/base_pos) and
 * switches the profile to UM98X_BASE_FIXED. Survey-in stays the first-boot
 * default; the UI offers "re-survey" (um98x_resurvey_profile). Fixed
 * positions are sent to the receiver as ECEF metres with 4 decimals
 * (um98x_llh_to_ecef), which avoids the height-reference ambiguity of
 * latitude/longitude/height input; they are stored as WGS84
 * latitude/longitude/ellipsoidal height. Pure helpers: um98x_base.h.
 *
 * Status: PROVISIONAL, to be confirmed on hardware: baud-scan order, model
 * detection, reply/ack parsing, timeouts and the exact command sequence.
 * The profile structure and Onocoy defaults (um98x_profile.h) are ours.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"
#include "gnss_types.h"
#include "um98x_apply.h"
#include "um98x_base.h"
#include "um98x_fixcheck.h"
#include "um98x_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare internal state (reply channel, wake-up, call lock). Idempotent. */
esp_err_t um98x_ctl_init(void);

/**
 * Line sink for gnss_link (called from the GNSS RX task; never blocks, never logs).
 * @return true when the line is a reply to the station's own commands (every
 *         command-layer line but unsolicited restart evidence: an
 *         acknowledgement, a dump item, a log, the restarting answer, the
 *         banner of a restart of ours); false for anything the receiver sent
 *         unasked (NMEA and other output, its own restart's banner or version
 *         log, lines this layer does not read).
 */
bool um98x_ctl_on_line(const char *line, size_t len, void *ctx);

/**
 * The link's loader prompt detector fired (call from the on_prompt sink).
 * A running detection stops probing at once.
 */
void um98x_ctl_notify_prompt(void);

/** Called (from the GNSS RX task: must not block) on unsolicited restart evidence. */
typedef void (*um98x_ctl_restart_cb_t)(void *ctx);

/** Set or clear (NULL) the restart callback. */
void um98x_ctl_set_restart_cb(um98x_ctl_restart_cb_t cb, void *ctx);

/**
 * Start of a COMMAND session: a lone CR LF, so a partial line left in the
 * receiver's input is terminated before the first command. The apply,
 * restore and detection calls do this themselves; call it before other
 * commands sent under a fresh lease.
 */
esp_err_t um98x_ctl_session_begin(gnss_link_lease_t lease);

/** Result of one detection pass. */
typedef struct {
    bool found;
    uint32_t baud;            /**< where the receiver answered (link left there) */
    bool by_listen;           /**< found by listening alone: nothing was sent */
    bool saw_bytes;           /**< bytes arrived at some baud during the pass (health's byte rule) */
    bool prompt;              /**< the loader prompt was seen: probing stopped */
    bool identity;            /**< the probe returned the version log: info is valid */
    um98x_info_t info;
} um98x_detect_result_t;

/**
 * Find the receiver (COMMAND lease; the station arms the loader prompt
 * detector first). Candidates: gnss/baud, 115200, 460800, 921600, 230400;
 * full_list adds 57600, 38400, 19200, 9600. Per baud: listen 1500 ms (a
 * CRC-valid RTCM frame or two checksum-valid text lines = found, nothing
 * sent), else probe "\r\nVERSIONA\r\n" twice with 2000 ms windows (any
 * acknowledgement or the version log = found; after an accepting
 * acknowledgement the log has 1000 ms more, and then gives the identity,
 * else the identity is the next query). A loader prompt reported during the pass (um98x_ctl_notify_prompt)
 * stops it before anything more is sent; one seen before the call is the
 * caller's to handle. Found -> gnss/baud persisted if it changed.
 * @return ESP_OK (found), ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_RESPONSE (loader
 *         prompt seen: out->prompt), ESP_ERR_INVALID_STATE (no lease, link
 *         error).
 */
esp_err_t um98x_ctl_detect(gnss_link_lease_t lease, bool full_list, um98x_detect_result_t *out);

/**
 * The receiver sits at its loader prompt: send its restart key ("6") once at
 * the current baud and wait up to 15 s for the application (COMMAND lease).
 * The caller keeps the once-per-boot rule. ESP_ERR_TIMEOUT if it does not
 * come back.
 */
esp_err_t um98x_ctl_leave_loader(gnss_link_lease_t lease);

/**
 * Version query (identity). lease == NULL while streaming, or with the
 * COMMAND lease. after_restart: poll every second until 20 s after the
 * latest restart evidence (or from now). On success the identity is stored
 * (um98x_ctl_get_info) and persisted on change; the model gate is the
 * caller's (um98x_ctl_model_supported).
 * @return ESP_OK, ESP_ERR_INVALID_RESPONSE (acknowledged but no version log:
 *         not ready), ESP_ERR_TIMEOUT (no answer), ESP_ERR_INVALID_STATE.
 */
esp_err_t um98x_ctl_query_version(gnss_link_lease_t lease, bool after_restart, um98x_info_t *out);

/** Last identity this boot (from a query, a detection or an apply). ESP_ERR_NOT_FOUND before one. */
esp_err_t um98x_ctl_get_info(um98x_info_t *out);

/** True only for the model this firmware configures and flashes (exactly "UM982"). */
bool um98x_ctl_model_supported(const um98x_info_t *info);

/**
 * Base-mode query (lease == NULL while streaming, or COMMAND): the body of
 * the receiver's base-mode log, e.g. "MODE BASE 1234 TIME 600 0". Used while
 * a survey runs (um98x_survey_on_mode) and for the antenna-moved check.
 */
esp_err_t um98x_ctl_query_mode(gnss_link_lease_t lease, char *body, size_t len);

/**
 * The receiver's own position (lease == NULL while streaming, or COMMAND):
 * BASEPOSA, or SPPNAVA with sppnav, one-shot (um98x_fixcheck.h). out gets
 * the parsed solution, usable or not (um98x_own_fix_usable).
 * @return ESP_OK (a log that parsed), ESP_ERR_NOT_SUPPORTED (the receiver
 *         refused the query), ESP_ERR_INVALID_RESPONSE (a log this firmware
 *         does not read), ESP_ERR_TIMEOUT (no log), ESP_ERR_INVALID_STATE
 *         (link or lease: nothing sent, or a link error).
 */
esp_err_t um98x_ctl_query_own_fix(gnss_link_lease_t lease, bool sppnav, um98x_own_fix_t *out);

/**
 * Send one command and wait for its acknowledgement or data (rules above).
 *
 * @param lease      COMMAND lease, or NULL for VERSIONA / MODE while streaming.
 * @param cmd        command text; surrounding CR/LF and spaces are dropped
 *                   (at most 127 characters, no control characters).
 * @param reply      optional buffer for the reply line(s), '\n'-separated; may be NULL.
 * @param timeout_ms per-send ack timeout, 0 = the default (3000 ms,
 *                   SAVECONFIG 5000 ms); ignored for VERSIONA.
 * @return ESP_OK on positive ack (or complete data), ESP_FAIL on a negative
 *         reply, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG for an unusable command,
 *         ESP_ERR_INVALID_STATE for anything but the read-only queries above
 *         without a lease (and for a link error).
 */
esp_err_t um98x_ctl_command(gnss_link_lease_t lease, const char *cmd, char *reply, size_t reply_len,
                            uint32_t timeout_ms);

/**
 * Apply a full profile and save it on the receiver, in one session under
 * the caller's COMMAND lease (the station pauses the casters first).
 *
 * Order (PROVISIONAL, to be confirmed on hardware): "\r\n"; identity (if
 * none is known this boot, VERSIONA first; only "UM982" is configured and
 * nothing is sent otherwise); UNLOG COM1; a known identity is confirmed
 * with VERSIONA now, so the applied firmware is this session's; read
 * back CONFIG and MODE; CONFIG SIGNALGROUP only when the read-back differs,
 * then wait for the receiver's restart (it saves itself; banner up to 15 s,
 * version polling until 20 s after it; nothing -> detection inside the
 * session, the receiver may be back at its saved baud), UNLOG COM1 again and
 * read back again (the signal group must now match); the COM1 baud change
 * only when it differs (verified at the new baud, restored on failure);
 * MODE BASE only when the read-back differs or opts->force_mode (lock,
 * re-survey; fixed positions as ECEF, 4 decimals); the antenna descriptor
 * only when the profile has one and the read-back differs or is unknown;
 * BeiDou B1C/B2a in RTCM turned on only when the read-back shows it off
 * (both not fatal: rejections are recorded in the result); the RTCM
 * messages in profile order (a rejected 1230 is not fatal); the
 * satellite-view sentence if enabled; one SAVECONFIG. The elevation mask
 * and multipath mitigation are read back and reported, never sent.
 *
 * Success: gnss/applied_hash, gnss/applied_fw (this session's firmware),
 * gnss/rtcm1230_ok and, if it changed, gnss/baud are written in one
 * settings batch (result->persisted); then gnss/hold (the first-apply hold:
 * the first configuration the user held back has been made) is erased.
 *
 * Failure of a fatal step: no SAVECONFIG, nothing persisted; if any command
 * had been accepted, RESET (the receiver returns to its saved configuration;
 * the host goes back to the saved baud and sends nothing until the restart
 * banner or 15 s, then asks for the version; then 10 s at the previous
 * baud). The
 * station then raises its config-failed alert and, if no RTCM resumes,
 * calls um98x_ctl_restore_output(). On failure, the baud where the receiver
 * last answered (a detection inside the session, then the rollback's version
 * query; result->answer_baud) is kept as gnss/baud; after a RESET without an
 * answer the stored baud stays.
 *
 * @param opts   may be NULL (no forced base mode)
 * @param result required; what the session did and why it failed
 * @return ESP_OK; ESP_FAIL (fatal step failed, see result); ESP_ERR_NOT_SUPPORTED
 *         (model is not UM982: nothing configuring sent); ESP_ERR_TIMEOUT
 *         (identity unknown and no version log: nothing configuring sent);
 *         ESP_ERR_NOT_FOUND (receiver lost after a restart or baud change);
 *         ESP_ERR_INVALID_ARG; ESP_ERR_INVALID_STATE (no lease, link error).
 */
esp_err_t um98x_ctl_apply_profile(gnss_link_lease_t lease, const um98x_base_profile_t *profile,
                                  const um98x_apply_opts_t *opts, um98x_apply_result_t *result);

/**
 * What an apply session would send (um98x_plan_preview): a static list for
 * the user to confirm, without a read-back (the conditional steps are
 * marked). Pure; nothing is sent. @return steps written (0 for an invalid
 * profile).
 */
size_t um98x_ctl_apply_preview(const um98x_base_profile_t *profile, uint32_t link_baud, um98x_preview_step_t *out,
                               size_t cap);

/**
 * Last resort after a failed apply when the receiver answers but sends no
 * RTCM: the profile's RTCM outputs only (no base mode, no baud change, no
 * save), normal reply handling. The applied state is not written. failed
 * (optional): the failed session's result; the RTCM messages the receiver
 * refused there (failed->rtcm_refused) are left out, since they would only
 * be refused again. result may be the same object as failed.
 * @return ESP_OK if every message sent except 1230 was accepted, else ESP_FAIL.
 */
esp_err_t um98x_ctl_restore_output(gnss_link_lease_t lease, const um98x_base_profile_t *profile,
                                   const um98x_apply_result_t *failed, um98x_apply_result_t *result);

/**
 * Recovery: the receiver's RESET command under the caller's COMMAND lease.
 * Written once and never resent (a resend would reset it again); the
 * restart is ours, so its banner and the version log after it are not
 * reported as an unsolicited restart. Waits for the banner (up to 15 s) and
 * polls the version every second until 20 s after it.
 * @return ESP_OK (it answered after the restart), ESP_ERR_TIMEOUT (no answer;
 *         the RESET went out), ESP_ERR_INVALID_STATE (nothing sent: no lease,
 *         a link error, or another call holds the command layer).
 */
esp_err_t um98x_ctl_reset_receiver(gnss_link_lease_t lease);

/**
 * Satellite-in-view output on ("GPGSV COM1 <period_s>", 1..60 s) or off
 * ("UNLOG COM1 GPGSV") for the live view, under the caller's COMMAND lease
 * (a configuring command; the lease holds RTCM back while it runs: keep it
 * short). Never saved: a receiver restart or reconfiguration ends it.
 * Whether toggling it interrupts the receiver's RTCM output is PROVISIONAL,
 * to be confirmed on hardware.
 * @param reason optional: the receiver's rejection text, "timeout" or
 *               "link error"
 * @return ESP_OK, ESP_FAIL (rejected), ESP_ERR_TIMEOUT, ESP_ERR_INVALID_ARG
 *         (period), ESP_ERR_INVALID_STATE (no lease, link error, another
 *         call holds the command layer).
 */
esp_err_t um98x_ctl_sat_view(gnss_link_lease_t lease, bool on, uint16_t period_s, char *reason, size_t reason_len);

/**
 * Current base position: the antenna reference point of the latest CRC-valid
 * 1005 (from gnss_obs_get_arp), converted to WGS84 latitude/longitude/height.
 * Sends nothing to the receiver and needs no lease; the stream is the only
 * source of the position. ESP_ERR_NOT_FOUND before the first 1005.
 */
esp_err_t um98x_ctl_get_base_position(um98x_base_position_t *out);

/**
 * Lock: the latest 1005 reference point as the fixed position. Fills the
 * profile to store and apply (with opts.force_mode) and the gnss/base_pos
 * record (source survey, locked_unix = now_unix). Sends and stores nothing.
 * @return ESP_ERR_NOT_FOUND without a 1005, ESP_ERR_INVALID_RESPONSE if the
 *         point is not a plausible position.
 */
esp_err_t um98x_ctl_lock_position(const um98x_base_profile_t *cur, int64_t now_unix, um98x_base_profile_t *next,
                                  um98x_base_position_t *pos);

/**
 * After a lock: is the latest 1005 (at most max_age_ms old) within 5 mm per
 * axis of the fixed profile's position?
 * @return ESP_OK, ESP_ERR_INVALID_RESPONSE (mismatch: position-mismatch
 *         alert), ESP_ERR_NOT_FOUND (no recent 1005), ESP_ERR_INVALID_ARG (not
 *         a fixed profile).
 */
esp_err_t um98x_ctl_check_position(const um98x_base_profile_t *profile, uint32_t max_age_ms);

/** Command-layer counters (monotonic since boot) and liveness. */
typedef struct {
    uint64_t lines;              /**< checksum-valid command-layer lines (acks, dump items, banners, logs) */
    uint64_t matched;            /**< consumed by the request in flight */
    uint64_t unmatched;          /**< nothing waited for them, or another command's acknowledgement */
    uint64_t bad;                /**< command-layer prefix with a wrong checksum or form */
    uint64_t other;              /**< NMEA and other lines (not for this layer) */
    uint64_t banners;            /**< restart banners */
    uint64_t resetting;          /**< restarting answers ("The Board is resetting,...") */
    uint64_t restarts;           /**< unsolicited restart evidence */
    uint32_t last_answer_age_ms; /**< since the last checksum-valid command-layer line (UINT32_MAX = never): "the receiver answers" */
    uint32_t last_restart_age_ms;/**< since the last unsolicited restart evidence (UINT32_MAX = never) */
    bool restarting;             /**< the receiver said it restarts (within 20 s) and its banner has not come yet */
    uint32_t version_age_ms;     /**< since the last version log, whoever asked (UINT32_MAX = none this boot) */
    uint32_t rx_uptime_ms;       /**< from that log: the receiver's time since its last restart (its header's millisecond
                                      field while it has no time, gnss_versiona_uptime_ms); UINT32_MAX = it did not tell */
} um98x_ctl_stats_t;

esp_err_t um98x_ctl_get_stats(um98x_ctl_stats_t *out);

#ifdef __cplusplus
}
#endif
