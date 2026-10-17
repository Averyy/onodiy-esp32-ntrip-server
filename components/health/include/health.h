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
 * GNSS ladder (entry step by class: NO_BYTES 1, GARBAGE 3, TEXT_ONLY 4,
 * classed from what the receiver sends unasked: its replies to the
 * station's own commands are no part of it; each rung only if the previous
 * one did not bring RTCM back within its grace; the grace counts from the
 * end of the step):
 *   1. resync framer / flush input (station_request_recovery)     grace 5 s
 *   2. restart the UART link at the stored baud (driver kept)       15 s
 *   3. UART break + full baud rescan                                30 s from the end of the scan
 *   4. re-apply the receiver profile                                90 s, longer while the survey
 *                                                                   window is open (not for a
 *                                                                   receiver that answers and sent
 *                                                                   no RTCM this boot); during the
 *                                                                   window missing 1005/1033 never
 *                                                                   enters this step
 *   5. reset the receiver (command; no reset line on this board)    120 s
 *   After a failed step 3 (no receiver found) or step 5, the byte rule
 *   decides: no byte at all in that step's window -> 6a; any bytes -> 6b.
 *   6a. reboot the ESP32 (the ESP32 side may be wedged), only within the
 *       reboot cap (lifecycle_ladder_reboot_allowed: 60 min apart, at most
 *       3 per 24 h, across reboots), while no reboot inhibit is held and
 *       nothing holds it back (below); blocked or held -> 7.
 *   6b. an ESP32 reboot does not reset the receiver: raise "receiver needs a
 *       power cycle" instead -> 7.
 *   7. FAILED: every 10 min re-classify and enter at max(2, class entry
 *      step); 6a stays subject to the cap. Never a reboot loop.
 * The receiver answers but sends no RTCM (replies to the station's
 * commands, its restart banner, text lines): after step 5 (failed,
 * abandoned twice, or right after a step 5 that brought RTCM back only
 * briefly) that is neither 6b ("needs a power cycle") nor 6a but FAILED
 * with the reason; step 5 is skipped
 * when the receiver's own position sentence says it uses no satellites;
 * and the retry pass then runs no step 4 or 5 again (a silent receiver gets
 * steps 2 and 3, whose rescan asks it again). While the first
 * configuration is held (the hold builds), a receiver that answers is not
 * stalled at all: it was never set to send RTCM.
 * Every FAILED level carries its reason as an alert (and an alerts-only
 * stall's DEGRADED level too; a RECOVERING level, which also covers the
 * gap between two steps and the station's own sessions meanwhile, carries
 * NO_RTCM or NO_SATELLITES as soon as the receiver answered after its last
 * RTCM frame within the last 90 s, so the antenna hint comes with the
 * recovery; the station asks a receiver that sends no RTCM once a minute):
 * RECEIVER_POWER (6b), RECEIVER_SILENT
 * (nothing usable, no answer; power-cycle the station), NO_SATELLITES (it
 * answers and uses no satellites: the antenna), NO_RTCM (it answers but
 * sends no RTCM) and RX_HELD (the held first configuration: DEGRADED,
 * waiting for the user). In NO_RECEIVER the station's own alert says it.
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
 * Suspension (the ladder neither starts nor advances, its timers stop; a
 * step the station ran waits through the station's own sessions with the
 * grace it had left, any other suspension abandons a step in progress and
 * it runs again later): station DETECTING,
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
 * and the leases of those passes suspend nothing. A 6a that the reboot cap,
 * a reboot inhibit or the "no ESP32 reboot" mode kept from running is
 * looked at again while the episode lasts: on a tick in full mode where the
 * cap allows a reboot, no inhibit is held, nothing holds the restart back
 * and the station has completed a detection pass since
 * (station_status_t.detect_passes), the byte rule runs on that latest pass
 * (6a or 6b). The cap still bounds every reboot. When a suspension ends the
 * stall timer and the loss window start again from there.
 *
 * Restart hold (6a is never queued: held, journalled once as code 9 with
 * outcome "held" and the reasons, and looked at again later like a cap
 * block; a held 6a outside NO_RECEIVER comes back with the FAILED retry
 * pass), while any of these holds:
 *   setup      the settings are available, first setup is not finished
 *              (sys/setup_done != 1) and no caster slot is enabled; no time
 *              limit: such a station streams nothing. The wizard enables the
 *              Onocoy slot before setup is marked done, and an owner may
 *              never finish the optional last step: once a caster is
 *              enabled the station is in service and its restart is not
 *              held for the setup (a person on the page holds it as admin
 *              or ap_client, within the presence limit);
 *   ap_client  someone is connected to the station's access point;
 *   admin      an admin session made a request or sent a WebSocket ping
 *              within CONFIG_HEALTH_REBOOT_HOLD_ADMIN_S
 *              (health_note_admin_activity);
 *   power_on   less than CONFIG_HEALTH_REBOOT_POWER_ON_GRACE_MIN of station
 *              time since power-on (lifecycle_station_time_s) and nothing
 *              valid heard from the receiver in this boot (no RTCM frame, no
 *              checksum-valid text line): a restart cannot clear a fault the
 *              power-on itself just cleared. A receiver that streamed in
 *              this boot and then stalled still gets 6a at once, within the
 *              cap.
 * ap_client and admin together hold for at most
 * CONFIG_HEALTH_REBOOT_HOLD_MAX_MIN of presence per ladder episode (the
 * network ladder's limit for AP clients); after that the restart runs
 * anyway. During a ladder episode (a stall or NO_RECEIVER, until the
 * ladder resets) the status shows the reasons holding now
 * (gnss_reboot_hold), when the last of them ends at the latest and the
 * reboot cap's wait; outside one nothing is pending and it shows none of
 * them (an admin page open on a healthy station postpones nothing). Nor
 * while the receiver answers (the no_rtcm or no_satellites reason): the
 * ladder restarts the ESP32 only for a receiver that sent nothing in the
 * failed step's window (after the receiver reset: no answer to it), so no
 * restart is pending until the evidence says it stopped answering.
 *
 * User recovery mode (gnss/recovery, read every tick; a step already running
 * completes): full (default); no ESP32 reboot (6a is journalled as not run,
 * the ladder goes to 7); alerts only (nothing is requested from the station
 * and nothing rebooted: stalls are declared, classed and shown, each step the
 * full ladder would start is journalled once with outcome "not run" and its
 * grace still runs, and where the byte rule would run the ladder goes to 7,
 * with no power-cycle alert; level DEGRADED before 7, FAILED at 7; the
 * FAILED retry pass, every 10 min, journals the step it would enter only
 * when that step was not journalled in the episode yet). A step
 * the station ran before the switch to "alerts only" keeps its grace, only
 * watched like a journalled step's: no step in progress, level DEGRADED with
 * the reason, so the status never says the station still fixes it. Leaving
 * "alerts only" while stalled starts a fresh pass at the class entry step.
 * The network ladder, the station's detection schedule and its apply rules
 * are not affected.
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
 * Network ladder (network_ladder_tick; triggers: the protected caster is not
 * streaming but RTCM is healthy, or the station has been associated 60 s
 * without an IP, or an enabled slot of any caster has failed 180 s in a row
 * without reaching its caster; health passes the longest such run,
 * ntrip_caster_status_t.unreached_s): +60 s reconnect, +3 min renew DHCP +
 * fallback DNS, +6 min restart Wi-Fi, +10 min reboot (only for the
 * protected-caster trigger; rate-limited, inhibit-aware, withheld while a
 * receiver flash journal is open; not withheld when the journal's state is
 * only unknown because the settings are unavailable, since an ESP32-only
 * reset sends nothing to the receiver then either). A DNS/TCP probe tells
 * "no IP" from "IP but no upstream".
 *
 * Power lost: the journal's boot records are read (a bounded number per
 * tick); three or more power losses within 24 h raise
 * HEALTH_ALERT_POWER_LOST (DEGRADED), which clears 24 h after the last
 * one. A power loss is a power-on boot whose receiver restarted with it
 * (the station's verdict record, DIAG_EV_RX_BOOT): on the ESP32-S3 its
 * reset pin (the USB serial chip's auto-reset, the RST button) reads as
 * power-on while the receiver keeps running, and such a boot is no power
 * loss; nor is one whose verdict is unknown. A boot dated by no trusted
 * clock counts as inside the window. The running boot is dated from the
 * trusted clock (now minus the
 * uptime) once the reader has caught up with the journal, so the alert
 * clears even when the station writes no dated record for days; until it
 * has caught up the alert is not judged (no flash of it at start-up).
 * Brownouts keep their own alert (HEALTH_ALERT_POWER_SUPPLY).
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
 * clears the admin password and starts the recovery setup AP (counted in
 * admin_resets: with the settings unavailable the web holds a password set
 * this boot in RAM only, and forgets it when the count goes up); held
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
    HEALTH_FAILED,         /**< ladder at 6b (with HEALTH_ALERT_RECEIVER_POWER) or 7 (FAILED, retry pass pending, with its
                                reason's alert); nothing else */
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

/** Where the step in progress is (health_status_t.gnss_step_phase). */
typedef enum {
    HEALTH_STEP_PHASE_NONE = 0,      /**< no step in progress */
    HEALTH_STEP_PHASE_RUNNING,       /**< due, or the station runs it */
    HEALTH_STEP_PHASE_WAITING,       /**< the station ran it and waits for RTCM (its grace) */
} health_step_phase_t;

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
    HEALTH_ALERT_POWER_LOST        = 1u << 9, /**< power was lost: >= 3 power-on boots within 24 h whose receiver restarted
                                                   with the ESP32 (clears 24 h after the last) */
    HEALTH_ALERT_RECEIVER_SILENT   = 1u << 10,/**< nothing usable from the receiver and no answer to commands (FAILED, or an
                                                   alerts-only stall): power-cycle the station */
    HEALTH_ALERT_NO_SATELLITES     = 1u << 11,/**< the receiver answers, sends no RTCM and uses no satellites: check the antenna */
    HEALTH_ALERT_NO_RTCM           = 1u << 12,/**< the receiver answers but sends no RTCM */
    HEALTH_ALERT_RX_HELD           = 1u << 13,/**< no RTCM because the first configuration waits for the user (DEGRADED) */
} health_alert_t;

/** Automatic receiver recovery chosen by the user (settings gnss/recovery). */
typedef enum {
    HEALTH_RECOVERY_FULL = 0,        /**< the whole GNSS ladder */
    HEALTH_RECOVERY_NO_REBOOT = 1,   /**< everything but the ESP32 reboot */
    HEALTH_RECOVERY_ALERTS = 2,      /**< nothing sent to the receiver, nothing rebooted: stalls are only reported */
} health_recovery_mode_t;

/** Why the GNSS ladder's ESP32 restart is postponed now (bit flags; journal detail of a held restart). */
typedef enum {
    HEALTH_HOLD_SETUP     = 1u << 0, /**< first setup not finished */
    HEALTH_HOLD_AP_CLIENT = 1u << 1, /**< a client on the station's access point */
    HEALTH_HOLD_ADMIN     = 1u << 2, /**< the web page in use (admin request or WebSocket ping) */
    HEALTH_HOLD_POWER_ON  = 1u << 3, /**< first hour after power-on, nothing heard from the receiver */
} health_reboot_hold_t;

/** Snapshot for status pages. */
/** Which limit of the ladder's reboot cap holds a restart back (lifecycle_cap_t). */
typedef enum {
    HEALTH_CAP_NONE = 0,
    HEALTH_CAP_HOUR,                 /**< at most one ladder restart an hour */
    HEALTH_CAP_DAY,                  /**< at most 3 in 24 hours */
} health_cap_limit_t;

typedef struct {
    health_level_t level;
    uint32_t alerts;                 /**< health_alert_t flags */
    health_step_t last_step;         /**< last ladder action started (1-5, or REBOOT for 6a) */
    health_step_t gnss_step;         /**< what the GNSS ladder does now: the step due, running or in its grace (REBOOT
                                          while 6a is requested); NONE outside a step, in FAILED and for a step only
                                          watched in alerts-only mode */
    health_step_phase_t gnss_step_phase; /**< where gnss_step is (NONE with it) */
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
    uint32_t gnss_retry_in_s;        /**< FAILED: until the next retry pass (0 = none scheduled, e.g. while the station's own detection schedule runs,
                                          or in alerts only, whose pass sends nothing) */
    uint32_t factory_reset_refused_age_s; /**< since a BOOT-button factory reset was refused because a receiver update was running (UINT32_MAX = not this boot) */
    uint32_t admin_resets;           /**< BOOT-button admin password resets this boot; the web also forgets a password it holds only in RAM (settings unavailable) and ends every session */
    health_recovery_mode_t recovery_mode; /**< in effect (gnss/recovery; a stored value out of range reads as FULL) */
    bool gnss_reboot_recheck;        /**< NO_RECEIVER: a blocked ESP32 reboot waits for the cap and the next detection pass */
    uint8_t gnss_reboot_hold;        /**< health_reboot_hold_t reasons that postpone the ladder's ESP32 restart now (0 = none; always 0 outside a ladder episode and while the receiver answers) */
    uint32_t gnss_reboot_hold_left_s;/**< until every reason holding now has ended at the latest (the presence limit of ap_client and admin, the end of the power-on grace); UINT32_MAX = no hold, or setup holds (no time limit) */
    uint32_t gnss_reboot_cap_wait_s; /**< until the reboot cap allows a ladder restart; UINT32_MAX = allowed now, outside a ladder episode, or while the receiver answers */
    health_cap_limit_t gnss_reboot_cap_limit; /**< the cap's limit that sets gnss_reboot_cap_wait_s (the later one when both hold); NONE when that is UINT32_MAX */
} health_status_t;

/** Register handlers; read the button pin from the board. */
esp_err_t health_init(void);

/** Start the monitor task (1 Hz). */
esp_err_t health_start(void);

/** Status snapshot. */
esp_err_t health_get_status(health_status_t *out);

/**
 * An admin session is in use: the web server calls this for every request
 * with a valid admin session and for each admin WebSocket ping. Holds the
 * ladder's ESP32 restart back for CONFIG_HEALTH_REBOOT_HOLD_ADMIN_S. Never
 * blocks (one atomic store); any task.
 */
void health_note_admin_activity(void);

#ifdef __cplusplus
}
#endif
