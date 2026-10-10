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
 *  - a boot with no open journal releases any staged package left over;
 *    in safe mode station_safe_mode_boot() does that without the UART.
 * Every successful detection persists gnss/model and gnss/fw.
 *
 * While CONFIGURING or FLASHING it pauses the casters (a planned source
 * stall: they idle, then close after their stall limit) and tells the ledger
 * the downtime is planned. Lease state, the time since the last lease
 * release, the survey window and the journal state are published in the
 * status for the health monitor's suspension rules.
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

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(STATION_EVENT);

/** STATION_EVENT ids; the event data differs per id. */
typedef enum {
    STATION_EVENT_STATE_CHANGED,     /**< data: station_state_t (the new state) */
    STATION_EVENT_FLASH_PROGRESS,    /**< data: um98x_flash_progress_t */
    STATION_EVENT_RECEIVER_RESTARTED,/**< the receiver restarted on its own (banner or an unsolicited version log); no data */
} station_event_t;

typedef enum {
    STATION_STATE_IDLE = 0,      /**< not started (or safe mode) */
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
    STATION_ALERT_CONFIG_FAILED      = 1u << 1, /**< the last profile apply failed; retried on the backoff (config_retry_in_s) */
    STATION_ALERT_POSITION_MISMATCH  = 1u << 2, /**< 1005 position more than 5 mm from the locked one after a lock */
    STATION_ALERT_ANTENNA_MOVED      = 1u << 3, /**< fixed base and no RTCM after a receiver restart: antenna moved more than 300 m? re-survey */
    STATION_ALERT_RECEIVER_ATTENTION = 1u << 4, /**< RTCM did not resume after a receiver update session ended */
    STATION_ALERT_RECEIVER_IN_LOADER = 1u << 5, /**< receiver found at its loader prompt without an update session: listen-only until the user acts */
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

/** Status snapshot. */
typedef struct {
    station_state_t state;
    um98x_info_t receiver;           /**< live, or last persisted when unresponsive */
    bool receiver_responding;
    bool identity_confirmed;         /**< receiver identity confirmed by this boot's version query */
    uint32_t alerts;                 /**< station_alert_t flags */
    bool profile_applied;            /**< receiver runs the stored profile */
    bool rtcm1230_supported;         /**< gnss/rtcm1230_ok: the receiver accepted RTCM1230 at the last apply (required in the observed set only then) */
    bool config_failed;              /**< STATION_ALERT_CONFIG_FAILED is raised */
    uint32_t config_retry_in_s;      /**< until the station's next apply retry (0 = none scheduled) */
    bool reconfigure_refused;        /**< STATION_RECOVER_RECONFIGURE would be refused now (gnss/autocfg = 0 or config_failed) */
    bool position_locked;            /**< profile uses a locked fixed position */
    um98x_base_position_t position;  /**< locked or last surveyed position */
    bool surveying;                  /**< inside the survey window */
    uint32_t survey_window_left_s;   /**< until the survey window ends (0 when not surveying) */
    bool lease_held;                 /**< a COMMAND or RAW lease is held */
    uint32_t since_lease_release_ms; /**< since the last lease release (UINT32_MAX = none this boot) */
    bool flash_journal_open;         /**< open, or present but unreadable; valid in safe mode too */
    bool flash_journal_readable;     /**< flash_journal holds the record (false: damaged or a newer schema: waits for the user) */
    bool flash_journal_settings_unavailable; /**< settings unavailable this boot: journal state unknown (nothing sent, nothing to clear) */
    um98x_flash_journal_t flash_journal;
    uint32_t detect_failures;        /**< consecutive failed detections */
    bool detect_saw_bytes;           /**< the last failed detection pass received bytes at some baud (health's byte rule on entering NO_RECEIVER) */
    uint64_t rtcm_excluded;          /**< frames of types 1101-1107 kept out of the hub */
    um98x_flash_progress_t flash;    /**< valid while FLASHING / FLASH_RECOVERY */
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

/** Status snapshot. Works before station_init() (safe mode): state IDLE, journal read from settings. */
esp_err_t station_get_status(station_status_t *out);

/** Re-apply the stored base profile (after the user edits it). Async. */
esp_err_t station_request_reconfigure(void);

/**
 * Flash the package staged in pkg_store to the receiver, only after an
 * explicit user confirmation (the request carries what was confirmed; the
 * banner's Update and the manual upload's confirm both come here). Async;
 * progress via station_get_status() and STATION_EVENT. The staged package is
 * validated on its RAM copy first; a failed validation releases it.
 * @return ESP_ERR_INVALID_STATE while a journal is open, during an app trial
 *         or crash watch, while an app update runs (update slot held by the
 *         app), unless the station is STREAMING (valid RTCM or NMEA within
 *         the last 10 s), uninitialised (safe mode) or for an unsupported
 *         model; ESP_ERR_NOT_FOUND if nothing is staged.
 */
esp_err_t station_request_flash(const station_flash_request_t *req);

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
 * journal, but not while a session is running, and not when the settings are
 * unavailable (nothing can be cleared then).
 * @return ESP_ERR_NOT_FOUND without an open journal (or loader alert),
 *         ESP_ERR_INVALID_STATE when the action is not allowed now.
 */
esp_err_t station_flash_user_action(station_flash_action_t action);

/** Copy the last receiver text of the flash session (up to 4 KB). @return bytes copied. */
size_t station_get_flash_log(char *buf, size_t len);

/**
 * Run one recovery action. Async; ignored if a stronger one is in progress.
 * @return ESP_ERR_INVALID_STATE when refused without sending anything:
 *         uninitialised, or RECONFIGURE while gnss/autocfg = 0 or
 *         STATION_ALERT_CONFIG_FAILED is raised (health counts a refused
 *         step as failed at once).
 */
esp_err_t station_request_recovery(station_recovery_t action);

/** Lock the current base position (persist it, switch the profile to FIXED, reconfigure). Async. */
esp_err_t station_lock_position(void);

/** Forget the locked position and survey again (reconfigure). Async. */
esp_err_t station_request_resurvey(void);

#ifdef __cplusplus
}
#endif
