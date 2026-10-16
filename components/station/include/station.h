/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Station supervisor: owns the receiver lifecycle and the GNSS link's mode.
 *
 * It is the only component that takes COMMAND/RAW leases on the GNSS link.
 * Everything that needs the receiver reconfigured, flashed or reset (web UI,
 * update manager, health ladder) asks the supervisor, which serialises the
 * requests in its own task:
 *
 *   (boot) --journal open--> FLASH_RECOVERY --+
 *     |                                       v
 *     +--> DETECTING -> CONFIGURING -> STREAMING
 *              ^            |            |  request: reconfigure / flash / recover
 *              |            v            v
 *              +------- RECOVERING <- FLASHING
 *
 * Boot is stream-first: the link opens at the stored baud and RTCM is
 * published immediately. If at least one CRC-valid RTCM frame or two
 * checksum-valid text lines arrive within 3 s, the station streams, queries
 * the version without a lease and runs the applied check: supported model,
 * live firmware equal to gnss/applied_fw, gnss/applied_hash equal to the
 * profile's hash, and the profile's messages observed at their rates within
 * 30 s. When it passes, no configuring command is sent at all. When the
 * version query fails while RTCM flows, the station keeps streaming with the
 * stored identity marked unconfirmed and asks again every 10 min. Nothing
 * valid within 3 s -> DETECTING (COMMAND lease, baud scan). While detecting,
 * the loader prompt detector is armed: if the receiver sits at its loader
 * prompt, the station stops probing and sends the loader's restart key once
 * (at most once per boot). If the application does not come back there is
 * no flash journal to wait in, so the station raises
 * STATION_ALERT_RECEIVER_IN_LOADER, enters NO_RECEIVER and only listens
 * (nothing is transmitted) until the receiver streams again or the user asks
 * for a receiver restart.
 *
 * Automatic re-applies (applied check, profile drift, the health ladder's
 * reconfigure step) need gnss/autocfg = 1. A STATION_RECOVER_RECONFIGURE
 * request is refused without sending anything while gnss/autocfg = 0 or
 * while STATION_ALERT_CONFIG_FAILED is raised: after a failed apply the
 * station's own retry backoff (10 min, 30 min, 2 h, then every 6 h) owns the
 * re-applies, so the receiver is not reset by a rollback every few minutes.
 * PROVISIONAL, to be confirmed on hardware.
 *
 * Receiver flashing is always started by the user (station_request_flash
 * after an explicit confirmation), never by a schedule, a policy or a
 * receiver that stopped answering. It is crash-safe:
 *  - preconditions: no journal open, no app trial or crash watch running
 *    (lifecycle_app_trial_active), no app update running (the update slot,
 *    lifecycle_update_claim), not safe mode, the station STREAMING (valid
 *    RTCM or NMEA within the last 10 s, so the receiver runs its
 *    application), model "UM982", and the staged package passes every
 *    pre-flight rule on its RAM copy (um98x_flash_prepare);
 *  - before the first action the station claims the update slot, pins the
 *    staged package, takes a reboot inhibit and the flasher writes the
 *    journal (gnss/flash_job, a versioned record, see um98x_flash.h),
 *    updating it before every new state;
 *  - after the transfer: confirm the receiver's application with a version
 *    query (any build), re-apply the station profile and check it (for every
 *    confirmed build, whatever gnss/autocfg says), then close the journal and
 *    release the inhibit, the update slot and the staged package. Expected
 *    build -> success; the pre-flash build -> "old firmware still running";
 *    any other build -> "unexpected build"; neither retries automatically;
 *  - the reboot inhibit (and the update slot) is released at exactly one
 *    of: the journal reaching "needs user", the post-flash re-apply and its
 *    check, or the abort path's re-apply and its 120 s RTCM check;
 *  - at boot, right after the settings and before anything that could
 *    transmit or start Wi-Fi (station_early_boot), an open journal is read,
 *    the inhibit taken again (unless it waits for the user) and the recovery
 *    table picks the first bytes, bauds and timeouts from the journal state
 *    and the reset reason; after a power cut during the loader phase that
 *    is a race into the receiver's loader. Report-only until proven on
 *    hardware (CONFIG_UM98X_FLASH_RECOVERY_ACTIONS). The station then goes
 *    straight to FLASH_RECOVERY: no baud scan against a receiver sitting in
 *    its loader, health's GNSS ladder suspended. Recovery transfers only
 *    ever resend the same pinned package (same SHA-256 as the journal, the
 *    journalled model; gnss/model never gates anything), within the
 *    flasher's retry caps;
 *  - a journal that cannot be read (damaged) or that a newer schema wrote
 *    counts as open and waiting for the user: nothing is sent to the
 *    receiver (listening is allowed); the user may clear it;
 *  - with settings unavailable this boot (degraded mode) the journal's state
 *    is unknown: it counts as open (nothing is sent to the receiver, health's
 *    GNSS ladder stays suspended) but there is nothing to clear, and it does
 *    not hold back health's network-ladder reboot
 *    (flash_journal_settings_unavailable);
 *  - not in safe mode: station_init and station_start do not run there and
 *    nothing is sent on the UART. station_get_status() still reads the
 *    journal from settings, so the UI can show it and offer "clear the
 *    journal"; retry and restart wait for normal mode. Safe mode counts as
 *    reduced lockout (app installs and the safe-mode retry reboots stay
 *    allowed). The crash that led to safe mode may be in the flash path
 *    itself; retrying it there would take the web UI down with it;
 *  - while the boot-time recovery is report-only, an open session that is
 *    not waiting for the user is logged (what the recovery would do) and
 *    then waits for the user like one in "needs user" (reduced lockout,
 *    flash.phase UM98X_FLASH_NEEDS_USER in the status); its record is left
 *    unchanged, so a build that can recover it still finds it as written;
 *  - a boot with no open journal releases any staged package left over;
 *    in safe mode station_safe_mode_boot() does that without the UART.
 * Every successful detection persists gnss/model and gnss/fw.
 *
 * While CONFIGURING or FLASHING, during the health ladder's rescan,
 * reconfigure and receiver-reset steps, and while a flash journal is open, it
 * pauses the casters (a planned source stall: they idle, then close after
 * their stall limit; the ledger counts planned downtime). Lease state, the
 * time since the last lease release, the survey window and the journal state
 * are published in the status for the health monitor's suspension rules.
 *
 * The receiver's output is watched in windows of 30 s (longer for slow
 * station messages): every MSM type of the profile at its period (the
 * regional QZSS and NavIC types only when they arrive at all), and 1005,
 * 1033 and, when the receiver accepted it, 1230 within twice their period,
 * at least 30 s (not 1005/1033 while a survey runs). The first window after
 * boot (or after a receiver restart, 30 s later) is the applied check's
 * last part; the one after an apply checks that apply (if it fails, the
 * station does not re-apply on drift until the receiver restarts or someone
 * asks); later ones watch for drift: two failed windows in a row while RTCM
 * flows re-apply (gnss/autocfg = 1, no CONFIG_FAILED). Silence is a stall
 * for the health ladder, not drift. PROVISIONAL, to be confirmed on
 * hardware.
 *
 * Base position: an apply that starts a survey-in, a receiver restart while
 * the profile surveys, or a running survey found at boot opens the survey
 * window; the base mode is read every 60 s while it is open. A finished
 * survey is locked automatically (gnss/autocfg = 1, no CONFIG_FAILED): the
 * latest 1005 becomes the fixed position, applied as ECEF, and the next 1005
 * must be within 5 mm (else STATION_ALERT_POSITION_MISMATCH). A fixed base
 * that sends no RTCM after a receiver restart while it still reports a fixed
 * base raises STATION_ALERT_ANTENNA_MOVED; it is never re-surveyed on its own.
 *
 * Receiver settings read back: every apply session reads the receiver's
 * configuration dump; the station keeps (in RAM, with the time of the
 * read-back) what it said about BeiDou B1C/B2a in RTCM, multipath
 * mitigation, the elevation mask and the base antenna descriptor, and the
 * outcome of the antenna and B1C/B2a steps (sent only when the read-back
 * asks for them; a rejection does not fail the session). Unknown after a
 * reboot until the next apply session.
 *
 * Signals per group: every apply session also asks which signals the
 * running signal group configures on the main antenna input (rx_bands,
 * um98x_bands.h) and compares them with what the profile's group should
 * give: a report that differs raises STATION_ALERT_SIGNALS_MISMATCH, one
 * that matches or is not read (a firmware without the query refuses it)
 * clears it. Never a failed apply.
 *
 * A signal group the receiver refuses outright (a negative reply: nothing
 * changed, no restart; build 25164 refuses every pair but 4 5, 3 6, 5 0 and
 * 7 0) goes back to the group it runs and is applied again at once, without
 * a RESET in between when there is such a group to go back to.
 *
 * Restarting: between a receiver's restarting answer ("The Board is
 * resetting,...", its reply to a command sent while it restarts) and its
 * banner, receiver_restarting says so; it counts as restart evidence once
 * unless the restart is the station's own.
 *
 * First-apply hold (CONFIG_STATION_HOLD_FIRST_APPLY, bench builds and the
 * sdkconfig.hold overlay): at init, before anything reaches the receiver,
 * a device whose profile was never applied (no gnss/applied_hash) and whose
 * gnss/autocfg was never set gets gnss/autocfg = 0, gnss/recovery = 2
 * (alerts only, unless set) and gnss/hold = 1 in one settings batch. While
 * gnss/hold is present (any build: it is kept in NVS) the station sends
 * only queries (identity, settings, base mode) and a loader prompt seen
 * during detection gets no restart key (RECEIVER_IN_LOADER, listen-only;
 * the user's receiver restart still works). The first successful apply
 * erases it, and so does turning gnss/autocfg on (the web API, then
 * station_first_apply_hold_refresh()); a factory reset erases it with the
 * other user settings. station_status_t.first_apply_held reports it. A hold
 * that is due but cannot be stored (settings unusable this boot, or the
 * write failed) holds for this boot only, in RAM, with automatic
 * configuration off: it fails closed, never open.
 *
 * Receiver maintenance (station_maintenance_start; "hands off" while the
 * user works on the receiver through its other port, e.g. a PC tool or a
 * reflash): a user action with a duration (15 min, 30 min, 1 h, 2 h, 4 h;
 * default 30 min; extendable while on, at most 4 h left; "end now").
 * Refused while a receiver-flash journal is open, while a receiver session
 * or job runs (offer again when it ends), while the live view's
 * satellite-in-view output may be on (it is turned off first, so nothing
 * done through the other port can save it; offer again in a few seconds)
 * and in safe mode. While on, the
 * GNSS link refuses every write and every lease, so nothing reaches the
 * receiver: no detection, no queries, no apply of any kind (the request
 * functions refuse with ESP_ERR_INVALID_STATE), no loader key, no RESET, no
 * receiver flash, no ladder step. Reception goes on: every CRC-valid RTCM
 * frame still reaches the casters. Restart evidence is noted and triggers
 * nothing. The ledger counts the time as planned downtime. The station
 * reports STATION_STATE_IDLE meanwhile (listening only, as in a boot's
 * first seconds), which suspends health's GNSS ladder; rx_maintenance tells
 * the two apart (the UI's banner with the time left). Exit (the
 * user, or the time is up) runs the stream-first path as at boot. A reset
 * that keeps the ESP32's RTC memory keeps the state with its time left
 * (RTC memory, never NVS; the boot only listens until it ends): a software
 * reset, and also a reset that reads as power-on or brownout, since on the
 * ESP32-S3 the EN pin (the USB auto-reset, the RST button) reads as
 * power-on while the receiver keeps running and may be mid-work; a record
 * that survived says the ESP32's memory kept its power. Neither a restart
 * banner nor the power-on verdict ends it then: the user may have
 * restarted the receiver, or cycled its power, as part of the work. A
 * power cut that clears the memory ends it. Entry and exit are journalled.
 * PROVISIONAL, to be confirmed on hardware (whether input on the link can
 * disturb the receiver while it is worked on through its other port, and
 * whether the ESP32-S3 keeps its RTC memory through an EN reset).
 *
 * Live view (station_live_view_start; satellites first): a window the user
 * opens for CONFIG_STATION_LIVE_VIEW_MIN minutes (extendable up to
 * CONFIG_STATION_LIVE_VIEW_MAX_MIN; it also closes when no admin client
 * asked for it, station_live_view_touch, for
 * CONFIG_STATION_LIVE_VIEW_IDLE_S). While it is open the receiver's
 * satellite-in-view output is turned on, unsaved (never SAVECONFIG), every
 * CONFIG_STATION_LIVE_VIEW_SKY_PERIOD_S, for the sky plot's azimuth and
 * elevation (gnss_obs_get_live); it is turned off when the window closes.
 * Each toggle is one configuring command under a short COMMAND lease (the
 * casters pause for it; journalled with its length), only while the
 * station streams with nothing else to do: never while it configures,
 * detects, recovers or flashes, never in maintenance, never with
 * gnss/autocfg = 0, never for a model it does not configure, and never when
 * the profile sends the output permanently. A toggle sends its command at
 * most twice (about 7 s without an answer). An "on" without an answer may
 * have taken: it is turned off when the window closes (extending sends it
 * again). An ESP32-only reset while it may be on is remembered (RTC memory,
 * like maintenance), and the next boot turns it off once the station
 * streams. After a receiver reconfiguration, reset or restart the output is
 * simply off (status says why); starting the view again turns it on again.
 * Everything else in the live view comes from the RTCM stream without any
 * receiver traffic.
 * Whether toggling the output interrupts RTCM is PROVISIONAL, to be
 * confirmed on hardware.
 *
 * Dependencies point down only: the station never calls health or
 * fw_update; it learns about an app trial from lifecycle.
 *
 * Safe mode skips station_init() and station_start() and runs only
 * station_safe_mode_boot(). station_get_status() works uninitialised (state
 * IDLE, journal read from settings); the request functions return
 * ESP_ERR_INVALID_STATE, except clearing the journal.
 *
 * Status: state machine and request API are ours. The detection/config and
 * flash details it drives are PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "gnss_types.h"
#include "station_sgt.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(STATION_EVENT);

/** STATION_EVENT ids; the event data differs per id. */
typedef enum {
    STATION_EVENT_STATE_CHANGED,     /**< data: station_state_t (the new state) */
    STATION_EVENT_FLASH_PROGRESS,    /**< data: um98x_flash_progress_t (a phase change, else at most once a second; FAILED here is a transfer the session goes on from) */
    STATION_EVENT_RECEIVER_RESTARTED,/**< the receiver restarted on its own (banner or an unsolicited version log); no data */
    STATION_EVENT_FLASH_ENDED,       /**< data: um98x_flash_progress_t: the job's last message (a session, the user's action, the journal's close or a package check ended; phase DONE, FAILED or NEEDS_USER); station_status_t.flash_last says how */
} station_event_t;

typedef enum {
    STATION_STATE_IDLE = 0,      /**< not started (or safe mode), the first 3 s of a boot, or receiver maintenance (rx_maintenance): listening at the stored baud, nothing sent */
    STATION_STATE_DETECTING,
    STATION_STATE_CONFIGURING,
    STATION_STATE_STREAMING,
    STATION_STATE_FLASHING,
    STATION_STATE_FLASH_RECOVERY,/**< an interrupted receiver flash is being finished */
    STATION_STATE_RECOVERING,
    STATION_STATE_NO_RECEIVER,   /**< detection failed: short baud list every 30 s for 5 min, then the full list every 5 min (listen-only while the receiver sits at its loader) */
} station_state_t;

/** Recovery actions the health ladder may request (mildest first). */
typedef enum {
    STATION_RECOVER_RESYNC = 0,      /**< reset the framer and drop buffered input (gnss_link_resync, no lease) */
    STATION_RECOVER_LINK_RESTART,    /**< reinstall the UART at the stored baud (gnss/baud, else the default) */
    STATION_RECOVER_RESCAN,          /**< UART break, then full baud rescan */
    STATION_RECOVER_RECONFIGURE,     /**< re-apply the base profile; refused while gnss/autocfg = 0 or CONFIG_FAILED is raised */
    STATION_RECOVER_RECEIVER_RESET,  /**< reset the receiver (command, or reset GPIO if wired) */
} station_recovery_t;

/**
 * Alerts the station raises and clears itself (bit flags, stable values;
 * shown next to health's alerts).
 */
typedef enum {
    STATION_ALERT_UNSUPPORTED_MODEL  = 1u << 0, /**< receiver model is not one we configure or flash: stream only */
    STATION_ALERT_CONFIG_FAILED      = 1u << 1, /**< the last profile apply failed; retried on the backoff (config_retry_in_s) with
                                                     automatic configuration on (config_auto_retry), else only by the user */
    STATION_ALERT_POSITION_MISMATCH  = 1u << 2, /**< 1005 position more than 5 mm from the locked one after a lock */
    STATION_ALERT_ANTENNA_MOVED      = 1u << 3, /**< fixed base and no RTCM after a receiver restart: antenna moved more than 300 m? re-survey */
    STATION_ALERT_RECEIVER_ATTENTION = 1u << 4, /**< RTCM did not resume after a receiver update session ended */
    STATION_ALERT_RECEIVER_IN_LOADER = 1u << 5, /**< receiver found at its loader prompt without an update session: listen-only until the user acts */
    STATION_ALERT_NO_RECEIVER        = 1u << 6, /**< no receiver found (state NO_RECEIVER; not with RECEIVER_IN_LOADER or in maintenance): next_detect_in_s says when the next search runs */
    STATION_ALERT_SIGNALS_MISMATCH   = 1u << 7, /**< the signals the receiver reports for its signal group are not what the profile's group
                                                     should configure (rx_bands, um98x_bands.h); set and cleared by each apply session's report */
} station_alert_t;

/**
 * What the user confirmed for a receiver update (download or upload): the
 * package identity and the expected result. Recovery never builds one.
 */
typedef struct {
    uint8_t sha256[32];              /**< manifest entry, or the digest the user confirmed for an upload */
    uint32_t size;
    char expected_fw[48];            /**< firmware string the update should produce */
    uint32_t expected_build;         /**< must not be lower than the receiver's build */
    bool cross_layout_confirmed;     /**< extra confirmation for a different partition layout */
} station_flash_request_t;

/** User choices for an open receiver-flash journal (and a receiver found at its loader). */
typedef enum {
    STATION_FLASH_ACTION_RETRY = 0,       /**< flash the same pinned package again (needs user confirmation) */
    STATION_FLASH_ACTION_RESTART_RECEIVER,/**< leave the loader ("6"), then check the version; also for STATION_ALERT_RECEIVER_IN_LOADER */
    STATION_FLASH_ACTION_CLEAR_JOURNAL,   /**< forget the session, release the package, resume normal operation */
} station_flash_action_t;

/** Why a maintenance request was refused (station_maintenance_start / _end). */
typedef enum {
    STATION_MAINT_OK = 0,
    STATION_MAINT_REFUSED_FLASH,        /**< a receiver-flash journal is open */
    STATION_MAINT_REFUSED_SESSION,      /**< a receiver session or job runs: offer again when it ends */
    STATION_MAINT_REFUSED_DURATION,     /**< not one of 15 min, 30 min, 1 h, 2 h, 4 h */
    STATION_MAINT_NOT_ON,               /**< "end now" without the state */
    STATION_MAINT_REFUSED_NOT_RUNNING,  /**< the station does not run (safe mode) */
    STATION_MAINT_REFUSED_SKY_OUTPUT,   /**< the live view's satellite-in-view output may be on: the station turns it off first (the open view keeps running without it); offer again in a few seconds */
} station_maint_why_t;

/** The receiver's satellite-in-view output as the live view sees it. */
typedef enum {
    STATION_LIVE_SKY_OFF = 0,     /**< no window */
    STATION_LIVE_SKY_PENDING,     /**< wanted; waits for a free moment (why) */
    STATION_LIVE_SKY_ON,          /**< on for this window (unsaved) */
    STATION_LIVE_SKY_ON_PROFILE,  /**< the profile sends it permanently */
    STATION_LIVE_SKY_BLOCKED,     /**< not available in this window (why) */
    STATION_LIVE_SKY_STOPPING,    /**< the window closed; turned off at the next free moment */
} station_live_sky_t;

/** Why the satellite-in-view output is not on (or why the view was refused). */
typedef enum {
    STATION_LIVE_WHY_NONE = 0,
    STATION_LIVE_WHY_BUSY,           /**< a receiver job or session runs */
    STATION_LIVE_WHY_NOT_STREAMING,  /**< no receiver streaming (detecting, no receiver, boot) */
    STATION_LIVE_WHY_MAINTENANCE,    /**< receiver maintenance: nothing is sent (pending), or turned off for a maintenance request (blocked: extend to want it again) */
    STATION_LIVE_WHY_AUTOCFG_OFF,    /**< gnss/autocfg = 0: the user configures the receiver */
    STATION_LIVE_WHY_UNSUPPORTED,    /**< not a receiver model this firmware configures */
    STATION_LIVE_WHY_RECONFIGURED,   /**< receiver reconfigured, reset or restarted: start the view again */
    STATION_LIVE_WHY_REJECTED,       /**< the receiver rejected the command (sky_reply) */
    STATION_LIVE_WHY_NO_ANSWER,      /**< no answer to the command (it may be on: turned off when the window closes; extending sends it again) */
    STATION_LIVE_WHY_FLASH,          /**< receiver update session (also refuses the view) */
    STATION_LIVE_WHY_NOT_RUNNING,    /**< the station does not run (safe mode; refuses the view) */
} station_live_why_t;

/** Why the last live view window closed. */
typedef enum {
    STATION_LIVE_END_NONE = 0,
    STATION_LIVE_END_USER,
    STATION_LIVE_END_EXPIRED,
    STATION_LIVE_END_IDLE,           /**< no admin client asked for it for CONFIG_STATION_LIVE_VIEW_IDLE_S */
    STATION_LIVE_END_FLASH,          /**< a receiver update began */
} station_live_end_t;

/**
 * After a power-on reset: did the receiver keep running? On the ESP32-S3 a
 * reset by the EN pin (the USB serial chip's auto-reset that every flash
 * uses, the RST button) reads as power-on, and the receiver, on the same
 * supply, keeps running through it. So a power-on boot counts as a power
 * loss (health's power-lost alert) and as a receiver restart only on the
 * receiver's word: its restart banner before any data, or a version log
 * whose header tells it has run for less time than the ESP32 (while it has
 * no time, the header's millisecond field counts from its restart) ->
 * RESTARTED; RTCM or NMEA within 3.5 s of the ESP32's start, or a version
 * log that tells it has run since before the ESP32 started -> KEPT; nothing
 * of that within 30 s -> UNKNOWN (counted as neither). Listening is enough,
 * so it is judged in receiver maintenance too. Journalled once
 * (DIAG_EV_RX_BOOT). PROVISIONAL, to be confirmed on hardware.
 */
typedef enum {
    STATION_RX_BOOT_NONE = 0,      /**< not a power-on boot: nothing to judge */
    STATION_RX_BOOT_PENDING,       /**< a power-on boot, judged within 30 s */
    STATION_RX_BOOT_KEPT,          /**< the receiver kept running: only the ESP32 was reset (its reset pin) */
    STATION_RX_BOOT_RESTARTED,     /**< the receiver restarted with it: the power was cut */
    STATION_RX_BOOT_UNKNOWN,       /**< nothing it sent told */
} station_rx_boot_t;

/** What told it (station_rx_boot_t). */
typedef enum {
    STATION_RX_BOOT_BY_NONE = 0,
    STATION_RX_BOOT_BY_RESTART,    /**< its restart banner (or restarting answer, an unasked version log) before any data */
    STATION_RX_BOOT_BY_UPTIME,     /**< its version log's time since its restart */
    STATION_RX_BOOT_BY_STREAM,     /**< RTCM or NMEA at once */
} station_rx_boot_by_t;

/** How the last receiver update (or package check) ended. */
typedef enum {
    STATION_FLASH_OUT_NONE = 0,       /**< none this boot */
    STATION_FLASH_OUT_SUCCESS,        /**< the expected build runs */
    STATION_FLASH_OUT_OLD_FIRMWARE,   /**< the build from before the update still runs */
    STATION_FLASH_OUT_UNEXPECTED,     /**< another build runs */
    STATION_FLASH_OUT_NOT_DONE,       /**< ended before the point of no return: the receiver is unchanged */
    STATION_FLASH_OUT_REJECTED,       /**< the package failed a pre-flight rule (failed_rule): nothing sent */
    STATION_FLASH_OUT_NOT_STARTED,    /**< the receiver could not be identified, sat at its loader, or the journal could not be written: nothing sent */
    STATION_FLASH_OUT_NEEDS_USER,     /**< the session waits for the user's choice */
    STATION_FLASH_OUT_CHECKED,        /**< check only (flashing not built in): every rule passed */
} station_flash_outcome_t;

/** The last receiver update session (or check) this boot. */
typedef struct {
    uint8_t outcome;                 /**< station_flash_outcome_t */
    uint8_t failed_rule;             /**< REJECTED: the first rule that failed (1..12) */
    bool cross_layout;               /**< the package's partition layout differs from the receiver's */
    bool check_only;                 /**< only checked: this build does not flash */
    bool reapply_ok;                 /**< the profile was re-applied and checked (RTCM / the observed set came back) */
    char firmware[48];               /**< the receiver's firmware afterwards ("" unknown) */
    uint32_t build;
    char reason[96];                 /**< what happened, in words */
    int64_t ended_unix;              /**< 0 without a trusted clock */
    uint32_t ended_age_s;            /**< since it ended (UINT32_MAX = none this boot) */
} station_flash_last_t;

/** Live view state (the satellites themselves: gnss_obs_get_live). */
typedef struct {
    bool open;
    uint32_t left_s;                 /**< until the window closes */
    uint32_t idle_left_s;            /**< until it closes for lack of an admin client */
    uint32_t open_s;                 /**< since it opened */
    uint32_t max_s;                  /**< longest window, extensions included */
    uint8_t sky;                     /**< station_live_sky_t */
    uint8_t sky_why;                 /**< station_live_why_t */
    uint16_t sky_period_s;           /**< satellite-in-view output period while on */
    uint8_t last_end;                /**< station_live_end_t of the last window */
    char sky_reply[64];              /**< the receiver's reply to the last failed toggle ("" none) */
} station_live_status_t;

/** Status snapshot. */
typedef struct {
    station_state_t state;
    um98x_info_t receiver;           /**< live, or last persisted when unresponsive */
    bool receiver_responding;        /**< the receiver answers (read at the time of the call): it streams (valid RTCM within
                                          10 s), or it sent a checksum-valid line (a reply, a log, NMEA) after its last RTCM
                                          frame (none this boot: any line); never while no receiver is found. A receiver that
                                          answers when asked and sends nothing unasked stays answering between questions */
    bool identity_confirmed;         /**< receiver identity confirmed by this boot's version query */
    uint32_t alerts;                 /**< station_alert_t flags */
    bool profile_applied;            /**< receiver runs the stored profile */
    bool rtcm_differs;               /**< the latest observation window saw RTCM flowing that differs from the profile
                                          (a checked message missing or at the wrong rate); never without RTCM */
    bool rtcm1230_supported;         /**< gnss/rtcm1230_ok: the receiver accepted RTCM1230 at the last apply (required in the observed set only then) */
    bool rtcm1230_known;             /**< gnss/rtcm1230_ok is stored (false: never applied; rtcm1230_supported is then false and means "not applied yet") */
    bool config_failed;              /**< STATION_ALERT_CONFIG_FAILED is raised */
    uint32_t config_retry_in_s;      /**< until the station's next apply retry (0 = none scheduled, or none at all) */
    bool config_auto_retry;          /**< a failed apply is retried by the station on its own (gnss/autocfg on, a model it
                                          configures); false: only the user's apply (the first-apply hold included) */
    bool reconfigure_refused;        /**< STATION_RECOVER_RECONFIGURE would be refused now (gnss/autocfg = 0 or config_failed) */
    bool position_locked;            /**< profile uses a locked fixed position */
    um98x_base_position_t position;  /**< locked or last surveyed position */
    bool surveying;                  /**< inside the survey window */
    uint32_t survey_window_left_s;   /**< until the survey window ends (0 when not surveying) */
    bool lease_held;                 /**< a COMMAND or RAW lease is held now (read live from the link, also while a job runs) */
    uint32_t since_lease_release_ms; /**< since the last lease release (UINT32_MAX = none this boot) */
    bool flash_journal_open;         /**< open, or present but unreadable; valid in safe mode too */
    bool flash_journal_readable;     /**< flash_journal holds the record (false: damaged or a newer schema: waits for the user) */
    bool flash_journal_settings_unavailable; /**< settings unavailable this boot: journal state unknown (nothing sent, nothing to clear) */
    bool flash_session_running;      /**< a receiver update job runs or is queued (a new update, the user's retry or restart, the boot-time recovery): the journal does not wait for the user, whatever its record says, and cannot be cleared */
    um98x_flash_journal_t flash_journal;
    uint32_t detect_failures;        /**< consecutive failed detections */
    bool detect_saw_bytes;           /**< the last failed detection pass received bytes at some baud (health's byte rule on entering NO_RECEIVER) */
    uint32_t detect_passes;          /**< detection passes completed since boot, found or not (health looks at a blocked ESP32 reboot again only after a newer pass) */
    uint32_t next_detect_in_s;       /**< NO_RECEIVER or DETECTING: seconds until the next detection pass (0 = due or running), read at the time of the call; UINT32_MAX otherwise */
    uint64_t rtcm_excluded;          /**< frames of types 1101-1107 kept out of the hub */
    um98x_flash_progress_t flash;    /**< valid while FLASHING / FLASH_RECOVERY */

    /* Receiver settings read back at the last apply session this boot. */
    bool rx_settings_valid;          /**< a configuration dump was read this boot (else every value is unknown) */
    um98x_rx_settings_t rx_settings; /**< B1C/B2a in RTCM, multipath mitigation, elevation mask, antenna descriptor */
    uint32_t rx_settings_age_s;      /**< since that read-back */
    int64_t rx_settings_unix;        /**< its wall-clock time, 0 = clock not set then */
    bool antenna_sent;               /**< the last apply session sent the antenna descriptor */
    bool antenna_ok;                 /**< ... and the receiver accepted it */
    char antenna_reply[64];          /**< its rejection text or "timeout" ("antenna model rejected: ...") */
    bool b1cb2a_sent;                /**< the last apply session turned BeiDou B1C/B2a in RTCM on */
    bool b1cb2a_ok;                  /**< ... and the receiver accepted it */
    char b1cb2a_reply[64];           /**< its rejection text or "timeout" */

    /* After a power-on reset (read at the time of the call). */
    uint8_t rx_boot;                 /**< station_rx_boot_t */
    uint8_t rx_boot_by;              /**< station_rx_boot_by_t */

    /* Receiver maintenance (hands off). */
    bool rx_maintenance;             /**< on: nothing is sent to the receiver; health suspends its GNSS ladder */
    uint32_t maintenance_left_s;     /**< until it ends by itself */
    uint32_t maintenance_chosen_s;   /**< chosen duration, extensions included */

    station_live_status_t live;      /**< live view */

    bool first_apply_held;           /**< gnss/hold: the receiver's first configuration waits for the user (only queries are sent);
                                          also a hold held for this boot only (it could not be stored) */
    bool autocfg_off_this_boot;      /**< automatic configuration is off for this boot whatever gnss/autocfg says: a first-apply
                                          hold that could not be stored (until the user turns automatic configuration on) */

    /* Receiver updates. */
    bool flash_available;            /**< receiver flashing is built in (station_flash_available) */
    bool pkg_check_running;          /**< a check of the staged package runs (check-only builds) */
    station_flash_last_t flash_last; /**< the last session or check this boot */

    /* The receiver as it answers right now (read at the time of the call). */
    bool receiver_restarting;        /**< it answered "The Board is resetting" within 20 s and its restart banner has not come yet */
    uint64_t rx_answers;             /**< checksum-valid command-layer lines since boot (acks, logs, banners, restarting answers) */
    uint32_t rx_answer_age_ms;       /**< since the last of them, UINT32_MAX = never: "the receiver answers" */

    /* What the running signal group configures (the last apply session's query this boot). */
    um98x_bands_t rx_bands;          /**< state UNKNOWN until a session asked (um98x_bands.h) */
    uint32_t rx_bands_age_s;         /**< since that query, UINT32_MAX = none this boot */
    int64_t rx_bands_unix;           /**< its wall-clock time, 0 = clock not set then */
    uint8_t rx_bands_profile_master; /**< the profile's main-antenna group it was compared with */
    bool rx_bands_match;             /**< the report names that group and lists its signals (false unless state OK) */
    char rx_bands_reply[64];         /**< the receiver's refusal text, or "timeout" ("" when read) */

    /* Signal groups. */
    bool rx_sg_valid;                /**< an apply session's read-back named the receiver's signal group this boot */
    station_sg_t rx_sg;              /**< ... that group (the latest read-back) */
    bool sg_refused;                 /**< the receiver refused a signal group (kept until the next profile save) */
    station_sg_t sg_refused_group;   /**< the refused pair */
    bool sg_back_valid;              /**< the profile went back to sg_back (else: an ordinary failed apply, CONFIG_FAILED) */
    station_sg_t sg_back;
    char sg_refused_reply[64];       /**< the receiver's reply ("signal group rejected" when its read-back kept another group) */
    station_sgt_brief_t sg_test;     /**< the signal-group comparison, in short */
} station_status_t;

/**
 * Early boot step, called by app_main right after settings_init() and before
 * Wi-Fi or any other init (not in safe mode): read the receiver-flash
 * journal; when one is open (and not waiting for the user) take the reboot
 * inhibit again; when the recovery table calls for the race into the
 * receiver's loader, run it through um98x_flash under a RAW lease that is
 * kept for station_start(). Report-only unless
 * CONFIG_UM98X_FLASH_RECOVERY_ACTIONS. Errors are logged; boot continues.
 */
esp_err_t station_early_boot(void);

/**
 * Safe-mode boot step (instead of station_init/station_start), after
 * pkg_store_init(): read the journal from settings without the UART and,
 * with none open, release any staged package left over, so nothing stays
 * staged for the whole safe-mode episode. Sends nothing to the receiver.
 */
esp_err_t station_safe_mode_boot(void);

/**
 * Wire GNSS link sinks (RTCM -> hub + observation summary, lines -> command
 * layer, framer resets -> hub), load the profile and the journal; with no
 * journal open, release any staged package left over.
 */
esp_err_t station_init(void);

/** Start the supervisor task (journal recovery, or stream-first detect -> configure -> stream). */
esp_err_t station_start(void);

/**
 * Status snapshot. Works before station_init() (safe mode): state IDLE,
 * journal read from settings. Most fields are what the station task
 * published after its last step; the live view window (`live`: open,
 * left_s, idle_left_s, open_s, sky, sky_why, last_end), the no-receiver
 * alert and next_detect_in_s are computed from the supervisor's state at
 * the time of the call, so a live-view start or stop shows at once even
 * while a long job (a detection pass) runs.
 */
esp_err_t station_get_status(station_status_t *out);

/**
 * The user's "check again" while no receiver is found: the next detection
 * pass starts now, without moving the search schedule on (30 s apart for
 * the first 5 min of the episode, then every 5 min; the full baud list
 * after two failed short passes and always after 5 min). Accepted while a
 * pass already runs. Async.
 * @return ESP_OK; ESP_ERR_INVALID_STATE unless the state is NO_RECEIVER
 *         (also uninitialised, safe mode, receiver maintenance).
 */
esp_err_t station_request_detect(void);

/**
 * Re-apply the stored base profile (after the user edits it). Async; runs
 * whatever gnss/autocfg says, also while CONFIG_FAILED is raised, once the
 * receiver streams.
 * @return ESP_ERR_INVALID_STATE uninitialised (safe mode), with a flash
 *         journal open or in receiver maintenance, ESP_ERR_NOT_SUPPORTED for
 *         an unsupported model.
 */
esp_err_t station_request_reconfigure(void);

/**
 * The user turned gnss/autocfg on (the web API, after it erased a stored
 * gnss/hold): a hold held for this boot only ends too, with its automatic
 * configuration off, and gnss/hold is read again; the status and the
 * detection's loader-key rule follow at once. Works uninitialised (nothing
 * to update then).
 */
void station_first_apply_hold_refresh(void);

/**
 * Flash the package staged in pkg_store to the receiver, only after an
 * explicit user confirmation (the request carries what was confirmed; the
 * banner's Update and the manual upload's confirm both come here). Async;
 * progress via station_get_status() and STATION_EVENT. The update slot is
 * claimed here, atomically (an app update can never start alongside); the
 * session then runs on the station task: the receiver identified (its live
 * version query), the staged package checked on its RAM copy against every
 * rule (a refusal releases it), the package pinned, the reboot inhibit
 * taken, the journal opened, the session, the profile re-applied and
 * checked, the journal closed (station_status_t.flash_last says how it
 * ended).
 * @return ESP_ERR_NOT_SUPPORTED when flashing is not built in (see
 *         station_request_package_check); ESP_ERR_INVALID_STATE while a
 *         journal is open, during an app trial or crash watch, while an app
 *         update runs (update slot held by the app), in receiver maintenance,
 *         while a signal-group comparison runs (station_sg_test_running),
 *         unless the station is STREAMING (valid RTCM or NMEA within the
 *         last 10 s), uninitialised (safe mode) or for an unsupported model;
 *         ESP_ERR_NOT_FOUND if nothing is staged.
 */
esp_err_t station_request_flash(const station_flash_request_t *req);

/** Receiver flashing is built in (CONFIG_UM98X_FLASH_ENABLE). */
bool station_flash_available(void);

/**
 * Check the staged package against every pre-flight rule without flashing
 * (the receiver identity is the one confirmed this boot; nothing is sent to
 * the receiver), then release it. For builds without flashing, so the user
 * sees whether the package would pass. Async: station_status_t.flash_last
 * (outcome CHECKED or REJECTED) and a STATION_EVENT_FLASH_PROGRESS at the end.
 * @return ESP_ERR_INVALID_STATE uninitialised, while a journal is open or a
 *         check runs; ESP_ERR_NOT_FOUND if nothing is staged.
 */
esp_err_t station_request_package_check(const station_flash_request_t *req);

/* ---- bench console (bench builds only; ESP_ERR_NOT_SUPPORTED elsewhere) ---------------- */

/** One bench operation: the same values as um98x_bench_op_t. */
typedef enum {
    STATION_BENCH_LISTEN = 0,   /**< listen only at `baud` for `seconds` */
    STATION_BENCH_VERSION,      /**< the version query at `baud` */
    STATION_BENCH_ENTRY,        /**< loader entry at `baud`, banner and menu captured, "6", version */
    STATION_BENCH_MENU2,        /**< entry, menu item 2, start bytes for `seconds`, CAN CAN / "q", "6", version (no data frame) */
    STATION_BENCH_IDLE,         /**< entry, `seconds` untouched at the prompt, "6", version */
    /* Station-side operations (not the flasher's): */
    STATION_BENCH_PROMPT_WATCH = 16, /**< COMMAND lease, host at `baud`, the report-only loader prompt observer for `seconds`; nothing transmitted; the link's baud restored */
    STATION_BENCH_LINK_RESTART,      /**< the ladder's link restart (gnss_link_restart, driver kept) on the station task; nothing sent to the receiver */
} station_bench_op_t;

typedef struct {
    uint8_t op;                 /**< station_bench_op_t */
    uint32_t baud;
    uint32_t seconds;
    bool record_only;           /**< LISTEN: record what COM1 carries (the receiver is flashed over its other port) */
} station_bench_req_t;

typedef struct {
    bool pass;
    char line[200];             /**< the PASS/FAIL line with the recorded values */
} station_bench_res_t;

/**
 * Run one bench operation on the station task (RAW lease through the
 * station, casters paused; refused in maintenance, while a receiver session,
 * job or journal is open). Blocks the caller until it ends or timeout_ms.
 */
esp_err_t station_bench_run(const station_bench_req_t *req, station_bench_res_t *out, uint32_t timeout_ms);

/**
 * Bench check "stop key from boot": from app_main, before anything else uses
 * the receiver's UART (no journal may be open); the station is not running
 * yet. The RAW lease is released afterwards.
 */
esp_err_t station_bench_boot_race(uint32_t max_ms, station_bench_res_t *out);

/**
 * Bench: start a receiver update of the staged package like
 * station_request_flash (the same refusals, a running signal-group
 * comparison included), but stop sending after stop_after_blocks
 * acknowledged blocks (0 = never; the failure policy then retries), or
 * restart the ESP32 right after the journal is written in reset_at_state
 * (0 = never), for the boot-time table.
 */
esp_err_t station_bench_flash(const station_flash_request_t *req, uint32_t stop_after_blocks, uint8_t reset_at_state);

/**
 * Bench: write a journal in `state` for the staged package (identity from
 * this boot's receiver), without any receiver traffic, for the boot-time
 * table test (reset the ESP32 afterwards). Refused while a session runs.
 */
esp_err_t station_bench_journal_set(uint8_t state);

/**
 * Expected receiver downtime of an update with a package of this size,
 * seconds: transfer at the loader baud (10 bits per byte + 10 %) plus
 * CONFIG_STATION_FLASH_RESTART_ALLOWANCE_S. For the update confirmation
 * dialog only; never a gate.
 */
uint32_t station_expected_flash_downtime_s(uint32_t package_size);

/**
 * Act on an open journal at the user's request. RETRY and RESTART_RECEIVER
 * need normal mode and a journal waiting for the user (RETRY is also refused
 * during an app trial and while an app update runs); RESTART_RECEIVER also
 * works without a journal while STATION_ALERT_RECEIVER_IN_LOADER is raised.
 * CLEAR_JOURNAL works uninitialised (safe mode) and also for an unreadable
 * journal, but not while a session, a retry or restart (queued or running)
 * or the boot-time recovery works on the receiver (flash_session_running),
 * and not when the settings are unavailable (nothing can be cleared then).
 * An accepted RETRY or RESTART_RECEIVER clears station_status_t.flash_last
 * (the stop it answers); the job's STATION_EVENT_FLASH_ENDED and the status
 * then tell how it ended.
 * @return ESP_ERR_NOT_FOUND without an open journal (or loader alert),
 *         ESP_ERR_INVALID_STATE when the action is not allowed now.
 */
esp_err_t station_flash_user_action(station_flash_action_t action);

/** Copy the last receiver text of the flash session (up to 4 KB). @return bytes copied. */
size_t station_get_flash_log(char *buf, size_t len);

/**
 * Run one recovery action. Async; ignored if an equal or stronger one is
 * queued or in progress. The station is RECOVERING while it runs.
 * @return ESP_ERR_INVALID_STATE when refused without sending anything:
 *         uninitialised, a flash journal open, not STREAMING (or
 *         CONFIGURING / RECOVERING; while DETECTING or NO_RECEIVER the
 *         station's own detection schedule runs), or RECONFIGURE while
 *         gnss/autocfg = 0 or STATION_ALERT_CONFIG_FAILED is raised (health
 *         counts a refused step as failed at once); ESP_ERR_INVALID_ARG for
 *         an unknown action.
 */
esp_err_t station_request_recovery(station_recovery_t action);

/**
 * Lock the current base position (persist it, switch the profile to FIXED,
 * reconfigure). Async.
 * @return ESP_ERR_NOT_FOUND before the receiver sent a 1005,
 *         ESP_ERR_NOT_SUPPORTED for an unsupported model,
 *         ESP_ERR_INVALID_STATE uninitialised or with a flash journal open.
 */
esp_err_t station_lock_position(void);

/** Forget the locked position and survey again (reconfigure). Async. Errors as station_request_reconfigure(). */
esp_err_t station_request_resurvey(void);

/**
 * Receiver maintenance (see above): enter for duration_s (900, 1800, 3600,
 * 7200 or 14400; 0 = 1800), or, while on, extend by it (at most 4 h left).
 * Effective at once: the link refuses every write and lease from the
 * return on.
 * @param why optional: the refusal reason (STATION_MAINT_OK on success)
 * @return ESP_OK; ESP_ERR_INVALID_ARG (duration); ESP_ERR_INVALID_STATE
 *         (refused: flash journal, a running session, the live view's
 *         satellite-in-view output, safe mode).
 */
esp_err_t station_maintenance_start(uint32_t duration_s, station_maint_why_t *why);

/**
 * The user's "end now": the station leaves maintenance within a second
 * and runs the stream-first path. ESP_ERR_NOT_FOUND when it is not on,
 * ESP_ERR_INVALID_STATE in safe mode.
 */
esp_err_t station_maintenance_end(void);

/**
 * Open the live view for `minutes` (0 = CONFIG_STATION_LIVE_VIEW_MIN), or
 * extend an open one by that much (at most CONFIG_STATION_LIVE_VIEW_MAX_MIN
 * from now); either also turns the satellite-in-view output on again when
 * a receiver reconfiguration had ended it, and counts as a touch.
 * @param why optional: the refusal reason (STATION_LIVE_WHY_NONE on success)
 * @return ESP_OK; ESP_ERR_INVALID_STATE (safe mode / not running:
 *         STATION_LIVE_WHY_NOT_RUNNING; receiver flash:
 *         STATION_LIVE_WHY_FLASH).
 */
esp_err_t station_live_view_start(uint32_t minutes, station_live_why_t *why);

/**
 * An admin client asked for the live view (the web API calls it on every
 * live-view request); without one for CONFIG_STATION_LIVE_VIEW_IDLE_S the
 * window closes by itself. ESP_ERR_NOT_FOUND when no window is open.
 */
esp_err_t station_live_view_touch(void);

/** Close the live view now (the output is turned off at the next free moment). ESP_ERR_NOT_FOUND when none is open. */
esp_err_t station_live_view_stop(void);

/* ---- signal-group comparison ----------------------------------------------------
 *
 * The user's comparison of two signal groups (station_sgt.h): each is
 * measured for phase_s seconds of good samples after a minute of settling,
 * the one running now first; then the station waits up to 24 h of running
 * time for the user to keep one (the receiver stays on the group measured
 * last), else it goes back to the group from before. Each switch is the
 * station's own profile write and re-apply: the casters pause for it and the
 * ledger counts it as planned downtime. Stored in gnss/sg_test, so a restart
 * resumes it. While a test runs the web answers 409 signal_test_running to
 * every other change of the receiver (profile, automatic configuration,
 * apply, position lock, maintenance, receiver updates).
 */

/**
 * Start a comparison. @return ESP_OK (*why STATION_SGT_OK), ESP_ERR_INVALID_ARG
 * (*why _E_GROUPS or _E_PHASE), ESP_ERR_INVALID_STATE (*why the refusal).
 */
esp_err_t station_sg_test_start(const station_sg_t groups[2], uint32_t phase_s, station_sgt_err_t *why);

/** Keep group 0 or 1 (while deciding). @return as station_sg_test_start (_E_NOT_DECIDING, _E_GROUPS). */
esp_err_t station_sg_test_keep(unsigned group, station_sgt_err_t *why);

/** Stop a running comparison (back to the group from before). @return ESP_OK, ESP_ERR_INVALID_STATE (_E_IDLE). */
esp_err_t station_sg_test_stop(station_sgt_err_t *why);

/** The comparison as GET /api/gnss/signal-test shows it (an idle view when the station does not run). */
esp_err_t station_sg_test_get(station_sg_test_t *out);

/** A comparison runs: every other change of the receiver is refused meanwhile. */
bool station_sg_test_running(void);

/** The user saved a profile: a signal-group refusal shown until now is cleared. */
void station_profile_saved(void);

#ifdef __cplusplus
}
#endif
