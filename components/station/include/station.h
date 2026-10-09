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
 * published immediately; DETECTING/CONFIGURING take a COMMAND lease only when
 * no valid RTCM arrives, or the version or applied-profile check fails.
 *
 * Receiver flashing is crash-safe:
 *  - before ENTERING the station writes a flash journal to settings
 *    (gnss/flash_job: state, package SHA-256, attempt, start time), pins the
 *    staged package (pkg_store_set_pinned) and takes a reboot inhibit;
 *  - the journal and pin are cleared only after the receiver reports the
 *    package's version;
 *  - at boot an open journal sends the station straight to FLASH_RECOVERY:
 *    no baud scan against a receiver sitting in its loader, health's GNSS
 *    ladder off, no downtime-budget gate, using the persisted model
 *    (gnss/model) when the receiver does not answer. A journal that cannot be
 *    read (degraded settings, or a record a newer app wrote) counts as
 *    possibly open: recovery then starts by checking whether the receiver
 *    already streams normally;
 *  - not in safe mode: the station does not run there at all, so an open
 *    journal waits for the next normal boot (the time-boxed safe-mode retry,
 *    or clearing safe mode from the UI). The crash that led to safe mode may
 *    be in the flash path itself; retrying it in safe mode would take the
 *    web UI down with it.
 * Every successful detection persists gnss/model and gnss/fw.
 *
 * While CONFIGURING or FLASHING it pauses the casters (short command
 * sessions keep them connected but idle) and tells the ledger the downtime
 * is planned.
 *
 * Dependencies point down only: the station never calls health or
 * fw_update. The downtime budget it gates automatic flashing with comes from
 * the ledger (L1).
 *
 * Safe mode skips station_init() and station_start(). station_get_status()
 * works uninitialised (state IDLE, nothing known); the request functions
 * return an error.
 *
 * Status: state machine and request API are ours. The detection/config and
 * flash details it drives are PROVISIONAL.
 */
#pragma once

#include <stdbool.h>
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
} station_event_t;

typedef enum {
    STATION_STATE_IDLE = 0,      /**< not started (or safe mode) */
    STATION_STATE_DETECTING,
    STATION_STATE_CONFIGURING,
    STATION_STATE_STREAMING,
    STATION_STATE_FLASHING,
    STATION_STATE_FLASH_RECOVERY,/**< an interrupted receiver flash is being finished */
    STATION_STATE_RECOVERING,
    STATION_STATE_NO_RECEIVER,   /**< detection keeps failing; retried slowly */
} station_state_t;

/** Recovery actions the health ladder may request (mildest first). */
typedef enum {
    STATION_RECOVER_RESYNC = 0,      /**< reset framer, flush input */
    STATION_RECOVER_LINK_RESTART,    /**< reinstall UART at the stored baud */
    STATION_RECOVER_RESCAN,          /**< UART break, then full baud rescan */
    STATION_RECOVER_RECONFIGURE,     /**< re-apply the base profile */
    STATION_RECOVER_RECEIVER_RESET,  /**< reset the receiver (command, or reset GPIO if wired) */
} station_recovery_t;

/** Why a receiver flash is requested. */
typedef enum {
    STATION_FLASH_MANUAL = 0,        /**< user confirmed in the UI: no budget gate */
    STATION_FLASH_AUTO,              /**< update policy: gated by time of day and remaining budget */
    STATION_FLASH_RECOVERY,          /**< receiver down / interrupted flash: no gate */
} station_flash_reason_t;

/** Status snapshot. */
typedef struct {
    station_state_t state;
    um98x_info_t receiver;           /**< live, or last persisted when unresponsive */
    bool receiver_responding;
    bool profile_applied;            /**< receiver runs the stored profile */
    bool position_locked;            /**< profile uses a locked fixed position */
    um98x_base_position_t position;  /**< locked or last surveyed position */
    bool flash_journal_open;         /**< open, or present but unreadable */
    uint32_t detect_failures;        /**< consecutive failed detections */
    um98x_flash_progress_t flash;    /**< valid while FLASHING / FLASH_RECOVERY */
} station_status_t;

/** Wire GNSS link sinks (RTCM -> hub + observation summary, lines -> command layer), load profile and journal. */
esp_err_t station_init(void);

/** Start the supervisor task (journal recovery, or stream-first detect -> configure -> stream). */
esp_err_t station_start(void);

/** Status snapshot. Works before station_init() (safe mode): state IDLE, nothing known. */
esp_err_t station_get_status(station_status_t *out);

/** Re-apply the stored base profile (after the user edits it). Async. */
esp_err_t station_request_reconfigure(void);

/**
 * Flash the package staged in pkg_store to the receiver. Async; progress via
 * station_get_status() and STATION_EVENT.
 *
 * STATION_FLASH_AUTO is refused unless the expected downtime fits the
 * protected slot's remaining daily budget twice over, the UTC hour is at or
 * after CONFIG_STATION_AUTO_FLASH_UTC_HOUR, the clock is trusted (GNSS or
 * SNTP) and RTCM has been healthy for an hour. Manual and recovery flashes
 * are not gated. All reasons are refused if the package does not match the
 * receiver model.
 */
esp_err_t station_request_flash(station_flash_reason_t reason);

/** Expected receiver downtime for flashing the staged package, seconds (for the budget gate and the UI). */
uint32_t station_expected_flash_downtime_s(void);

/** Run one recovery action. Async; ignored if a stronger one is in progress. */
esp_err_t station_request_recovery(station_recovery_t action);

/** Lock the current base position (persist it, switch the profile to FIXED, reconfigure). Async. */
esp_err_t station_lock_position(void);

/** Forget the locked position and survey again (reconfigure). Async. */
esp_err_t station_request_resurvey(void);

#ifdef __cplusplus
}
#endif
