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
 *   FLASHING: the user's receiver update (STN_JOB_FLASH, casters paused,
 *     the live view closed). The session's outcome decides: the application
 *     answers -> re-apply the profile (STN_WHY_FLASH, whatever gnss/autocfg
 *     says), then STN_NOTE_FLASH_CLOSE after the re-apply's observed-set
 *     window (or RTCM within STN_FLASH_RTCM_MS after the abort path), at the
 *     latest STN_FLASH_CLOSE_MAX_MS after the session; a journal waiting
 *     for the user -> FLASH_RECOVERY; nothing changed -> STREAMING.
 *   FLASH_RECOVERY: a receiver-flash journal is open (or its state is
 *     unknown): nothing is decided here until it is cleared, or until the
 *     user's retry or restart (or the boot-time recovery, where it is built
 *     in) runs as a STN_JOB_FLASH.
 *   Bench operations (STN_JOB_BENCH, bench builds) run from STREAMING or
 *     NO_RECEIVER with the casters paused; afterwards the stream-first path
 *     runs as at boot (the receiver may have been reset).
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
 * Receiver maintenance (hands off; the user works on the receiver through
 * its other port): entered by the user with a duration (15 min, 30 min,
 * 1 h, 2 h or 4 h; extendable while on, at most 4 h left), refused while a
 * receiver-flash journal is open (FLASHING, FLASH_RECOVERY), while a
 * session or a job runs (a lease is held, CONFIGURING, RECOVERING: offer
 * again when it ends). While on, the state is IDLE (listening only, as in a
 * boot's first seconds: the health ladder stays suspended, no drift is
 * judged) and no job is handed out at all: no detection,
 * no queries, no apply, no loader key, no RESET, no satellite-view toggle;
 * the caller's link refuses every write and lease (hands off). Reception
 * goes on. Restart evidence is noted and triggers nothing. Queued ladder
 * steps and automatic re-applies are dropped at entry (the user's own
 * requests stay queued). Exit (the user, or the time is up): the
 * stream-first path as at boot (IDLE, listen 3 s, identity, applied
 * check). The time left survives an ESP32 reset through the caller's RTC
 * record (stn_maint_restore; codec stn_maint_rtc_*), never a power cut.
 *
 * Live view (a time-boxed window the user opens; satellites first): while
 * it is open the station wants the receiver's satellite-in-view output on,
 * unsaved, at the caller's period; it is turned on and off with short
 * COMMAND-lease jobs (STN_JOB_SKY; the casters pause for them), only while
 * STREAMING with no other job due, never in maintenance, never with
 * gnss/autocfg = 0 (the user configures the receiver), only for the model
 * we configure, and never when the profile sends it permanently. A
 * receiver reconfiguration, RESET or restart ends the output (it was never
 * saved): the window stays open without it until the user starts the
 * view again. The window closes when its time is up, when no admin client
 * asked for it for the idle time (stn_live_touch), at the user's stop, or
 * when a receiver flash begins; output still on is then turned off at the
 * next allowed moment (a failed attempt is retried after 60 s, at most 3
 * attempts). Whether toggling the output interrupts RTCM is PROVISIONAL,
 * to be confirmed on hardware.
 *
 * Notes (stn_note_t) tell the caller what to journal: maintenance entry
 * and exit, live view opened, extended, closed, output turned on or off.
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

#define STN_MAINT_DEFAULT_S    (30u * 60u)   /**< receiver maintenance: default duration */
#define STN_MAINT_MAX_S        (4u * 3600u)  /**< longest choice, and the most time left after an extension */
#define STN_LIVE_DEFAULT_MS    (10 * 60000)  /**< live view window (the caller passes its own) */
#define STN_LIVE_MAX_MS        (60 * 60000)  /**< longest window, extensions included */
#define STN_LIVE_IDLE_MS       120000        /**< closes when no admin client asked for it this long */
#define STN_SKY_RETRY_MS       60000         /**< a failed "output off" is tried again after this */
#define STN_SKY_OFF_TRIES      3             /**< then it is left on until the receiver restarts */
#define STN_NOTES              8             /**< notes kept for the caller between two steps */

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
#define STN_ALERT_NO_RECEIVER        (1u << 6)   /**< derived (stn_alerts): never stored in `alerts` */

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
    STN_JOB_SKY,             /**< COMMAND lease: satellite-in-view output on or off (sky_on), never saved */
    STN_JOB_FLASH,           /**< RAW lease: a receiver update session, a user action on a waiting one, or boot-time recovery (flash_kind) */
    STN_JOB_BENCH,           /**< RAW lease: one bench console operation (bench builds) */
    STN_JOB_PKG_CHECK,       /**< no lease: check the staged package against every rule, send nothing */
} stn_job_kind_t;

/** What a STN_JOB_FLASH does. */
typedef enum {
    STN_FLASH_NEW = 1,       /**< the user's update (STREAMING) */
    STN_FLASH_RETRY,         /**< the user's "retry flash (same package)" (FLASH_RECOVERY) */
    STN_FLASH_RESTART,       /**< the user's "restart receiver" (FLASH_RECOVERY) */
    STN_FLASH_RECOVER,       /**< boot-time recovery of an open journal (FLASH_RECOVERY, actions built in) */
} stn_flash_kind_t;

/** How a STN_JOB_FLASH ended (the caller's mapping of the session). */
typedef enum {
    STN_FLASH_END_NOTHING = 0,  /**< nothing changed on the receiver and no journal is open: stream on */
    STN_FLASH_END_REAPPLY,      /**< the application answers after a transfer: re-apply, check the observed set, close */
    STN_FLASH_END_REAPPLY_RTCM, /**< ended before the point of no return: re-apply, RTCM within STN_FLASH_RTCM_MS, close */
    STN_FLASH_END_NEEDS_USER,   /**< the journal waits for the user: FLASH_RECOVERY, nothing sent */
} stn_flash_end_t;

#define STN_FLASH_RTCM_MS      120000    /**< after the abort path's re-apply: RTCM within this */
#define STN_FLASH_CLOSE_MAX_MS 600000    /**< the journal closes at the latest this long after the session (re-apply and check included) */

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
    STN_WHY_FLASH,           /**< after a receiver update session (whatever gnss/autocfg says) */
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
    bool sky_on;             /**< SKY: turn the output on (else off) */
    uint8_t flash_kind;      /**< FLASH: stn_flash_kind_t */
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
    /* SKY */
    uint32_t lease_ms;       /**< how long the lease held RTCM back */
    /* FLASH */
    uint8_t flash_end;       /**< stn_flash_end_t */
    bool loader_key_sent;    /**< the identification sent the loader's restart key (the boot's one) */
    bool in_loader;          /**< ... and the application did not come back */
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
    bool profile_sky;        /**< the profile sends satellite-in-view output permanently */
    const stn_obs_t *obs;    /**< snapshot of the observed-set tracker (may be NULL) */
} stn_in_t;

/** Why maintenance (or its end) was refused. */
typedef enum {
    STN_MAINT_OK = 0,
    STN_MAINT_FLASH,         /**< a receiver-flash journal is open */
    STN_MAINT_SESSION,       /**< a session or job runs: offer again when it ends */
    STN_MAINT_DURATION,      /**< not one of the choices */
    STN_MAINT_NOT_ON,        /**< "end now" without the state */
    /* 5 is the caller's own "not running" */
    STN_MAINT_SKY_OUTPUT = 6, /**< the live view's satellite-in-view output may be on: turned off first, offer again */
} stn_maint_why_t;

/** Why a live view window closed. */
typedef enum {
    STN_LIVE_END_NONE = 0,
    STN_LIVE_END_USER,       /**< the user stopped it */
    STN_LIVE_END_EXPIRED,    /**< its time was up */
    STN_LIVE_END_IDLE,       /**< no admin client asked for it for the idle time */
    STN_LIVE_END_FLASH,      /**< a receiver flash began */
} stn_live_end_t;

/** The satellite-in-view output as the live view sees it. */
typedef enum {
    STN_SKY_OFF = 0,         /**< not wanted (no window) */
    STN_SKY_PENDING,         /**< wanted, waits for a free moment (why) */
    STN_SKY_ON,              /**< on for this window (unsaved) */
    STN_SKY_ON_PROFILE,      /**< the profile sends it permanently */
    STN_SKY_BLOCKED,         /**< not available in this window (why) */
    STN_SKY_STOPPING,        /**< the window closed; turned off at the next free moment */
} stn_sky_t;

/** Why the output is not on while the window wants it. */
typedef enum {
    STN_SKY_WHY_NONE = 0,
    STN_SKY_WHY_BUSY,            /**< a receiver job or session runs */
    STN_SKY_WHY_NOT_STREAMING,   /**< no receiver streaming (detecting, no receiver, boot) */
    STN_SKY_WHY_MAINTENANCE,     /**< receiver maintenance: nothing is sent (pending), or turned off for a maintenance request (dropped) */
    STN_SKY_WHY_AUTOCFG_OFF,     /**< gnss/autocfg = 0: the user configures the receiver */
    STN_SKY_WHY_UNSUPPORTED,     /**< not a receiver model we configure */
    STN_SKY_WHY_RECONFIGURED,    /**< receiver reconfigured, reset or restarted: start again to turn it on */
    STN_SKY_WHY_REJECTED,        /**< the receiver rejected the command */
    STN_SKY_WHY_NO_ANSWER,       /**< no answer (it may be on: turned off when the window closes; extending sends it again) */
    STN_SKY_WHY_FLASH,           /**< receiver update session */
} stn_sky_why_t;

/** Live view status. */
typedef struct {
    bool open;
    uint32_t left_s;         /**< until the window closes */
    uint32_t idle_left_s;    /**< until it closes for lack of an admin client */
    uint32_t open_s;         /**< since it opened */
    uint8_t sky;             /**< stn_sky_t */
    uint8_t why;             /**< stn_sky_why_t */
    uint8_t last_end;        /**< stn_live_end_t of the last window */
} stn_live_status_t;

/** What the caller journals (and logs). */
typedef enum {
    STN_NOTE_MAINT_ON = 1,   /**< a = chosen duration (s) */
    STN_NOTE_MAINT_OFF,      /**< a = time spent (s), b = 1 the time was up, 0 the user ended it */
    STN_NOTE_LIVE_OPENED,    /**< a = window length (s) */
    STN_NOTE_LIVE_EXTENDED,  /**< a = time left (s) */
    STN_NOTE_LIVE_CLOSED,    /**< a = time open (s), b = stn_live_end_t */
    STN_NOTE_SKY_ON,         /**< a = lease length (ms), b = stn_err_t of the job */
    STN_NOTE_SKY_OFF,        /**< as SKY_ON */
    STN_NOTE_SKY_LEFT_ON,    /**< turning it off failed STN_SKY_OFF_TRIES times: left on until the receiver restarts */
    STN_NOTE_FLASH_CLOSE,    /**< close the receiver-flash journal now: a = 1 the re-apply's check passed, 0 it did not (RECEIVER_ATTENTION raised) */
} stn_note_kind_t;

typedef struct {
    uint8_t kind;            /**< stn_note_kind_t */
    int32_t a;
    int32_t b;
} stn_note_t;

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
    uint32_t detect_passes;    /**< detection passes completed since boot, found or not (health re-checks a blocked reboot on a newer pass) */
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

    /* receiver maintenance (hands off) */
    bool maint;
    int64_t maint_start_ms;    /**< when it began (moved back by the time spent before a reset) */
    int64_t maint_until_ms;
    uint32_t maint_chosen_s;   /**< chosen duration (extensions added; for the status) */
    bool maint_restart_seen;   /**< restart evidence while it was on (noted, nothing done) */

    /* live view */
    uint32_t live_idle_ms;     /**< closes after this long without stn_live_touch (STN_LIVE_IDLE_MS) */
    uint32_t live_max_ms;      /**< longest window (STN_LIVE_MAX_MS) */
    bool live_open;
    int64_t live_opened_ms;
    int64_t live_until_ms;
    int64_t live_touch_ms;
    uint8_t live_last_end;     /**< stn_live_end_t */
    bool sky_want;             /**< the window wants the satellite-in-view output */
    bool sky_on;               /**< it may be on because of us (unsaved) */
    bool sky_unsure;           /**< ... only maybe: "on" got no answer (off when the window closes, or sent again on an extension) */
    uint8_t sky_why;           /**< why the window dropped its wish (stn_sky_why_t) */
    uint8_t sky_off_tries;
    int64_t sky_retry_ms;

    /* notes for the caller (journal) */
    uint8_t note_n;
    stn_note_t notes[STN_NOTES];

    /* receiver update sessions, bench operations, package checks */
    uint8_t flash_want;        /**< stn_flash_kind_t queued (0 = none) */
    bool bench_want;
    bool check_want;
    uint8_t flash_close;       /**< 0 none, 1 after the re-apply's window, 2 after RTCM within STN_FLASH_RTCM_MS */
    int64_t flash_close_ms;    /**< the journal closes by then whatever happens */
    int64_t flash_rtcm_ms;     /**< close kind 2: RTCM is required by then */

    /* the job handed out by stn_next() */
    stn_job_t job;
    bool job_busy;
} stn_t;

/** Fresh core: IDLE, not started; journal_open -> FLASH_RECOVERY. */
void stn_init(stn_t *s, bool journal_open);

/** Start of the station task: the boot listen window begins (unless FLASH_RECOVERY). */
void stn_start(stn_t *s, const stn_in_t *in);

/**
 * The flash journal was cleared by the user: leave FLASH_RECOVERY and boot
 * stream-first. A receiver update job still queued is dropped (there is no
 * journal left for it to work on).
 */
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
 * in: only STREAMING, CONFIGURING and RECOVERING; never in maintenance).
 */
int stn_request_recovery(stn_t *s, uint8_t action, bool autocfg);

/**
 * The user's receiver restart while RECEIVER_IN_LOADER is raised.
 * @return STN_OK, STN_E_STATE without that alert.
 */
int stn_request_restart_receiver(stn_t *s);

/**
 * A receiver update job. NEW needs STREAMING (and no session, job or
 * maintenance); RETRY, RESTART and RECOVER need FLASH_RECOVERY. The session
 * itself is the caller's (STN_JOB_FLASH); afterwards the core re-applies the
 * profile (STN_WHY_FLASH, whatever gnss/autocfg says), checks it and notes
 * STN_NOTE_FLASH_CLOSE. @return STN_OK, STN_E_STATE.
 */
int stn_request_flash(stn_t *s, uint8_t kind);

/** A receiver update job is queued or running (the journal is being worked on: it may not be cleared). */
bool stn_flash_pending(const stn_t *s);

/** A bench operation (STN_JOB_BENCH): STREAMING or NO_RECEIVER, nothing else running, not in maintenance. */
int stn_request_bench(stn_t *s);

/** Check the staged package only (STN_JOB_PKG_CHECK): not while a flash journal is open. */
int stn_request_pkg_check(stn_t *s);

/**
 * The user's "check again" in NO_RECEIVER: the next detection pass is due
 * now (the episode's schedule position, short_fails and episode start, is
 * kept). A pass that runs already counts. @return STN_OK, STN_E_STATE in
 * any other state or in maintenance.
 */
int stn_request_detect(stn_t *s, int64_t now_ms);

/** Unsolicited restart evidence (banner or version log). */
void stn_restart_seen(stn_t *s);

/** A new CRC-valid 1005/1006 reference point (survey fallback). */
void stn_on_arp(stn_t *s, const int64_t ecef_01mm[3]);

/* ---- receiver maintenance ------------------------------------------------- */

/** One of the durations the user may choose: 15 min, 30 min, 1 h, 2 h, 4 h (seconds). */
bool stn_maint_duration_ok(uint32_t dur_s);

/**
 * The user's maintenance request: enter for dur_s, or, while on, extend by
 * dur_s (at most STN_MAINT_MAX_S left). lease_held: a lease is held on the
 * link now (the caller's live view). While the live view's satellite-in-view
 * output may be on (and the receiver is reachable) it is refused with
 * STN_MAINT_SKY_OUTPUT: the output goes off at the next free moment and the
 * open window drops its wish for it (STN_SKY_WHY_MAINTENANCE).
 * @return stn_maint_why_t (STN_MAINT_OK = on now).
 */
int stn_maint_enter(stn_t *s, uint32_t dur_s, bool lease_held, int64_t now_ms);

/** The user's "end now". @return STN_MAINT_OK or STN_MAINT_NOT_ON. */
int stn_maint_end(stn_t *s, const stn_in_t *in);

/** Back in maintenance after an ESP32 reset (the caller's RTC record): left_s remain, spent_s were spent before the reset. */
void stn_maint_restore(stn_t *s, uint32_t left_s, uint32_t spent_s, int64_t now_ms);

uint32_t stn_maint_left_s(const stn_t *s, int64_t now_ms);

/** The caller's RTC record of the maintenance state (magic, image stamp, time left, CRC). */
typedef struct {
    uint32_t magic;
    uint32_t stamp;          /**< the image generation stamp that wrote it */
    uint32_t left_s;
    uint32_t spent_s;        /**< time spent in maintenance before the record was written */
    uint32_t crc;
} stn_maint_rtc_t;

/** Write the record (left_s 0 clears it). */
void stn_maint_rtc_write(stn_maint_rtc_t *r, uint32_t stamp, uint32_t left_s, uint32_t spent_s);

/** Read the record: false for a cleared, damaged or foreign one, or one with nothing left. */
bool stn_maint_rtc_read(const stn_maint_rtc_t *r, uint32_t *stamp, uint32_t *left_s, uint32_t *spent_s);

/**
 * The caller's RTC record of "the satellite-in-view output may be on because
 * of us" (magic, image stamp, CRC): written before the "on" command and kept
 * while sky_on holds, so an ESP32-only reset (the receiver keeps running and
 * keeps the unsaved output) still turns it off.
 */
typedef struct {
    uint32_t magic;
    uint32_t stamp;          /**< the image generation stamp that wrote it */
    uint32_t crc;
} stn_sky_rtc_t;

/** Write the record (on false clears it). */
void stn_sky_rtc_write(stn_sky_rtc_t *r, uint32_t stamp, bool on);

/** Read the record: false for a cleared, damaged or foreign one. */
bool stn_sky_rtc_read(const stn_sky_rtc_t *r, uint32_t *stamp);

/** After an ESP32-only reset with the record set: the output may still be on; turned off once the station streams. */
void stn_sky_restore(stn_t *s);

/* ---- live view --------------------------------------------------------------- */

/**
 * Open the window for dur_ms, or extend an open one by dur_ms (at most
 * live_max_ms from now), and want the satellite-in-view output again.
 * Counts as a touch.
 * @return STN_OK, or STN_E_STATE with *why = STN_SKY_WHY_FLASH while a
 *         receiver-flash journal is open (nothing changes).
 */
int stn_live_start(stn_t *s, uint32_t dur_ms, int64_t now_ms, uint8_t *why);

/** An admin client asked for the live view (keeps the window from closing as idle). */
void stn_live_touch(stn_t *s, int64_t now_ms);

/** The user's stop. */
void stn_live_stop(stn_t *s, int64_t now_ms);

void stn_live_status(const stn_t *s, const stn_in_t *in, stn_live_status_t *out);

/**
 * Take the notes written since the last call (at most STN_NOTES; older
 * ones are dropped when more pile up). @return count copied.
 */
unsigned stn_take_notes(stn_t *s, stn_note_t *out, unsigned cap);

/* ---- status --------------------------------------------------------------- */

bool stn_surveying(const stn_t *s, int64_t now_ms);
uint32_t stn_survey_left_s(const stn_t *s, int64_t now_ms);
/** Seconds to the next CONFIG_FAILED retry (0 = none scheduled). */
uint32_t stn_config_retry_in_s(const stn_t *s, int64_t now_ms);

/**
 * The alerts to report: `alerts` plus STN_ALERT_NO_RECEIVER while the state
 * is NO_RECEIVER, the receiver is not at its loader (RECEIVER_IN_LOADER)
 * and maintenance is off. Leaving the state clears it.
 */
uint32_t stn_alerts(const stn_t *s);

/**
 * NO_RECEIVER or DETECTING: seconds until the next detection pass (0 = due
 * now or running). UINT32_MAX in every other state.
 */
uint32_t stn_next_detect_in_s(const stn_t *s, int64_t now_ms);

/** CONFIG_FAILED backoff: delay after the n-th failure in a row (n >= 1), ms. */
int64_t stn_apply_backoff_ms(uint32_t failures);

/** Detection candidates: stored baud first, then the short list; full adds the rest; no duplicates. */
unsigned stn_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, unsigned cap);

#ifdef __cplusplus
}
#endif
