/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Station supervisor core (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, host-testable). station.c runs it on the station task:
 * stn_next() names the next job, the task runs it (the blocking calls into
 * the command layer and the GNSS link) and reports the outcome with
 * stn_done(). Requests from other tasks only set flags here (under the
 * station's lock); every decision is taken in stn_next() / stn_done().
 *
 * States and what moves between them:
 *
 *   IDLE (first 3 s of a boot: listen at the stored baud, hub fed)
 *     >= 1 RTCM frame or >= 2 checksum-valid text lines -> STREAMING
 *     nothing                                           -> DETECTING
 *   STREAMING: identity query without a lease (a failure leaves the
 *     identity unconfirmed and retries every 10 min; a lease is taken for it
 *     only when no model was ever stored), then the applied check: live
 *     firmware == gnss/applied_fw, gnss/applied_hash == the profile's hash
 *     (both checked by the caller, STN result static_ok) and the observed
 *     set over one window (station_obs.h). All true: nothing is sent. Any
 *     false and gnss/autocfg = 1 and no CONFIG_FAILED: apply. An unsupported
 *     model: stream only (alert). Later windows watch for drift: two failed
 *     windows in a row while RTCM flows re-apply (same conditions), unless
 *     the previous apply's own check already failed (re-applying would not
 *     help; that waits for the user, the ladder or a receiver restart).
 *   CONFIGURING (or RECOVERING for the ladder's own reconfigure step): one
 *     apply session; casters paused. Success -> STREAMING and the post-apply
 *     check. Failure -> CONFIG_FAILED, retried after 10 min, 30 min, 2 h,
 *     then every 6 h; then the last resort: wait 10 s for RTCM; none and the
 *     receiver answered -> restore the RTCM outputs only; none and it did
 *     not -> DETECTING. Receiver lost -> DETECTING.
 *   DETECTING: one detection pass (short list). Found -> STREAMING. The
 *     loader prompt -> its restart key once per boot (inside the pass); the
 *     application back -> detect again, else RECEIVER_IN_LOADER and
 *     listen-only. Not found -> NO_RECEIVER.
 *   NO_RECEIVER: passes every 30 s for the first 5 min of the episode, then
 *     every 5 min; the full baud list after two failed short passes in a row
 *     and always after 5 min. Listen-only (nothing transmitted) while the
 *     receiver sits at its loader, until it streams again or the user asks
 *     for a restart. Data arriving between passes at the stored baud ->
 *     STREAMING at once.
 *   RECOVERING: a step the health ladder asked for (resync, link restart,
 *     rescan, reconfigure, receiver reset); weaker requests are ignored while
 *     a stronger one is queued or running. A refused reconfigure (autocfg
 *     off, or CONFIG_FAILED raised) sends nothing. A reconfigure carries out
 *     an apply that was queued (the user's lock or re-survey) in the same
 *     session, so the queued request is never lost.
 *   A version query that needs a lease (no model ever stored) pauses the
 *   casters while it runs: the lease holds RTCM back.
 *   FLASH_RECOVERY: a receiver-flash journal is open (or its state is
 *     unknown): nothing is decided here until it is cleared.
 *
 * Survey: an apply that sent a survey-in base mode, a receiver restart while
 * the profile surveys (also one an apply session caused itself: the
 * signal-group reset or a rollback), or a base-mode read-back at boot that
 * shows a running survey opens the survey window (um98x_base.h); the base
 * mode is read every
 * 60 s while it is open. Once the survey is complete (the read-back, or the
 * 1005 fallback) the position is locked automatically (gnss/autocfg = 1, no
 * CONFIG_FAILED) and checked against the next 1005.
 *
 * Restart evidence (banner, unsolicited version log): identity again in
 * restart mode, the applied check 30 s later, a new survey window for a
 * survey profile; for a fixed profile with no RTCM by then the base mode is
 * read: a fixed base that sends nothing raises ANTENNA_MOVED (no apply).
 *
 * Timings PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "station_obs.h"
#include "um98x_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STN_LISTEN_MS          3000      /**< stream-first listen at boot */
#define STN_IDENTITY_RETRY_MS  600000    /**< unconfirmed identity: query again */
#define STN_RESTART_CHECK_MS   30000     /**< applied check after restart evidence */
#define STN_LAST_RESORT_MS     10000     /**< after a failed apply: wait this long for RTCM */
#define STN_DETECT_FAST_MS     30000     /**< pass interval in the first part of an episode */
#define STN_DETECT_SLOW_MS     300000    /**< pass interval after it */
#define STN_DETECT_FAST_SPAN_MS 300000   /**< first part of an episode (the full list always runs after it) */
#define STN_MODE_POLL_MS       (UM98X_SURVEY_MODE_POLL_S * 1000)  /**< base-mode read-back while surveying */
#define STN_POS_CHECK_MS       30000     /**< after a lock: compare the next 1005 */
#define STN_POS_CHECK_TRIES    10
#define STN_LOCK_RETRY_MS      60000     /**< automatic lock without a 1005 yet: try again */
#define STN_DRIFT_WINDOWS      2         /**< failed windows in a row before a drift re-apply */

/** Same values as station_state_t. */
typedef enum {
    STN_IDLE = 0,
    STN_DETECTING,
    STN_CONFIGURING,
    STN_STREAMING,
    STN_FLASHING,
    STN_FLASH_RECOVERY,
    STN_RECOVERING,
    STN_NO_RECEIVER,
} stn_state_t;

/** Same bits as station_alert_t. */
#define STN_ALERT_UNSUPPORTED_MODEL  (1u << 0)
#define STN_ALERT_CONFIG_FAILED      (1u << 1)
#define STN_ALERT_POSITION_MISMATCH  (1u << 2)
#define STN_ALERT_ANTENNA_MOVED      (1u << 3)
#define STN_ALERT_RECEIVER_ATTENTION (1u << 4)
#define STN_ALERT_RECEIVER_IN_LOADER (1u << 5)

/** Same values as station_recovery_t (mildest first). */
typedef enum {
    STN_REC_RESYNC = 0,
    STN_REC_LINK_RESTART,
    STN_REC_RESCAN,
    STN_REC_RECONFIGURE,
    STN_REC_RECEIVER_RESET,
    STN_REC_COUNT,
} stn_rec_t;

typedef enum {
    STN_JOB_NONE = 0,
    STN_JOB_IDENTIFY,        /**< version query: no lease (STREAM), or a COMMAND lease when `leased` */
    STN_JOB_MODE,            /**< base-mode read-back without a lease */
    STN_JOB_DETECT,          /**< COMMAND lease: one detection pass */
    STN_JOB_LEAVE_LOADER,    /**< COMMAND lease: the user's receiver restart (loader key at the prompt's baud) */
    STN_JOB_APPLY,           /**< COMMAND lease: one apply session (after the profile change, if any) */
    STN_JOB_RESTORE,         /**< COMMAND lease: the profile's RTCM outputs only (last resort) */
    STN_JOB_RESYNC,          /**< framer reset and input drop, no lease */
    STN_JOB_LINK_RESTART,    /**< UART reinstall at the stored baud, no lease */
    STN_JOB_RECEIVER_RESET,  /**< COMMAND lease: the receiver's RESET command */
    STN_JOB_CHECK_POSITION,  /**< after a lock: the latest 1005 against the locked position (sends nothing) */
} stn_job_kind_t;

/** Profile change an apply carries out first. */
typedef enum {
    STN_CHANGE_NONE = 0,
    STN_CHANGE_LOCK,         /**< the latest 1005 becomes the fixed position */
    STN_CHANGE_RESURVEY,     /**< back to survey-in, locked position forgotten */
} stn_change_t;

/** Why a base-mode read-back runs. */
typedef enum {
    STN_MODE_POLL = 0,       /**< survey progress */
    STN_MODE_BOOT,           /**< once per boot with a survey profile: is a survey running, or finished? */
    STN_MODE_ANTENNA,        /**< fixed profile, no RTCM after a restart: does the receiver hold a fixed base? */
} stn_mode_why_t;

/** Why an apply runs (logs and the RECOVERING/CONFIGURING choice). */
typedef enum {
    STN_WHY_CHECK = 0,       /**< the applied check failed */
    STN_WHY_DRIFT,           /**< the observed set drifted */
    STN_WHY_RETRY,           /**< CONFIG_FAILED backoff */
    STN_WHY_USER,            /**< the user's "apply now" */
    STN_WHY_LADDER,          /**< the health ladder's reconfigure step */
    STN_WHY_LOCK,            /**< position lock (user or automatic) */
    STN_WHY_RESURVEY,        /**< the user's re-survey */
} stn_why_t;

typedef struct {
    uint8_t kind;            /**< stn_job_kind_t */
    bool leased;             /**< IDENTIFY: with a COMMAND lease (no model ever stored) */
    bool after_restart;      /**< IDENTIFY: restart mode (poll every second for 20 s) */
    bool full;               /**< DETECT: full baud list */
    bool listen_only;        /**< DETECT: nothing is transmitted (receiver at its loader) */
    bool break_first;        /**< DETECT: UART break before the pass (the ladder's rescan) */
    bool loader_key;         /**< DETECT: the loader's restart key may be sent once if the prompt shows */
    bool force_mode;         /**< APPLY: send the base mode even if the read-back matches */
    uint8_t change;          /**< APPLY: stn_change_t */
    uint8_t why;             /**< APPLY: stn_why_t */
    uint8_t mode_why;        /**< MODE: stn_mode_why_t */
} stn_job_t;

/** Job outcome codes (the caller maps its errors). */
typedef enum {
    STN_OK = 0,
    STN_E_FAIL,              /**< a fatal step failed or the receiver said no */
    STN_E_TIMEOUT,           /**< no answer */
    STN_E_NOT_READY,         /**< acknowledged without data (version query) */
    STN_E_UNSUPPORTED,       /**< receiver model is not the one we configure */
    STN_E_NOT_FOUND,         /**< receiver not found / lost; LOCK: no 1005 yet; position: no recent 1005 */
    STN_E_MISMATCH,          /**< position check: more than 5 mm off */
    STN_E_STATE,             /**< link or lease unavailable: nothing was sent */
} stn_err_t;

typedef struct {
    uint8_t err;             /**< stn_err_t */
    /* IDENTIFY, and DETECT with identity */
    bool identity;           /**< DETECT: the probe returned the version log */
    bool supported;          /**< the model is the one we configure */
    bool static_ok;          /**< live firmware == gnss/applied_fw, applied hash == the profile's hash, 1230 result stored */
    /* MODE */
    bool mode_survey;        /**< read-back shows a running survey-in */
    bool mode_fixed;         /**< read-back shows a fixed base (or a finished survey) */
    /* DETECT */
    bool saw_bytes;          /**< bytes arrived at some baud during the pass */
    bool prompt;             /**< the loader prompt was seen: probing stopped */
    bool key_sent;           /**< the loader's restart key was sent */
    bool app_back;           /**< the application answered after it */
    /* APPLY */
    bool nothing_sent;       /**< the session sent nothing configuring (unsupported model, no identity, no 1005 for a lock) */
    bool mode_sent;          /**< the base mode was sent and accepted (survey restarts) */
    int64_t mode_ack_ms;     /**< when */
    bool restarted;          /**< the receiver restarted in the session (signal-group reset, an answered rollback RESET) */
    bool receiver_lost;      /**< the receiver was not found again during the session */
    bool answered;           /**< the receiver answered during or after the session */
} stn_result_t;

/** Inputs sampled by the caller for each stn_next() / stn_done(). */
typedef struct {
    int64_t now_ms;
    uint64_t rtcm_frames;    /**< framer: CRC-valid RTCM frames (published or held) */
    uint64_t text_lines;     /**< framer: checksum-valid text lines */
    bool autocfg;            /**< gnss/autocfg */
    bool model_known;        /**< a model was stored (gnss/model) at some point */
    bool profile_survey;     /**< the profile surveys (else fixed) */
    uint32_t survey_s;       /**< the profile's survey length */
    bool rtcm1230_ok;        /**< gnss/rtcm1230_ok */
    const stn_obs_t *obs;    /**< snapshot of the observed-set tracker (may be NULL) */
} stn_in_t;

typedef struct {
    stn_state_t state;
    uint32_t alerts;
    bool started;

    /* listen (boot) and "data appeared" baselines */
    int64_t listen_until_ms;
    uint64_t base_rtcm;
    uint64_t base_lines;

    /* identity */
    bool identity_ok;          /**< confirmed this boot */
    bool supported;            /**< valid with identity_ok */
    bool static_ok;
    bool leased_tried;         /**< the one leased identity query was made */
    bool identify_leased;      /**< the next query takes a COMMAND lease */
    int64_t identify_ms;       /**< next query (STN_NEVER = none) */
    bool identify_restart;

    /* applied check and drift */
    bool obs_rearm;            /**< output: re-arm the tracker with the profile (caller clears) */
    bool check_pending;        /**< the next verdict is the applied check */
    bool verify_pending;       /**< the next verdict checks the apply that just ran */
    bool obs_on;               /**< windows are evaluated */
    int64_t check_ms;          /**< re-arm and check at this time (after restart evidence) */
    bool profile_applied;
    uint8_t drift_windows;
    bool drift_blocked;
    uint16_t drift_msg;        /**< first message that failed the latest window (0 = none) */

    /* apply */
    bool apply_wanted;
    uint8_t apply_why;
    uint8_t apply_change;
    bool apply_force;
    stn_state_t session_state; /**< CONFIGURING or RECOVERING while a session or its last resort runs */
    uint32_t apply_failures;   /**< since the last successful apply (backoff step) */
    int64_t apply_retry_ms;    /**< CONFIG_FAILED retry (STN_NEVER = none) */
    bool last_resort;
    int64_t last_resort_ms;    /**< end of the 10 s wait for RTCM */
    bool last_resort_answered;
    bool paused;               /**< output: casters paused */

    /* detection */
    int64_t episode_ms;
    int64_t detect_ms;         /**< next pass (STN_NEVER = none) */
    uint8_t short_fails;
    uint32_t detect_failures;
    bool detect_saw_bytes;
    bool loader_key_sent;      /**< the loader's restart key went out this boot */
    bool listen_only;
    bool user_restart;         /**< the user asked for a receiver restart (loader case) */

    /* recovery requests (action + 1, 0 = none) */
    uint8_t rec_pending;
    uint8_t rec_running;

    /* restart evidence */
    bool restart_seen;
    bool antenna_check;        /**< fixed profile restarted: look at RTCM at the check time */

    /* survey and position */
    um98x_survey_t survey;
    bool mode_probe;           /**< read the base mode once (survey profile, boot) */
    bool mode_probe_done;
    bool antenna_probe;        /**< read the base mode for the antenna check */
    int64_t mode_poll_ms;
    bool lock_done;            /**< the automatic lock of this survey was made (or given up) */
    int64_t lock_retry_ms;
    int64_t pos_check_ms;
    uint8_t pos_check_tries;

    /* the job handed out by stn_next() */
    stn_job_t job;
    bool job_busy;
} stn_t;

/** Fresh core: IDLE, not started; journal_open -> FLASH_RECOVERY. */
void stn_init(stn_t *s, bool journal_open);

/** Start of the station task: the boot listen window begins (unless FLASH_RECOVERY). */
void stn_start(stn_t *s, const stn_in_t *in);

/** The flash journal was cleared by the user: leave FLASH_RECOVERY and boot stream-first. */
void stn_journal_cleared(stn_t *s, const stn_in_t *in);

/**
 * Next job, or STN_JOB_NONE (call again after at most a second, or when
 * woken). May change state, alerts and the outputs (paused, obs_rearm).
 * Between stn_next() returning a job and stn_done() no other job is given.
 */
stn_job_t stn_next(stn_t *s, const stn_in_t *in);

/** Outcome of the job stn_next() returned. */
void stn_done(stn_t *s, const stn_result_t *r, const stn_in_t *in);

/* ---- requests (other tasks, under the caller's lock) ------------------- */

/** The user's "apply now" (the profile was edited). Runs once the receiver streams. */
void stn_request_apply(stn_t *s);

/** Lock the current position (user). */
void stn_request_lock(stn_t *s);

/** Survey again (user). */
void stn_request_resurvey(stn_t *s);

/** True if a RECONFIGURE request would be refused now (autocfg off or CONFIG_FAILED raised). */
bool stn_reconfigure_refused(const stn_t *s, bool autocfg);

/**
 * A ladder step. @return STN_OK (queued, or ignored because an equal or
 * stronger one is queued or running), STN_E_FAIL (refused: RECONFIGURE while
 * stn_reconfigure_refused()), STN_E_STATE (not in a state the ladder acts
 * in: only STREAMING, CONFIGURING and RECOVERING).
 */
int stn_request_recovery(stn_t *s, uint8_t action, bool autocfg);

/**
 * The user's receiver restart while RECEIVER_IN_LOADER is raised.
 * @return STN_OK, STN_E_STATE without that alert.
 */
int stn_request_restart_receiver(stn_t *s);

/** Unsolicited restart evidence (banner or version log). */
void stn_restart_seen(stn_t *s);

/** A new CRC-valid 1005/1006 reference point (survey fallback). */
void stn_on_arp(stn_t *s, const int64_t ecef_01mm[3]);

/* ---- status --------------------------------------------------------------- */

bool stn_surveying(const stn_t *s, int64_t now_ms);
uint32_t stn_survey_left_s(const stn_t *s, int64_t now_ms);
/** Seconds to the next CONFIG_FAILED retry (0 = none scheduled). */
uint32_t stn_config_retry_in_s(const stn_t *s, int64_t now_ms);

/** CONFIG_FAILED backoff: delay after the n-th failure in a row (n >= 1), ms. */
int64_t stn_apply_backoff_ms(uint32_t failures);

/** Detection candidates: stored baud first, then the short list; full adds the rest; no duplicates. */
unsigned stn_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, unsigned cap);

#ifdef __cplusplus
}
#endif
