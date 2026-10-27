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
 *     once per boot, only when no model was ever stored and no valid RTCM
 *     frame arrived in the last STN_RTCM_QUIET_MS: while RTCM flows a
 *     receiver that does not answer between frames gains nothing from an
 *     exclusive line, and the casters keep streaming), then the applied check: live
 *     firmware == gnss/applied_fw, gnss/applied_hash == the profile's hash
 *     (both checked by the caller, STN result static_ok) and the observed
 *     set over one window (station_obs.h). All true: nothing is sent. Any
 *     false and gnss/autocfg = 1 and no CONFIG_FAILED: apply. The observed
 *     set needs RTCM: a window without any leaves the check pending, judged
 *     by the first window with RTCM, while the static part holds (the
 *     receiver acknowledged this profile on this firmware at its last
 *     apply): a re-apply could not be checked without RTCM, and does not
 *     help a receiver without an antenna; such a stall is the health
 *     ladder's (its reconfigure step, under the user's recovery mode).
 *     With the recovery set to alerts only (in.ladder_alerts_only) the
 *     ladder re-applies nothing, so the check does, once: a receiver that
 *     lost its configuration (a factory reset over its other port, a
 *     restart without the saved one) would otherwise stay unconfigured.
 *     The window after it is the apply's own check, which a window without
 *     RTCM does not repeat: one apply per check (a start, a receiver
 *     restart, the end of maintenance). With the static part false the
 *     apply runs at once. An unsupported
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
 *   A version query with a lease (no model ever stored, nothing flowing)
 *   does not pause the casters: there is no RTCM to hold back, and the
 *   ledger keeps such an outage unplanned.
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
 * check).
 *
 * Maintenance across an ESP32 reset (stn_maint_boot, stn_maint_restore):
 * the caller keeps two records of it. Its RTC record (stn_maint_rtc_*),
 * written every step, is exact and costs no flash, but only a reset that
 * keeps the ESP32's RTC memory keeps it: a software reset, a crash, a
 * watchdog or a brownout. A reset by the EN pin (on the ESP32-S3 the USB serial chip's
 * auto-reset and the RST button; it reads as power-on) powers the chip's
 * RTC domain down and loses it, while the receiver, which may be mid-work,
 * keeps running. So the caller also stores a copy in settings
 * (stn_maint_store_*): at entry, at every extension and every
 * STN_MAINT_SAVE_S, with the time left STN_MAINT_STORE_SHORT_S short of
 * what it was, so a reset before the next save never resumes more than was
 * left; erased when maintenance ends. Flash wear: about 240 small writes
 * for 4 h of maintenance, next to the ledger's one a minute around the
 * clock. A boot resumes from the RTC record when it
 * is intact and from the run right before this boot (an "ended" RTC record
 * makes a stored copy stale), else from the stored copy, never when a
 * receiver flash journal is open or the previous boot ended in a factory
 * reset. The time across the reset is taken off: by the RTC timer when it
 * ran on (a software, crash, task/interrupt watchdog or brownout reset:
 * lifecycle_reset_keeps_rtc_timer), else at
 * least this boot's run time, and, when the record carried the end by the
 * trusted clock, by that clock once there is one (stn_maint_clock). So
 * resumed maintenance never ends later than the time chosen (apart from
 * the time the ESP32 was held in reset while no clock tells it), and ends
 * at once when that time ran out meanwhile. A power-on or brownout boot
 * whose receiver restarted too (stn_rxboot_judge: RESTARTED) lost the power
 * for both: a maintenance resumed at that boot and not extended since ends
 * then (stn_maint_power_cut, stn_rxboot_follow). Kept, or unknown: it goes
 * on.
 *
 * What the station knew of the receiver (stn_maint_known_t) goes into the
 * stored copy too (version 2): its identity and the signal group it runs,
 * the profile applied, the survey window, the fixed-position verdict, the
 * alerts. Nothing is checked during maintenance, so that is what a
 * maintenance without a reset still shows; a resumed one shows it until it
 * ends (stn_maint_boot_t.known): the
 * survey window goes back into the core (stn_maint_restore_survey; its end
 * kept relative to maintenance's own end, so the copy's age and the time
 * across the reset come off both alike), the rest is the caller's to report.
 * Then the stream-first path checks the receiver as at boot. The copy is
 * written whichever record gives the time, so it serves both.
 *
 * Power-on reset (stn_rxboot_judge): the ESP32-S3 reports a reset by its
 * EN pin (the USB serial chip's auto-reset that every flash uses, the RST
 * button) as a power-on reset, and the receiver keeps running through it
 * (our bench: no restart banner, the heartbeat on its other port without a
 * gap). So a power-on boot counts as a power loss, and as a receiver
 * restart, only on the receiver's word, judged from what it sent since the
 * ESP32 started: its restart banner (or other restart evidence) before any
 * data -> it restarted with the ESP32; a version log that tells its time
 * since its restart (while it has no time) -> it restarted with the ESP32
 * when that time started after the ESP32's, else it kept running; RTCM or
 * NMEA within STN_RXBOOT_DATA_MS of the ESP32's start, before any restart
 * evidence -> it kept running (a receiver that just got power needs longer
 * to boot: about 4.4 s from a RESET to its banner, 1.6 s more to data). The
 * earliest of these decides. Nothing within STN_RXBOOT_WINDOW_MS -> unknown
 * (counted as neither). Listening is enough for each of them: the verdict
 * also comes in receiver maintenance, where nothing is sent. A brownout
 * boot is judged the same way (stn_rxboot_judged): the ESP32's detector
 * watches its own 3.3 V rail, and on our board each half has a regulator
 * of its own on the shared USB 5 V (two LDOs, one per half: inferred from
 * the board, not measured), so a dip of the 5 V input takes both down while
 * one the ESP32's own load causes (Wi-Fi transmit peaks through its
 * regulator) leaves the receiver running; the reset says only that the
 * ESP32's rail dipped, the receiver's word says the rest. PROVISIONAL,
 * to be confirmed on hardware (the receiver's boot time after a real power
 * cut was not measured).
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
 * First-apply hold (gnss/hold; written by a build with the hold option on a
 * device whose profile was never applied, stn_hold_at_boot): gnss/autocfg is
 * 0, so nothing is applied on its own, and while the caller reports the
 * hold (stn_set_first_apply_held) a loader prompt seen during detection or
 * the ladder's rescan gets no restart key: RECEIVER_IN_LOADER and
 * listen-only, as when the boot's one key was used. The user's receiver
 * restart and the user's apply still run.
 *
 * A receiver that sends no RTCM (none this boot, or none for
 * STN_RTCM_QUIET_MS: no antenna, no output configured, its RTCM stopped)
 * may answer only when asked. While STREAMING the station asks it
 * (STN_JOB_ASK: the version query without a lease, one line, nothing
 * paused, nothing configured; after every other job): at once when no line
 * came since its last RTCM frame, then whenever no line came for
 * STN_ASK_MS, at most once every STN_ASK_MS; so its answers stay fresh and
 * one that hung or was unplugged stops counting as answering
 * (stn_responding). A receiver whose RTCM stops is asked within seconds,
 * before health declares the stall, so the ladder's first look already
 * knows whether it answers.
 * Never in maintenance (no job at all), never while RTCM flows. The replies
 * are the station's own evidence: health keeps them out of its stall class
 * (station_status_t.rx_unasked_line_age_ms).
 *
 * Fixed-position check (um98x_fixcheck.h; the caller passes the fixed
 * profile's position in stn_in_t): is the fixed position where the
 * receiver is? While STREAMING with a fixed profile and a model we
 * configure, the receiver's own position is asked without a lease
 * (STN_JOB_OWN_FIX: BASEPOSA, or SPPNAVA once the receiver refused that,
 * echoed the set position or gave nothing usable STN_FIX_BASEPOS_TRIES
 * times; one line, nothing paused, nothing configured), first STN_FIX_FIRST_MS
 * after the check is armed (the profile became fixed or changed, a
 * receiver restart, a successful apply), then every STN_FIX_PERIOD_MS
 * until three usable solutions decide, or STN_FIX_TRIES queries did not;
 * the next check runs STN_FIX_RECHECK_MS later with manual setup. Automatic
 * setup instead continues every STN_FIX_PERIOD_MS, also while surveying,
 * and exposes only usable measured positions (never FIXEDPOS echoes).
 * Consistent horizontal displacement beyond both 3 m and four reported
 * standard deviations for five minutes queues STN_CHANGE_RESURVEY. Missing
 * fixes, gaps over 40 s, and inconsistent positions restart observation. The caller holds
 * the RTCM hub and casters while relocating, until CHECK_POSITION succeeds.
 * A far verdict, or a fixed
 * height that is the receiver's height above sea level, raises
 * STN_ALERT_FIXED_FAR (the warning alone does not hold streaming), a near one
 * clears it; each is noted when it changes the warning. A change of the fixed
 * position, or a survey profile, drops the verdict and the alert at once
 * (noted when the alert stood). Never in maintenance (no job at all).
 *
 * Notes (stn_note_t) tell the caller what to journal: maintenance entry
 * and exit, live view opened, extended, closed, output turned on or off,
 * the fixed-position warning raised, cleared, or dropped for a new
 * position.
 *
 * Timings PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "station_obs.h"
#include "um98x_base.h"
#include "um98x_fixcheck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STN_LISTEN_MS          3000      /**< stream-first listen at boot */
#define STN_IDENTITY_RETRY_MS  600000    /**< unconfirmed identity: query again */
#define STN_RTCM_QUIET_MS      3000      /**< no valid RTCM frame this long: nothing flows (the leased identity query) */
#define STN_FRESH_MS           10000     /**< "the receiver streams": a valid RTCM frame (or line) this recently */
#define STN_ANSWER_SLACK_MS    1000      /**< a line this soon after the last RTCM frame may be that output's own: no answer after it */
#define STN_ASK_MS             60000     /**< no RTCM flowing and no line this long: the receiver is asked (STN_JOB_ASK) */
#define STN_ANSWER_FRESH_MS    90000     /**< no RTCM flowing: a line this recent (after the last RTCM frame) is an answer
                                              (asked every STN_ASK_MS, plus a busy station's delay and the query's timeout) */
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
#define STN_MAINT_SAVE_S       60u           /**< the stored copy's period */
#define STN_MAINT_STORE_SHORT_S (STN_MAINT_SAVE_S + 10u) /**< its time left is this much short: until the next save (which may come a little late), plus the reset and the boot before this boot's uptime counts */
#define STN_LIVE_DEFAULT_MS    (10 * 60000)  /**< live view window (the caller passes its own) */
#define STN_LIVE_MAX_MS        (60 * 60000)  /**< longest window, extensions included */
#define STN_LIVE_IDLE_MS       120000        /**< closes when no admin client asked for it this long */
#define STN_SKY_RETRY_MS       60000         /**< a failed "output off" is tried again after this */
#define STN_QUERY_WAIT_MS      30000         /**< the user's query is dropped when it could not run within this */
#define STN_SKY_OFF_TRIES      3             /**< then it is left on until the receiver restarts */
#define STN_NOTES              8             /**< notes kept for the caller between two steps */
#define STN_FIX_FIRST_MS       60000         /**< fixed-position check: first query after it was armed (a fix after a restart: under 30 s) */
#define STN_FIX_PERIOD_MS      20000         /**< ... between the queries of one check */
#define STN_FIX_TRIES          15            /**< ... queries without a decision before the check gives up */
#define STN_MOVE_CONFIRM_MS  300000        /**< five minutes of consistent displacement */
#define STN_MOVE_MAX_GAP_MS   40000         /**< longer gaps restart movement observation */
#define STN_MOVE_FLOOR_M      3.0           /**< guard against optimistic standalone uncertainty */
#define STN_FIX_BASEPOS_TRIES  3             /**< ... base-position answers without a usable solution, then the single-point log */
#define STN_FIX_RECHECK_MS     3600000       /**< ... the next check after one ended */
#define STN_FIX_BUSY_MS        5000          /**< ... nothing was sent (the command layer was busy) or the other log is asked: again after this */

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
#define STN_ALERT_SIGNALS_MISMATCH   (1u << 7)   /**< the receiver's signal report differs from the profile's group (stn_note_signals) */
#define STN_ALERT_FIXED_FAR          (1u << 8)   /**< the fixed position is far from the receiver's own solution (the check above) */

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
    STN_JOB_OWN_FIX,         /**< no lease: the receiver's own position, one line (BASEPOSA, or SPPNAVA with sppnav) */
    STN_JOB_ASK,             /**< no lease: the version query to a receiver that never streamed, only to see that it answers */
    STN_JOB_QUERY,           /**< the user's read-only query (um98x_query.h): with a COMMAND lease when `leased` (casters paused), else none */
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
    STN_WHY_SG_TEST,         /**< the signal-group comparison's switch (the caller wrote the group into the profile) */
    STN_WHY_SG_BACK,         /**< the receiver refused the profile's signal group: the group it runs went back into the profile */
} stn_why_t;

typedef struct {
    uint8_t kind;            /**< stn_job_kind_t */
    bool leased;             /**< IDENTIFY: with a COMMAND lease (no model ever stored, no RTCM flowing); QUERY: with the COMMAND lease */
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
    bool sppnav;             /**< OWN_FIX: ask the single-point log (the base-position log was refused or gave nothing usable) */
    uint8_t query;           /**< QUERY: um98x_query_t */
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
    /**
     * The receiver refused the profile's signal group and the caller wrote a
     * group it runs back into the stored profile: no CONFIG_FAILED, no
     * backoff (the refused pair is gone), a re-apply right away
     * (STN_WHY_SG_BACK; STN_WHY_SG_TEST for the comparison's own switch).
     */
    bool sg_back;
    /* SKY, QUERY */
    uint32_t lease_ms;       /**< how long the lease held RTCM back (QUERY: 0 without a lease) */
    /* FLASH */
    uint8_t flash_end;       /**< stn_flash_end_t */
    bool loader_key_sent;    /**< the identification sent the loader's restart key (the boot's one) */
    bool in_loader;          /**< ... and the application did not come back */
    /* OWN_FIX: err STN_OK (a log that parsed: own), STN_E_FAIL (the receiver
     * refused the query), STN_E_TIMEOUT (no log, or one not read),
     * STN_E_STATE (nothing sent) */
    um98x_own_fix_t own;
} stn_result_t;

/** Inputs sampled by the caller for each stn_next() / stn_done(). */
typedef struct {
    int64_t now_ms;
    uint64_t rtcm_frames;    /**< framer: CRC-valid RTCM frames (published or held) */
    uint64_t text_lines;     /**< framer: checksum-valid text lines */
    bool autocfg;            /**< gnss/autocfg */
    bool ladder_alerts_only; /**< gnss/recovery = 2: the health ladder only alerts, its reconfigure step sends nothing */
    bool model_known;        /**< a model was stored (gnss/model) at some point */
    bool profile_survey;     /**< the profile surveys (else fixed) */
    uint32_t survey_s;       /**< the profile's survey length */
    bool rtcm1230_ok;        /**< gnss/rtcm1230_ok */
    bool profile_sky;        /**< the profile sends satellite-in-view output permanently */
    const stn_obs_t *obs;    /**< snapshot of the observed-set tracker (may be NULL) */
    bool fixed_valid;        /**< the profile is fixed: fixed_llh is its position (the fixed-position check runs) */
    double fixed_llh[3];     /**< latitude, longitude (degrees), ellipsoidal height (m) */
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

/** Why maintenance ended (STN_NOTE_MAINT_OFF's b). */
typedef enum {
    STN_MAINT_END_USER = 0,  /**< the user's "end now" */
    STN_MAINT_END_TIME,      /**< its time was up (also when it ran out while the ESP32 was off, by the clock) */
    STN_MAINT_END_POWER,     /**< resumed after a power-on reset that also restarted the receiver: a power cut */
} stn_maint_end_t;

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
    STN_NOTE_MAINT_OFF,      /**< a = time spent (s), b = stn_maint_end_t */
    STN_NOTE_LIVE_OPENED,    /**< a = window length (s) */
    STN_NOTE_LIVE_EXTENDED,  /**< a = time left (s) */
    STN_NOTE_LIVE_CLOSED,    /**< a = time open (s), b = stn_live_end_t */
    STN_NOTE_SKY_ON,         /**< a = lease length (ms), b = stn_err_t of the job */
    STN_NOTE_SKY_OFF,        /**< as SKY_ON */
    STN_NOTE_SKY_LEFT_ON,    /**< turning it off failed STN_SKY_OFF_TRIES times: left on until the receiver restarts */
    STN_NOTE_FLASH_CLOSE,    /**< close the receiver-flash journal now: a = 1 the re-apply's check passed, 0 it did not (RECEIVER_ATTENTION raised) */
    STN_NOTE_APPLY_FAILED,   /**< an apply that sent something failed (CONFIG_FAILED raised): a = the station's own retry
                                  in s, 0 = none (automatic configuration off: only the user's apply), b = 1 the
                                  receiver was lost (detection follows) */
    STN_NOTE_FIXED_FAR,      /**< STN_ALERT_FIXED_FAR raised: a = horizontal distance (m), b = fixed height minus the
                                  receiver's (m), both rounded (the medians that decided) */
    STN_NOTE_FIXED_NEAR,     /**< it cleared: within the limits again (a, b as FIXED_FAR) */
    STN_NOTE_FIXED_CHANGED,  /**< it cleared: the fixed position changed (or the profile surveys now); checked again */
    STN_NOTE_FIXED_SEA_LEVEL, /**< STN_ALERT_FIXED_FAR raised (or kept) because the fixed height is the receiver's height
                                   above sea level (a, b as FIXED_FAR) */
    STN_NOTE_QUERY,          /**< the user's query ran: a = lease length (ms, 0 = no lease), b = stn_err_t | query << 8 */
    STN_NOTE_QUERY_DROPPED,  /**< the user's query did not run (not within STN_QUERY_WAIT_MS, or maintenance began): b = query
                                  | its request's query_seq << 8 */
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
    uint64_t rtcm_seen;        /**< in->rtcm_frames as last seen */
    int64_t rtcm_change_ms;    /**< when it last changed (STN_NEVER = never) */
    uint64_t lines_seen;       /**< in->text_lines as last seen */
    int64_t line_change_ms;    /**< when it last changed (STN_NEVER = never) */
    int64_t ask_ms;            /**< the last STN_JOB_ASK (STN_NEVER = none) */
    int64_t identify_ms;       /**< next query (STN_NEVER = none) */
    bool identify_restart;

    /* applied check and drift */
    bool obs_rearm;            /**< output: re-arm the tracker with the profile (caller clears) */
    bool check_pending;        /**< the next verdict is the applied check */
    bool verify_pending;       /**< the next verdict checks the apply that just ran */
    bool obs_on;               /**< windows are evaluated */
    int64_t check_ms;          /**< re-arm and check at this time (after restart evidence) */
    bool profile_applied;
    /**
     * The latest window saw RTCM that differs from the profile: RTCM flowed,
     * but a checked message was missing or at the wrong rate. Never set by a
     * window without any RTCM (no antenna, a silent receiver: a stall, which
     * other alerts explain); cleared by a passing window, a window without
     * RTCM, and whenever the windows start over (restart, detection, the
     * stream-first path, a successful apply). Health's profile-drift alert.
     */
    bool rtcm_differs;
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
    int64_t no_rx_ms;          /**< the station last found no receiver: a pass that found none, or the receiver left at its
                                    loader (STN_NEVER = not this boot); a line from before it is no answer now */
    bool loader_key_sent;      /**< the loader's restart key went out this boot */
    bool first_apply_held;     /**< gnss/hold: the first receiver configuration waits for the user (no loader key) */
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
    bool maint_resumed;        /**< resumed after an ESP32 reset and not extended since (stn_maint_power_cut) */
    int64_t maint_end_unix;    /**< resumed: its end by the trusted clock (UTC s), checked once there is a clock; 0 = none */

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

    /* the user's query (one at a time) */
    uint8_t query_want;        /**< queued: um98x_query_t + 1 (0 = none) */
    bool query_leased;         /**< it needs the COMMAND lease */
    int64_t query_until_ms;    /**< dropped when it has not run by then */
    uint16_t query_seq;        /**< counts accepted requests: tells the caller which one a drop is about */
    uint8_t flash_close;       /**< 0 none, 1 after the re-apply's window, 2 after RTCM within STN_FLASH_RTCM_MS */
    int64_t flash_close_ms;    /**< the journal closes by then whatever happens */
    int64_t flash_rtcm_ms;     /**< close kind 2: RTCM is required by then */

    /* fixed-position check */
    bool fix_on;               /**< a fixed profile is checked (armed) */
    bool fix_reference;        /**< fixed reference available (else live GNSS monitoring during survey) */
    bool fix_continuous;       /**< automatic setup: continue measuring and detect relocation */
    bool relocating;           /**< corrections held until the new survey is locked and its 1005 checked */
    bool relocation_ready;    /**< new fixed reference applied; only its check may release the hold */
    uint8_t move_samples;      /**< nonzero while observing a candidate relocation */
    int64_t move_started_ms;
    int64_t move_last_ms;
    um98x_own_fix_t move_anchor; /**< first displaced fix; later fixes must stay near it */
    um98x_own_fix_t own_fix;    /**< latest usable measured position, never a FIXEDPOS echo */
    int64_t own_fix_ms;
    double fix_ref[3];         /**< the fixed position the check and its verdict are about */
    int64_t fix_due_ms;        /**< next query (STN_NEVER = none) */
    bool fix_running;          /**< a check runs */
    uint8_t fix_tries;         /**< queries of the running check */
    bool fix_sppnav;           /**< the base-position log was refused or gave nothing usable: the single-point log is asked */
    bool fix_usable_seen;      /**< the running check got a usable solution */
    uint8_t fix_end;           /**< how the last check ended without a decision (stn_fix_end_t) */
    int64_t fix_sample_ms;     /**< the latest usable solution (STN_NEVER = none) */
    um98x_fixchk_t fix;        /**< the verdict and the running check's samples */

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

/**
 * An internal re-apply with its own reason (STN_WHY_SG_TEST, STN_WHY_SG_BACK):
 * like the user's, it runs once the receiver streams, also while
 * CONFIG_FAILED is raised; a re-apply queued for another reason takes this
 * one's reason, so its outcome reaches the comparison.
 */
void stn_request_apply_why(stn_t *s, uint8_t why);

/**
 * The group a refused signal group goes back to: the group the session read
 * back before the change (before_known), else the one stored at the last
 * successful apply (gnss/sg_ok), never the refused pair itself.
 * @return false when neither is known (the refusal is then an ordinary
 *         failed apply: CONFIG_FAILED and its backoff).
 */
bool stn_sg_fallback(uint8_t refused_m, uint8_t refused_s, bool before_known, uint8_t before_m, uint8_t before_s,
                     bool ok_known, uint16_t ok_pair, uint8_t *out_m, uint8_t *out_s);

/**
 * A different profile was stored, by the user or by the station itself (a
 * lock, a re-survey, a signal group): what the receiver acknowledged, and
 * the applied check, were about the one before (static_ok and
 * profile_applied false until an apply of it works; the user's save queues
 * that apply, stn_request_apply, the station's own change is applied by the
 * job that stored it or right after).
 */
void stn_profile_changed(stn_t *s);

/**
 * The receiver acknowledged the stored profile on the firmware it runs (the
 * static part of the applied check: the stored applied hash and firmware
 * match, this boot's identity confirmed it or an apply worked since); not
 * while the station looks for the receiver (DETECTING, NO_RECEIVER) or
 * updates it (FLASHING, FLASH_RECOVERY). It says nothing of the output:
 * profile_applied adds that a window saw the profile's RTCM (a receiver
 * without an antenna sends none).
 */
bool stn_profile_acknowledged(const stn_t *s);

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
 * The user's read-only query (STN_JOB_QUERY, um98x_query.h; leased: it
 * needs the COMMAND lease, and the casters pause for it): STREAMING only,
 * not in maintenance or with a flash journal open, one at a time (queued
 * or running), else STN_E_STATE. It runs at the station's next free
 * moment while streaming; one that has not run within STN_QUERY_WAIT_MS,
 * or when maintenance begins, is dropped (STN_NOTE_QUERY_DROPPED).
 */
int stn_request_query(stn_t *s, uint8_t query, bool leased, int64_t now_ms);

/** A user's query is queued or running. */
bool stn_query_pending(const stn_t *s);

/**
 * The user's "check again" in NO_RECEIVER: the next detection pass is due
 * now (the episode's schedule position, short_fails and episode start, is
 * kept). A pass that runs already counts. @return STN_OK, STN_E_STATE in
 * any other state or in maintenance.
 */
int stn_request_detect(stn_t *s, int64_t now_ms);

/** Unsolicited restart evidence (banner or version log). */
void stn_restart_seen(stn_t *s);

/* ---- first-apply hold --------------------------------------------------------- */

/** What a hold build writes at boot, in one settings batch (gnss namespace). */
typedef struct {
    bool autocfg_off;        /**< gnss/autocfg = 0 */
    bool recovery_alerts;    /**< gnss/recovery = 2 (alerts only): only when it was not set */
    bool hold;               /**< gnss/hold = 1 */
    bool ram_only;           /**< nothing can be written (settings unusable): hold for this boot only, autocfg off */
} stn_hold_writes_t;

/**
 * The first-apply hold at boot, before anything is sent to the receiver.
 * Due only with the build option on, on a device whose profile was never
 * applied (no gnss/applied_hash) and whose automatic configuration was
 * never set (no gnss/autocfg): then gnss/autocfg = 0, gnss/hold = 1, and
 * gnss/recovery = 2 unless it is set. Anything stored already (a later
 * boot of a held device, a device that was configured once, a user's
 * choice) writes nothing: the hold is written once and lives in NVS. A
 * presence the caller cannot read counts as present (nothing is written on
 * a doubt). With the settings unusable this boot the device's state is
 * unknown: the hold fails closed, for this boot only (ram_only, autocfg off,
 * nothing written); the caller holds in RAM as well when the batch fails.
 * @return true when the hold applies this boot (*out filled: the batch, or
 *         ram_only); false and *out cleared otherwise.
 */
bool stn_hold_at_boot(bool option, bool settings_ok, bool have_applied_hash, bool have_autocfg, bool have_recovery,
                      stn_hold_writes_t *out);

/** The caller's gnss/hold as it stands (read at init, after an apply and after the user turned autocfg on). */
void stn_set_first_apply_held(stn_t *s, bool held);

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

uint32_t stn_maint_left_s(const stn_t *s, int64_t now_ms);

/** What a maintenance record says (the caller's RTC record, or its stored copy). */
typedef struct {
    bool on;                 /**< false: maintenance ended (RTC record only: a stored copy left behind is stale) */
    uint32_t stamp;          /**< lifecycle's image-generation stamp of the run that wrote it */
    uint32_t left_ms;        /**< time left when written (the stored copy: at most, STN_MAINT_STORE_SHORT_S short) */
    uint32_t chosen_s;       /**< chosen duration, extensions added */
    uint64_t rtc_us;         /**< the RTC timer when written (RTC record only) */
    int64_t end_unix;        /**< its end by the trusted clock (UTC s); 0 = no clock when written */
} stn_maint_rec_t;

/**
 * The record of the state now (the caller adds stamp and rtc_us). `utc_now`:
 * the trusted clock (UTC s), 0 = none. `store`: for the stored copy, whose
 * time left is STN_MAINT_STORE_SHORT_S short (never below 0), so a reset
 * before the next save never resumes more than was left.
 */
void stn_maint_record(const stn_t *s, int64_t now_ms, int64_t utc_now, bool store, stn_maint_rec_t *rec);

/**
 * When the caller stores the copy again: maintenance on and nothing stored
 * yet (`saved_ms` STN_NEVER), the chosen duration changed (an extension),
 * or STN_MAINT_SAVE_S passed since the last save.
 */
bool stn_maint_store_due(const stn_t *s, uint32_t saved_chosen_s, int64_t saved_ms, int64_t now_ms);

/** The caller's RTC record of the maintenance state (magic, the record's fields, CRC). */
typedef struct {
    uint32_t magic;
    uint32_t stamp;
    uint32_t on;
    uint32_t left_ms;
    uint32_t chosen_s;
    uint32_t rtc_lo;
    uint32_t rtc_hi;
    uint32_t end_lo;
    uint32_t end_hi;
    uint32_t crc;
} stn_maint_rtc_t;

/** Write the RTC record. */
void stn_maint_rtc_write(stn_maint_rtc_t *r, const stn_maint_rec_t *rec);

/** Read the RTC record: false for a damaged or foreign one (random memory after a power cut). */
bool stn_maint_rtc_read(const stn_maint_rtc_t *r, stn_maint_rec_t *rec);

/**
 * What the station knew of the receiver when it wrote maintenance's stored
 * copy ("Maintenance across an ESP32 reset"): during maintenance nothing is
 * checked, so it stays what it was at entry (or at the resume).
 */
typedef struct {
    bool valid;                  /**< false: nothing known (a copy of version 1, or an unreadable part) */
    uint32_t profile_hash;       /**< the profile it is about (um98x_profile_hash): applied, verdict and alerts hold for it only */
    um98x_info_t receiver;       /**< the receiver's identity (receiver.valid: identified) */
    bool identity_confirmed;     /**< confirmed by a version query */
    bool sg_valid;               /**< an apply session's read-back named the signal group the receiver runs */
    uint8_t sg_master;
    uint8_t sg_slave;
    bool profile_applied;
    bool profile_acknowledged;   /**< stn_profile_acknowledged */
    uint32_t alerts;             /**< the alerts that stood (STN_ALERT_*; NO_RECEIVER, which is derived, left out) */
    uint8_t fix_state;           /**< stn_fix_state_t: the fixed-position check */
    float fix_horizontal_m;      /**< NEAR, FAR: the medians that decided */
    float fix_up_m;
    float fix_distance_m;
    bool fix_height_only;
    bool fix_sea_level;          /**< FAR: the fixed height is the receiver's height above sea level */
    /* The survey window (stn_maint_known_survey, stn_maint_restore_survey). */
    bool survey_active;          /**< a survey window was started */
    bool survey_complete;
    bool lock_done;              /**< its automatic lock was made (or given up) */
    uint32_t survey_s;
    int32_t survey_end_rel_ms;   /**< the window's end minus maintenance's end (the same before and after the reset) */
} stn_maint_known_t;

#define STN_MAINT_STORE_VERSION 2u
#define STN_MAINT_STORE_MAX     256u  /**< buffer for the stored copy */

/**
 * The stored copy (settings_codec: u8 version, u32 stamp, u32 left_ms, u32
 * chosen_s, i64 end_unix; version 2 adds u8 1 and what the station knew, or
 * u8 0 without it; newer writers may append). Only "on" is stored: the copy
 * is erased when maintenance ends. known may be NULL (nothing known).
 * @return length, 0 if cap is short.
 */
size_t stn_maint_store_encode(const stn_maint_rec_t *rec, const stn_maint_known_t *known, uint8_t *buf, size_t cap);

/**
 * Read the stored copy: false for an unreadable one. known (optional) gets
 * what the station knew, valid false for a copy without it (version 1) or
 * with an unreadable one (the time left still counts: hands off comes
 * first).
 */
bool stn_maint_store_decode(stn_maint_rec_t *rec, stn_maint_known_t *known, const uint8_t *buf, size_t len);

/**
 * The survey window into known (its other fields are the caller's), while
 * maintenance is on; survey_active false otherwise.
 */
void stn_maint_known_survey(const stn_t *s, stn_maint_known_t *known);

/** Where a boot's maintenance resumes from. */
typedef enum {
    STN_MAINT_FROM_NONE = 0,  /**< it does not */
    STN_MAINT_FROM_RTC,       /**< the RTC record */
    STN_MAINT_FROM_STORE,     /**< the stored copy (the RTC memory was lost: the EN pin, or the power was cut) */
} stn_maint_from_t;

/** What a boot knows (stn_maint_boot). */
typedef struct {
    bool blocked;            /**< nothing resumes: a receiver flash journal is open, or the previous boot ended in a factory reset */
    bool rtc_ok;             /**< the RTC record is intact and from the run right before this boot */
    stn_maint_rec_t rtc;
    bool rtc_timer_kept;     /**< the RTC timer ran on through the reset (lifecycle_reset_keeps_rtc_timer: software, crash, watchdog, brownout) */
    uint64_t rtc_now_us;     /**< the RTC timer now */
    bool store_ok;           /**< the stored copy is intact and from the run right before this boot */
    stn_maint_rec_t store;
    bool store_known;        /**< ... and it carried what the station knew (stn_maint_known_t valid) */
    int64_t uptime_ms;       /**< this boot's run time so far: the least the reset took */
} stn_maint_boot_in_t;

/** A boot's decision (stn_maint_boot). */
typedef struct {
    uint8_t from;            /**< stn_maint_from_t */
    bool expired;            /**< it was on, but its time ran out across the reset */
    bool elapsed_known;      /**< the time across the reset is known (the RTC timer) */
    bool known;              /**< from != NONE and the stored copy (of this maintenance: "on") carried what the station knew:
                                  the caller shows it until maintenance ends, stn_maint_restore_survey takes the survey */
    uint32_t left_ms;        /**< from != NONE: time left now */
    uint32_t chosen_s;       /**< from != NONE or expired: the chosen duration, extensions added */
    int64_t end_unix;        /**< from != NONE: to check by the trusted clock (0 = no need, or none known) */
} stn_maint_boot_t;

/**
 * Does maintenance resume at this boot, and with how much time left (see
 * "Maintenance across an ESP32 reset")? Never more than the record had left
 * less the time across the reset; nothing left resumes nothing.
 */
void stn_maint_boot(const stn_maint_boot_in_t *in, stn_maint_boot_t *out);

/** Back in maintenance after an ESP32 reset (stn_maint_boot's decision; nothing when from is NONE). */
void stn_maint_restore(stn_t *s, const stn_maint_boot_t *b, int64_t now_ms);

/**
 * The survey window the station had before the reset, back into the core
 * after stn_maint_restore (b->known): its end as far before or after
 * maintenance's end as it was, so it runs on as in a maintenance without a
 * reset, and after maintenance the station goes on with it (the boot
 * read-back of the base mode then finds it open and starts no new one).
 * Nothing without a resumed maintenance, or when no survey window was
 * started.
 */
void stn_maint_restore_survey(stn_t *s, const stn_maint_known_t *known);

/**
 * The trusted clock (UTC s, 0 = none yet) against a resumed maintenance's
 * end by the clock (maint_end_unix): it never ends later than that, and when
 * that end has passed it ends at the next step (its time was up). Applied
 * once, at the first trusted clock. A survey window restored with it moves
 * as much (the receiver surveyed on while the ESP32 was held in reset).
 */
void stn_maint_clock(stn_t *s, int64_t utc_now, int64_t now_ms);

/**
 * A power-on or brownout boot's verdict: the receiver restarted with the
 * ESP32 (the power was lost for both; stn_rxboot_follow). A maintenance
 * resumed at this boot and not extended since ends (STN_MAINT_END_POWER;
 * the stream-first path, as at boot).
 * @return true if it ended.
 */
bool stn_maint_power_cut(stn_t *s, const stn_in_t *in);

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
/**
 * A survey started and has not finished (by the base-mode read-back, or by
 * its time with a steady 1005): the receiver may still be surveying. True
 * inside the window (stn_surveying) and after it: a survey makes no
 * progress without satellites, so one without data outlasts the window,
 * which only times the station's mode poll, the station-message exemption
 * and health's survey rule. The lock follows once it finishes.
 */
bool stn_survey_unfinished(const stn_t *s, int64_t now_ms);
/**
 * What follows a failed apply, in words for its log line: "the station
 * tries again in 10 min" for retry_in_s > 0 (STN_NOTE_APPLY_FAILED's a),
 * else that nothing retries it on its own. Pure.
 * @return the length written (cut to cap - 1).
 */
size_t stn_apply_next_text(uint32_t retry_in_s, char *out, size_t cap);

/**
 * The station retries a failed apply on its own (CONFIG_FAILED's backoff):
 * only with automatic configuration on (gnss/autocfg; off while the first
 * apply is held) and for a model it configures (or one not identified yet).
 * Otherwise the user's apply is the only one.
 */
bool stn_config_auto_retry(const stn_t *s, bool autocfg);

/** Seconds to the next CONFIG_FAILED retry (0 = none scheduled, or none at all: stn_config_auto_retry false). */
uint32_t stn_config_retry_in_s(const stn_t *s, bool autocfg, int64_t now_ms);

/**
 * The alerts to report: `alerts` plus STN_ALERT_NO_RECEIVER while the state
 * is NO_RECEIVER, the receiver is not at its loader (RECEIVER_IN_LOADER)
 * and maintenance is off. Leaving the state clears it.
 */
uint32_t stn_alerts(const stn_t *s);

/**
 * An apply session's signal report was judged against the profile's
 * main-antenna group: 1 it matches (clears STN_ALERT_SIGNALS_MISMATCH), 0 it
 * differs (raises it), -1 unknown (the query was refused or not read:
 * clears it, an alert needs a report to stand on).
 */
void stn_note_signals(stn_t *s, int verdict);

/** How a fixed-position check ended without a decision. */
typedef enum {
    STN_FIX_END_NONE = 0,    /**< none ended yet, or the last one decided */
    STN_FIX_END_NO_SOLUTION, /**< no usable solution (no sky, or none reported in this mode) */
    STN_FIX_END_UNDECIDED,   /**< usable solutions that did not agree */
    STN_FIX_END_REFUSED,     /**< the receiver refused both logs */
} stn_fix_end_t;

/** The fixed-position check for the status. */
typedef enum {
    STN_FIX_OFF = 0,         /**< no check: the profile surveys, or not a model we configure */
    STN_FIX_CHECKING,        /**< no verdict yet (a check runs or is due; also after an undecided one) */
    STN_FIX_NEAR,            /**< within the limits */
    STN_FIX_FAR,             /**< beyond them, or a sea-level height (sea_level): STN_ALERT_FIXED_FAR */
    STN_FIX_NO_SOLUTION,     /**< no verdict: the last check got no usable solution */
    STN_FIX_REFUSED,         /**< no verdict: the receiver refused both logs */
} stn_fix_state_t;

typedef struct {
    uint8_t state;           /**< stn_fix_state_t */
    bool own_valid;
    um98x_own_fix_t own;
    uint32_t own_age_s;
    um98x_fix_offset_t offset; /**< NEAR, FAR: the medians that decided */
    bool height_only;        /**< FAR by the height alone (um98x_fix_height_only; always with sea_level) */
    bool sea_level;          /**< FAR: the fixed height is the receiver's height above sea level (um98x_fix_sea_level) */
    uint32_t checked_age_s;  /**< since the latest usable solution, UINT32_MAX = none */
} stn_fix_status_t;

void stn_fix_status(const stn_t *s, int64_t now_ms, stn_fix_status_t *out);

/**
 * NO_RECEIVER or DETECTING: seconds until the next detection pass (0 = due
 * now or running). UINT32_MAX in every other state.
 */
uint32_t stn_next_detect_in_s(const stn_t *s, int64_t now_ms);

/**
 * Whether the receiver answers (station_status_t.receiver_responding): it
 * streams (a CRC-valid RTCM frame within STN_FRESH_MS), or it sent a
 * checksum-valid text line (a reply, a log or an NMEA sentence) within
 * STN_ANSWER_FRESH_MS and after its last RTCM frame, more than
 * STN_ANSWER_SLACK_MS after it (any line, with no RTCM frame this boot).
 * Never in NO_RECEIVER, and a line from before the station last found no
 * receiver (no_rx_ms) does not count.
 *
 * So a receiver that answers when asked and sends nothing unasked (no
 * antenna, no output configured, its RTCM stopped) keeps answering: the
 * station asks it every STN_ASK_MS (STN_JOB_ASK); one that hung or was
 * unplugged stops answering once its last answer is that old. The same
 * evidence, by the same rule, as the health ladder's "answered after its
 * last RTCM frame".
 * @param rtcm_age_ms since the last CRC-valid RTCM frame, UINT32_MAX = none this boot
 * @param line_ms     when the last checksum-valid line arrived (now_ms's clock), STN_NEVER = none this boot
 */
bool stn_responding(const stn_t *s, int64_t now_ms, uint32_t rtcm_age_ms, int64_t line_ms);

/** CONFIG_FAILED backoff: delay after the n-th failure in a row (n >= 1), ms. */
int64_t stn_apply_backoff_ms(uint32_t failures);

/* ---- power-on or brownout reset: did the receiver keep running? ------------- */

#define STN_RXBOOT_WINDOW_MS 30000   /**< evidence counts this long after the ESP32's start */
#define STN_RXBOOT_DATA_MS   3500    /**< RTCM or NMEA this soon after the ESP32's start: the receiver was running */
#define STN_RXBOOT_MARGIN_MS 2000    /**< its clock started at least this much before the ESP32's: it kept running */

typedef enum {
    STN_RXBOOT_PENDING = 0,  /**< no evidence yet */
    STN_RXBOOT_KEPT,         /**< the receiver kept running: only the ESP32 was reset (its reset pin, or its own power dip) */
    STN_RXBOOT_RESTARTED,    /**< the receiver restarted with the ESP32: the power was cut (or dipped) for both */
    STN_RXBOOT_UNKNOWN,      /**< nothing told within STN_RXBOOT_WINDOW_MS */
} stn_rxboot_t;

typedef enum {
    STN_RXBOOT_BY_NONE = 0,
    STN_RXBOOT_BY_RESTART,   /**< its restart banner (or restarting answer, an unasked version log) before any data */
    STN_RXBOOT_BY_UPTIME,    /**< its version log's time since its restart */
    STN_RXBOOT_BY_STREAM,    /**< RTCM or NMEA at once */
} stn_rxboot_by_t;

/** What the receiver sent since the ESP32 started (all times: the ESP32's ms since its start). */
typedef struct {
    int64_t now_ms;
    int64_t restart_ms;      /**< first unsolicited restart evidence (STN_NEVER = none) */
    int64_t data_ms;         /**< first CRC-valid RTCM frame or checksum-valid NMEA sentence (STN_NEVER = none) */
    int64_t uptime_at_ms;    /**< arrival of the first version log that told the receiver's time since its restart (STN_NEVER = none) */
    uint32_t uptime_ms;      /**< that time */
} stn_rxboot_in_t;

/**
 * The verdict for a power-on or brownout boot (see "Power-on reset" above).
 * Final once it is not PENDING: the caller judges until then and keeps the
 * first verdict.
 * @param by out (optional): which evidence decided.
 */
stn_rxboot_t stn_rxboot_judge(const stn_rxboot_in_t *in, stn_rxboot_by_t *by);

/** The reset this boot follows, as far as the receiver's verdict goes. */
typedef enum {
    STN_RXBOOT_RESET_OTHER = 0,  /**< software, crash, watchdog...: the supply was not touched, nothing to judge */
    STN_RXBOOT_RESET_POWER_ON,   /**< power-on class: a power cut, or on the ESP32-S3 its EN pin */
    STN_RXBOOT_RESET_BROWNOUT,   /**< the ESP32's own rail dipped below its brownout level */
} stn_rxboot_reset_t;

/** Whether such a boot waits for the receiver's word (power-on and brownout). */
bool stn_rxboot_judged(stn_rxboot_reset_t reset);

/** What a verdict leads to. */
typedef struct {
    bool maint_ends;   /**< the receiver restarted with the ESP32, so the power was lost for both: a maintenance resumed at
                            this boot ends (stn_maint_power_cut) */
    bool power_kept;   /**< the gap before this boot was no power-off time (the ledger): a power-on boot whose receiver kept
                            running was the EN pin's; a brownout is the ESP32's own power dip whatever the receiver did */
} stn_rxboot_follow_t;

void stn_rxboot_follow(stn_rxboot_reset_t reset, stn_rxboot_t verdict, stn_rxboot_follow_t *out);

/** A checksum-valid text line that is an NMEA sentence ('$', five capitals, ','), not a command-layer line. */
bool stn_line_is_nmea(const char *line, size_t len);

/** Detection candidates: stored baud first, then the short list; full adds the rest; no duplicates. */
unsigned stn_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, unsigned cap);

/* ---- receiver flash lockout ------------------------------------------------ */

typedef struct {
    bool update_claimed;
    bool inhibit_held;
    bool request_reserved;
    int inhibit;
} stn_flash_lockout_t;

/* The lifecycle operations return 0 on success, otherwise their error code.
 * No receiver operation may run until take succeeds. Only resources acquired
 * by a failed take are unwound; an earlier claim/inhibit still belongs to the
 * session. Release ends that session, giving back the update slot before the
 * inhibit (whose release may immediately commit a queued reboot). */
typedef struct {
    void *ctx;
    int (*claim_update)(void *ctx);
    void (*release_update)(void *ctx);
    int (*acquire_inhibit)(void *ctx, int *token);
    void (*release_inhibit)(void *ctx, int token);
} stn_flash_lockout_ops_t;

int stn_flash_lockout_take(stn_flash_lockout_t *lockout, const stn_flash_lockout_ops_t *ops);
void stn_flash_lockout_release(stn_flash_lockout_t *lockout, const stn_flash_lockout_ops_t *ops);
/* Under the station lock. Reserve before claiming the lifecycle update slot,
 * then clear request_reserved after publishing the job or unwinding its
 * failed admission. A finishing session also reserves before publishing a
 * parked/absent journal, until its lifecycle release and final notification
 * are complete. A losing request must never claim/release that slot. */
bool stn_flash_lockout_reserve(stn_flash_lockout_t *lockout, bool busy);

#ifdef __cplusplus
}
#endif
