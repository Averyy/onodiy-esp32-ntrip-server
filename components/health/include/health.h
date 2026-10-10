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
 * GNSS path: healthy = a CRC-valid RTCM frame within CONFIG_HEALTH_GNSS_STALL_S.
 * A stall is classed when it is declared and again before each step, from
 * the link counters of the last 10 s: NO_BYTES (nothing received), GARBAGE
 * (bytes but no valid frame or line) or TEXT_ONLY (valid text lines, no
 * RTCM). Valid RTCM with the profile's messages missing is profile drift,
 * not a stall.
 *
 * GNSS ladder (entry step by class: NO_BYTES 1, GARBAGE 3, TEXT_ONLY 4; each
 * rung only if the previous one did not bring RTCM back within its grace;
 * the grace counts from the end of the step):
 *   1. resync framer / flush input (station_request_recovery)     grace 5 s
 *   2. restart the UART link at the stored baud                     15 s
 *   3. UART break + full baud rescan                                30 s from the end of the scan
 *   4. re-apply the receiver profile                                90 s; during the survey
 *                                                                   window missing 1005/1033
 *                                                                   never enters this step
 *   5. reset the receiver (command; no reset line on this board)    120 s
 *   After a failed step 3 (no receiver found) or step 5, the byte rule
 *   decides: no byte at all in that step's window -> 6a; any bytes -> 6b.
 *   6a. reboot the ESP32 (the ESP32 side may be wedged), only within the
 *       reboot cap (lifecycle_ladder_reboot_allowed: 60 min apart, at most
 *       3 per 24 h, across reboots) and while no reboot inhibit is held;
 *       blocked -> 7.
 *   6b. an ESP32 reboot does not reset the receiver: raise "receiver needs a
 *       power cycle" instead -> 7.
 *   7. FAILED: every 10 min re-classify and enter at max(2, class entry
 *      step); 6a stays subject to the cap. Never a reboot loop.
 * Step 4 needs the station's consent: it refuses RECONFIGURE without
 * sending anything while gnss/autocfg = 0 or after a failed apply
 * (station_status_t.reconfigure_refused). A refused step 4 counts as failed
 * at once (no grace) and the ladder goes on to step 5. In state 7 a
 * TEXT_ONLY stall is not re-entered while reconfigure_refused is set: the
 * station's own apply backoff (or the user) acts, so a configuration the
 * receiver keeps rejecting never resets it every 10 min.
 * The ladder resets after 60 s of healthy RTCM (that also clears the power
 * cycle alert); a shorter healthy period keeps the step, so a flapping link
 * escalates. Health posts the station request and returns: one tick never
 * blocks for more than 100 ms. A step ends when the station leaves
 * RECOVERING (seen through STATION_EVENT_STATE_CHANGED and the status).
 * Every step start and result is journalled (DIAG_EV_LADDER_STEP, a = 0).
 * With CONFIG_HEALTH_SURVEY_HOLDS_RTCM (a receiver that sends no RTCM while
 * it surveys) no stall is declared and no grace runs out while the station's
 * survey window is open.
 *
 * Suspension (the ladder neither starts nor advances, its timers stop, a
 * step in progress is abandoned and runs again later): station DETECTING,
 * CONFIGURING, FLASHING or FLASH_RECOVERY (and IDLE: not running yet); a
 * receiver flash journal open in any state, including waiting for the user,
 * and a journal whose state is unknown because the settings are
 * unavailable; safe mode; any GNSS lease held (or station recovery job run)
 * by someone other than the ladder; and for 10 s after the release of such a
 * lease, the stall timer then counting from the release. While the station
 * is NO_RECEIVER, health applies the byte rule once per episode, using the
 * station's classification of its whole detection pass
 * (station_status_t.detect_saw_bytes: none -> 6a, some -> 6b), and then
 * holds step 7: the station's own detection schedule (short list every 30 s
 * for 5 min, then the full list every 5 min) replaces the ladder's retries,
 * and the leases of those passes suspend nothing. When a suspension ends the
 * stall timer and the loss window start again from there.
 *
 * Link-loss alert: any UART FIFO overflow, or RTCM CRC errors above 1 % with
 * at least 100 frames, within a 10-11 min window of one-minute RAM buckets
 * (deltas accrued while suspended are not counted) raises
 * HEALTH_ALERT_LINK_LOSS; it clears after 30 min without either. Loss
 * counter deltas go to the journal (DIAG_EV_DATA_LOSS), at most one record
 * per tick; a record the journal's rate limit refused stays pending.
 *
 * Caster supervision: slot tasks are not on the task watchdog. Health
 * restarts only the slot that overruns its per-state budget
 * (ntrip_server_restart_slot).
 *
 * Network ladder (while the protected caster is not streaming but RTCM is
 * healthy): +60 s reconnect, +3 min renew DHCP + fallback DNS, +6 min
 * restart Wi-Fi, +10 min reboot (rate-limited, inhibit-aware, withheld while
 * a receiver flash journal is open; not withheld when the journal's state is
 * only unknown because the settings are unavailable, since an ESP32-only
 * reset sends nothing to the receiver then either). A DNS/TCP probe tells
 * "no IP" from "IP but no upstream".
 *
 * All reboots go through lifecycle_reboot()/lifecycle_request_reboot(), so
 * they honour reboot inhibits (receiver flash, app update) and close caster
 * sessions cleanly. While a receiver flash journal is open health requests
 * no ESP32 reboot of its own, in any journal state and in safe mode, with
 * one exception: the low-heap reboot (internal low-water below 8 KB). It
 * waits for the reboot inhibit, so it never lands in a flash session's full
 * lockout, and once the journal waits for the user an ESP32-only reset sends
 * nothing to the receiver, while a starved heap could take the web UI the
 * user needs down with it.
 *
 * BOOT button (GPIO0, read after boot): held >= CONFIG_HEALTH_BUTTON_ADMIN_S
 * clears the admin password and starts the recovery setup AP; held
 * >= CONFIG_HEALTH_BUTTON_FACTORY_S runs a factory reset. The action is
 * chosen by the hold time and taken on release (the log says when each
 * threshold is reached); a button held or stuck since boot does nothing
 * until it has been released once. The factory reset is refused (logged
 * and shown in the UI, factory_reset_refused_age_s) while a receiver flash
 * journal is open and not waiting for the user: it would erase the profile
 * that the session re-applies to the receiver.
 *
 * Alerts: health_alert_t below; the station raises and clears its own
 * (station_status_t.alerts). The UI shows both.
 *
 * Status: thresholds, grace periods and loss rules are PROVISIONAL, to be
 * confirmed on hardware.
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
    HEALTH_DEGRADED,       /**< something is off (one caster down, fewer than 4 bands, link losing data, safe mode, ...) */
    HEALTH_RECOVERING,     /**< stall declared: a ladder step runs or is in its grace */
    HEALTH_FAILED,         /**< ladder at 6b (with HEALTH_ALERT_RECEIVER_POWER) or 7 (FAILED, retry pass pending); nothing else */
} health_level_t;

/** Last ladder action started (6b and 7 start no action and leave it unchanged). */

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
    HEALTH_ALERT_SAFE_MODE         = 1u << 5, /**< safe mode (level DEGRADED; FAILED is only for ladder 6b and 7) */
    HEALTH_ALERT_SETTINGS_DEGRADED = 1u << 6, /**< NVS unusable this boot: nothing is saved */
    HEALTH_ALERT_NO_UPSTREAM       = 1u << 7, /**< Wi-Fi connected but no internet */
    HEALTH_ALERT_LINK_LOSS         = 1u << 8, /**< UART overflows or RTCM CRC errors in the last 10 min */
} health_alert_t;

/** Snapshot for status pages. */
typedef struct {
    health_level_t level;
    uint32_t alerts;                 /**< health_alert_t flags */
    health_step_t last_step;         /**< last ladder action started (1-5, or REBOOT for 6a) */
    bool gnss_ladder_suspended;      /**< see the suspension rules: detecting, configuring, flashing, open or unknown flash journal, safe mode, leases and 10 s after */
    uint64_t uptime_s;
    uint32_t gnss_stalls;            /**< stall declarations since boot */
    uint32_t recoveries;             /**< ladder actions 1-5 and 6a started since boot */
    uint32_t caster_restarts;        /**< slot restarts after budget overruns */
    uint32_t network_recoveries;
    uint32_t rtcm_gap_ms;            /**< current time since the last RTCM frame */
    uint32_t heap_internal_free;
    uint32_t heap_internal_min;      /**< low-water mark since boot */
    uint32_t heap_internal_largest;  /**< largest free internal block (fragmentation) */
    uint32_t heap_psram_free;
    uint32_t gnss_retry_in_s;        /**< FAILED: until the next retry pass (0 = none scheduled, e.g. while the station's own detection schedule runs) */
    uint32_t factory_reset_refused_age_s; /**< since a BOOT-button factory reset was refused because a receiver update was running (UINT32_MAX = not this boot) */
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
