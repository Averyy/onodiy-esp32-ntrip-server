/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Health monitor: watches the whole station and escalates recovery.
 *
 * Inputs (polled once per second): GNSS link counters, framer/hub counters,
 * observation summary, caster states and liveness stamps, network state,
 * heap (free, minimum, largest free internal block), the BOOT button.
 * Every second it also drives ledger_tick() (availability accounting).
 * Stable-boot marking and the safe-mode retry are not health's job: lifecycle
 * runs them in its own service task, so they keep working if this monitor
 * is not running. Health only reads the lifecycle state (safe-mode alert).
 *
 * GNSS ladder (each rung only if the previous one did not bring RTCM back
 * within its grace period; suspended while the station is CONFIGURING,
 * FLASHING or in FLASH_RECOVERY):
 *   1. resync framer / flush input                 (station_request_recovery)
 *   2. restart the UART link at the stored baud
 *   3. UART break + full baud rescan
 *   4. re-apply the receiver profile
 *   5. reset the receiver
 *   6. reboot the ESP32 - only when no bytes arrive at all (the ESP32 side
 *      may be wedged). An ESP32 reboot does not reset the receiver, so when
 *      bytes arrive but the receiver does not answer, this rung is skipped
 *      and the "receiver needs a power cycle" alert is raised instead.
 *
 * Caster supervision: slot tasks are not on the task watchdog. Health
 * restarts only the slot that overruns its per-state budget
 * (ntrip_server_restart_slot).
 *
 * Network ladder (while the protected caster is not streaming but RTCM is
 * healthy): +60 s reconnect, +3 min renew DHCP + fallback DNS, +6 min
 * restart Wi-Fi, +10 min reboot (rate-limited, inhibit-aware). A DNS/TCP
 * probe tells "no IP" from "IP but no upstream".
 *
 * All reboots go through lifecycle_reboot()/lifecycle_request_reboot(), so
 * they honour reboot inhibits (receiver flash, app update) and close caster
 * sessions cleanly.
 *
 * BOOT button (GPIO0, read after boot): held >= CONFIG_HEALTH_BUTTON_ADMIN_S
 * clears the admin password and starts the recovery setup AP; held
 * >= CONFIG_HEALTH_BUTTON_FACTORY_S runs a factory reset.
 *
 * Status: ladder structure is ours. Stall thresholds, grace periods and which
 * counters indicate loss are PROVISIONAL until confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEALTH_OK = 0,
    HEALTH_DEGRADED,       /**< something is off (one caster down, fewer than 4 bands, ...) */
    HEALTH_RECOVERING,     /**< a ladder step is in progress */
    HEALTH_FAILED,         /**< ladder exhausted short of reboot, or safe mode */
} health_level_t;

typedef enum {
    HEALTH_STEP_NONE = 0,
    HEALTH_STEP_RESYNC,
    HEALTH_STEP_LINK_RESTART,
    HEALTH_STEP_RESCAN,
    HEALTH_STEP_RECONFIGURE,
    HEALTH_STEP_RECEIVER_RESET,
    HEALTH_STEP_REBOOT,
} health_step_t;

/** Persistent alerts shown in the UI (bit flags). */
typedef enum {
    HEALTH_ALERT_POWER_SUPPLY      = 1u << 0, /**< brownout resets since power-on */
    HEALTH_ALERT_RECEIVER_POWER    = 1u << 1, /**< receiver silent to commands: needs a power cycle */
    HEALTH_ALERT_BANDS             = 1u << 2, /**< fewer than 4 bands on the best constellation */
    HEALTH_ALERT_PROFILE_DRIFT     = 1u << 3, /**< observed RTCM set/rates differ from the profile */
    HEALTH_ALERT_HEAP_FRAGMENTED   = 1u << 4, /**< largest free internal block below threshold */
    HEALTH_ALERT_SAFE_MODE         = 1u << 5,
    HEALTH_ALERT_SETTINGS_DEGRADED = 1u << 6, /**< NVS unusable this boot: nothing is saved */
    HEALTH_ALERT_NO_UPSTREAM       = 1u << 7, /**< Wi-Fi connected but no internet */
} health_alert_t;

/** Snapshot for status pages. */
typedef struct {
    health_level_t level;
    uint32_t alerts;                 /**< health_alert_t flags */
    health_step_t last_step;
    bool gnss_ladder_suspended;      /**< station is configuring/flashing */
    uint32_t uptime_s;
    uint32_t gnss_stalls;            /**< stall episodes since boot */
    uint32_t recoveries;             /**< ladder steps taken since boot */
    uint32_t caster_restarts;        /**< slot restarts after budget overruns */
    uint32_t network_recoveries;
    uint32_t rtcm_gap_ms;            /**< current time since the last RTCM frame */
    uint32_t heap_internal_free;
    uint32_t heap_internal_min;      /**< low-water mark since boot */
    uint32_t heap_internal_largest;  /**< largest free internal block (fragmentation) */
    uint32_t heap_psram_free;
} health_status_t;

/** Register handlers; read the button pin from the board. */
esp_err_t health_init(void);

/** Start the monitor task (1 Hz). */
esp_err_t health_start(void);

/** Status snapshot. */
esp_err_t health_get_status(health_status_t *out);

#ifdef __cplusplus
}
#endif
