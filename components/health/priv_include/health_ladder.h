/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS recovery ladder (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, I/O behind callbacks; host-tested). health.c runs it
 * once a second from the health task: it samples the link counters and the
 * station status into hl_in_t, and the ladder calls back to request a
 * station recovery step, request the ESP32 reboot and journal its steps.
 *
 * Healthy = a CRC-valid RTCM frame within the stall threshold (published or
 * held by a lease: the receiver's output is what counts). A stall is
 * declared when there was none for longer than the threshold, measured from
 * the later of the last frame and the end of the last suspension. The stall
 * is classed from the last threshold's worth of input when it is declared
 * and again before each step:
 *   NO_BYTES   nothing at all                          -> entry step 1
 *   GARBAGE    bytes, no valid frame or text line      -> entry step 3
 *   TEXT_ONLY  checksum-valid text lines, no RTCM      -> entry step 4
 * Later steps never go back: the next step is max(last step + 1, class
 * entry).
 *
 *   step  action (station request)        grace from the end of the step
 *   1     RESYNC                           5 s
 *   2     LINK_RESTART                     15 s
 *   3     RESCAN                           30 s; found -> 4, not found -> byte rule
 *   4     RECONFIGURE                      90 s, longer while the survey window is open
 *   5     RECEIVER_RESET                   120 s, then the byte rule
 *   6a    ESP32 reboot (cap, no reboot inhibit, no hold), else blocked or
 *         held -> 7
 *   6b    "receiver needs a power cycle" alert -> 7
 *   7     FAILED: every 10 min, while still stalled, enter again at
 *         max(2, class entry); a TEXT_ONLY stall is not re-entered while the
 *         station refuses RECONFIGURE.
 * Byte rule: no byte at all in the failed step's window (step 3: the scan
 * and its grace, also the station's own byte report for the pass; step 5:
 * its grace) -> 6a; any bytes -> 6b.
 * A refused step 4 (the station sends nothing while gnss/autocfg = 0 or
 * after a failed apply) fails at once and the ladder goes on to step 5.
 * Success = healthy at any time during a step's grace. The ladder resets
 * (step index 0, power-cycle alert cleared) after 60 s of continuous health;
 * a shorter healthy spell keeps the index, so a flapping link escalates.
 *
 * Suspension (no step starts or advances, the ladder's timers stop, a step
 * in progress is abandoned): the station DETECTING, CONFIGURING, FLASHING,
 * FLASH_RECOVERY or IDLE (not running yet); a receiver flash journal open in
 * any state or of unknown state; safe mode; a lease, or a station recovery
 * job, that is not the ladder's own step; and the post-lease grace after any
 * release that was not the ladder's own. When a suspension ends the stall
 * timer restarts from there. An abandoned step runs again, once per
 * episode: a step that ran and is abandoned a second time before the ladder
 * resets counts as failed, and the ladder goes on past it (after step 5,
 * the byte rule on the stall class).
 * Station NO_RECEIVER: the station's slow detection schedule replaces the
 * ladder's retries. On entering it the byte rule runs once per episode on
 * the station's report for its whole detection pass, then the ladder holds
 * FAILED without retries until the station leaves NO_RECEIVER. An episode
 * that began with the ladder's own failed rescan is handled by that step.
 * A 6a the episode's byte rule could not take (the reboot cap, a reboot
 * inhibit, a hold, or the "no ESP32 reboot" mode) is looked at again: on a
 * tick where the mode is full, the cap allows a reboot, no inhibit and no
 * hold applies and the station has completed a detection pass since the
 * block, the byte rule runs once more on that latest pass (no bytes -> 6a,
 * bytes -> 6b). The cap still bounds every reboot; a reboot is never queued
 * behind an inhibit or a hold.
 *
 * Hold (hl_in_t.reboot_hold, computed by health: health_mon.h
 * hm_reboot_hold): reasons to postpone 6a right now, while a person sets the
 * station up or uses it, or in the first hour after power-on when nothing
 * was heard from the receiver. The byte rule treats a hold like a cap block
 * (the cap and an inhibit are journalled first, as before): journal code 9
 * with outcome HELD and the hold bits as detail (record b bits 16-23), go to
 * 7, and in NO_RECEIVER look again after the next detection pass once the
 * hold is gone. A held 6a in a stall that is not NO_RECEIVER comes back
 * through the FAILED retry pass (every 10 min). The ALERTS and NO_REBOOT
 * modes never reach the hold.
 *
 * User recovery mode (gnss/recovery, read before every decision; a step
 * already running completes):
 *   FULL     the ladder above;
 *   NO_REBOOT  6a is never taken: where the byte rule would choose it, the
 *            ladder journals code 9 with outcome NOT_RUN and goes to 7;
 *            steps 1-5 and 6b unchanged;
 *   ALERTS   nothing is requested and nothing rebooted. Stalls are still
 *            declared and classed; each step the full ladder would start is
 *            journalled once (outcome NOT_RUN) when it would start and its
 *            grace still runs; where the byte rule would run (after step 3
 *            or 5) the ladder goes to 7, with no 6b and no power-cycle
 *            alert. In 7 the retry pass journals the step it would enter
 *            every 10 min and sends nothing. The level is DEGRADED before 7
 *            and FAILED at 7. Leaving this mode while stalled starts a fresh
 *            pass at the class entry step.
 *
 * Timings are PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_GRACE_RESYNC_MS        5000
#define HL_GRACE_LINK_RESTART_MS  15000
#define HL_GRACE_RESCAN_MS        30000
#define HL_GRACE_RECONFIGURE_MS   90000
#define HL_GRACE_RECEIVER_RESET_MS 120000
#define HL_HEALTHY_RESET_MS       60000      /**< continuous health that resets the ladder */
#define HL_FAILED_RETRY_MS        600000     /**< FAILED: retry pass interval */
#define HL_POST_LEASE_MS          10000      /**< suspension after a lease release that is not the ladder's */
#define HL_PICKUP_TIMEOUT_MS      60000      /**< accepted step never seen running: assume it ran unseen */
#define HL_RUN_CAP_MS             600000     /**< a step seen running this long counts as ended */
#define HL_REBOOT_WAIT_MS         60000      /**< reboot requested and still alive: count as FAILED */
#define HL_MAX_TICK_MS            5000       /**< longest tick gap credited to the ladder clock */

#define HL_NEVER INT64_MIN

/** Same values as station_state_t. */
typedef enum {
    HL_STN_IDLE = 0,
    HL_STN_DETECTING,
    HL_STN_CONFIGURING,
    HL_STN_STREAMING,
    HL_STN_FLASHING,
    HL_STN_FLASH_RECOVERY,
    HL_STN_RECOVERING,
    HL_STN_NO_RECEIVER,
} hl_stn_state_t;

/** Stall classes. */
typedef enum {
    HL_CLASS_NO_BYTES = 0,
    HL_CLASS_GARBAGE,
    HL_CLASS_TEXT_ONLY,
} hl_class_t;

/**
 * Journal step codes (record b = (outcome << 8) | code). 1-5 are the
 * recovery steps, the rest name the rungs that start no station action.
 */
typedef enum {
    HL_CODE_NONE = 0,
    HL_CODE_RESYNC = 1,
    HL_CODE_LINK_RESTART = 2,
    HL_CODE_RESCAN = 3,
    HL_CODE_RECONFIGURE = 4,
    HL_CODE_RECEIVER_RESET = 5,
    HL_CODE_REBOOT = 6,           /**< 6a: ESP32 reboot requested */
    HL_CODE_POWER_ALERT = 7,      /**< 6b: "receiver needs a power cycle" */
    HL_CODE_FAILED = 8,           /**< 7: FAILED, retry pass pending */
    HL_CODE_REBOOT_BLOCKED = 9,   /**< 6a blocked (reboot cap, or a reboot inhibit held) */
} hl_code_t;

/** Journal outcomes. */
typedef enum {
    HL_OUT_STARTED = 0,
    HL_OUT_HEALTHY = 1,
    HL_OUT_FAILED = 2,            /**< also: refused by the station without sending anything */
    HL_OUT_ABANDONED = 3,         /**< a suspension began */
    HL_OUT_NOT_RUN = 4,           /**< not run: the user's recovery mode */
    HL_OUT_HELD = 5,              /**< 6a held back for now (code 9; detail = the hold bits) */
} hl_outcome_t;

/** Why 6a is held back (hl_in_t.reboot_hold, journal detail of HL_OUT_HELD). */
#define HL_HOLD_SETUP     (1u << 0)  /**< first setup not finished */
#define HL_HOLD_AP_CLIENT (1u << 1)  /**< a client is connected to the station's access point */
#define HL_HOLD_ADMIN     (1u << 2)  /**< an admin used the web page in the last minutes */
#define HL_HOLD_POWER_ON  (1u << 3)  /**< first hour after power-on, nothing heard from the receiver this boot */

/** User recovery mode (gnss/recovery). */
typedef enum {
    HL_MODE_FULL = 0,             /**< the whole ladder */
    HL_MODE_NO_REBOOT = 1,        /**< everything but the ESP32 reboot (6a) */
    HL_MODE_ALERTS = 2,           /**< nothing sent, nothing rebooted: stalls are only declared and journalled */
} hl_mode_t;

/** Result of a step request. */
typedef enum {
    HL_REQ_ACCEPTED = 0,          /**< queued (or merged with an equal or stronger one) */
    HL_REQ_REFUSED,               /**< RECONFIGURE refused: nothing sent */
    HL_REQ_REJECTED,              /**< not a state the station acts in now: try again later */
} hl_req_t;

/** Ladder phases. */
typedef enum {
    HL_PH_IDLE = 0,               /**< no stall in progress (the step index may still be set) */
    HL_PH_WANT,                   /**< a step is due; its request was not accepted yet */
    HL_PH_RUN,                    /**< the station runs the step */
    HL_PH_GRACE,                  /**< step done: waiting for RTCM */
    HL_PH_REBOOTING,              /**< 6a requested */
    HL_PH_FAILED,                 /**< 7 (after 6b with the power-cycle alert) */
} hl_phase_t;

/** Severity the ladder adds to the overall level (same order as health_level_t). */
typedef enum {
    HL_LEVEL_OK = 0,
    HL_LEVEL_DEGRADED,
    HL_LEVEL_RECOVERING,
    HL_LEVEL_FAILED,
} hl_level_t;

/** Suspension reasons (bit flags). */
#define HL_SUSP_STATION    (1u << 0)  /**< detecting, configuring, flashing, flash recovery, not running */
#define HL_SUSP_JOURNAL    (1u << 1)  /**< receiver flash journal open, or its state unknown */
#define HL_SUSP_SAFE_MODE  (1u << 2)
#define HL_SUSP_LEASE      (1u << 3)  /**< a lease or a recovery job that is not the ladder's step */
#define HL_SUSP_POST_LEASE (1u << 4)  /**< the grace after another owner's lease release */

/** What the station must do (callbacks into health.c). */
typedef struct {
    /** Request recovery step 1-5 from the station (never blocks). */
    hl_req_t (*request)(void *ctx, uint8_t step);
    /** Request the ESP32 reboot (6a). @return false if it was blocked after all (cap or inhibit, a race). */
    bool (*reboot)(void *ctx);
    /** Journal one ladder record; `detail` = the hold bits of HL_OUT_HELD, else 0. */
    void (*journal)(void *ctx, uint8_t code, uint8_t outcome, uint8_t detail);
    void *ctx;
} hl_ops_t;

/** Inputs sampled once per tick. */
typedef struct {
    int64_t now_ms;
    uint32_t stall_ms;            /**< healthy = an RTCM frame within this */
    uint32_t rtcm_age_ms;         /**< since the last CRC-valid RTCM frame, UINT32_MAX = never */
    uint32_t rx_age_ms;           /**< since any byte, UINT32_MAX = never */
    uint64_t rx_bytes;            /**< link bytes received (monotonic) */
    uint64_t text_lines;          /**< checksum-valid text lines (monotonic) */
    uint8_t stn_state;            /**< hl_stn_state_t, sampled after the transition counters */
    uint32_t rec_enters;          /**< station transitions into RECOVERING (monotonic) */
    uint32_t rec_exits;           /**< station transitions out of RECOVERING (monotonic) */
    bool lease_held;
    uint32_t since_release_ms;    /**< since the last lease release, UINT32_MAX = none */
    bool journal_open;            /**< open in any state, unreadable, or unknown */
    bool journal_unknown;         /**< settings unavailable: state unknown (also suspends) */
    bool safe_mode;
    bool detect_saw_bytes;        /**< the station's last failed detection pass received bytes */
    uint32_t detect_passes;       /**< detection passes the station completed since boot (monotonic) */
    uint8_t recovery_mode;        /**< hl_mode_t (any other value acts as FULL) */
    bool reconfigure_refused;     /**< the station would refuse RECONFIGURE now */
    bool surveying;               /**< the station's survey window is open */
    bool survey_holds_rtcm;       /**< the receiver sends no RTCM while surveying: no stall inside the window */
    bool reboot_allowed;          /**< the ladder reboot cap allows a reboot now */
    bool reboot_inhibited;        /**< a reboot inhibit is held */
    uint8_t reboot_hold;          /**< HL_HOLD_* reasons that hold 6a back now (0 = none) */
} hl_in_t;

typedef struct {
    hl_ops_t ops;
    hl_phase_t phase;
    uint8_t idx;                  /**< last step of this episode (0 = none) */
    uint8_t step;                 /**< step wanted, running or in its grace */
    uint8_t last_step;            /**< last action started (1-5, 6 = reboot), 0 = none */
    uint8_t abandoned;            /**< bit n: step n ran and was abandoned in this episode */
    bool power_alert;

    int64_t last_now;
    int64_t active_ms;            /**< ladder clock: stops while suspended */
    int64_t stall_ref_ms;         /**< wall time the stall timer counts from */
    bool was_suspended;
    uint8_t susp;                 /**< reasons of the last tick */

    /* text-line liveness (sampled) */
    bool lines_valid;
    uint64_t lines_last;
    int64_t line_ms;              /**< wall time a new line was last seen (HL_NEVER = never) */

    /* healthy tracking */
    bool healthy;
    int64_t healthy_since;        /**< ladder clock */

    /* the running step */
    int64_t req_ms;               /**< wall time of the accepted request */
    uint32_t enters0, exits0;
    bool seen_running;
    int64_t seen_ms;              /**< wall time it was first seen running */
    int64_t grace_start;          /**< ladder clock */
    uint64_t win_bytes0;          /**< rx_bytes at the start of the byte-rule window */
    bool win_saw_bytes;           /**< the station reported bytes during the window */
    bool not_found;               /**< step 3 ended without finding the receiver */
    int64_t own_from_ms;          /**< lease releases in [own_from, own_to] were the ladder's own */
    int64_t own_to_ms;            /**< HL_NEVER while the step runs (window still open) */

    /* FAILED */
    int64_t retry_at;             /**< ladder clock */
    bool hold_no_rx;              /**< station NO_RECEIVER: its schedule retries, not the ladder */

    /* NO_RECEIVER episodes */
    bool no_rx;                   /**< the station is in a NO_RECEIVER episode */
    bool no_rx_handled;           /**< the byte rule already ran for it */
    bool blocked_6a;              /**< its byte rule chose 6a and could not take it: looked at again */
    uint32_t blocked_at_pass;     /**< detect_passes when it was blocked */

    /* user recovery mode */
    uint8_t mode;                 /**< hl_mode_t of the last decision */
    bool dry;                     /**< the step in its grace was only journalled (ALERTS mode) */

    /* REBOOTING */
    int64_t reboot_ms;

    /* counters */
    uint32_t stalls;
    uint32_t recoveries;
} hl_t;

/** Status snapshot. */
typedef struct {
    uint8_t level;                /**< hl_level_t */
    uint8_t phase;                /**< hl_phase_t */
    uint8_t rung;                 /**< hl_code_t of the current rung (0 = none) */
    uint8_t last_step;            /**< last action started: 1-5, 6 = reboot, 0 = none */
    bool suspended;
    uint8_t susp;                 /**< HL_SUSP_* */
    bool power_alert;
    uint32_t stalls;
    uint32_t recoveries;
    uint32_t retry_in_s;          /**< FAILED: until the next retry pass (0 = none scheduled) */
    uint8_t mode;                 /**< hl_mode_t the ladder last decided with */
    bool reboot_recheck;          /**< NO_RECEIVER: a blocked 6a waits to be looked at again */
    bool episode;                 /**< a stall or NO_RECEIVER episode is in progress (the ladder has not reset since) */
} hl_status_t;

/** Fresh ladder; the stall timer starts at now_ms. */
void hl_init(hl_t *l, const hl_ops_t *ops, int64_t now_ms);

/** One tick (about once a second). Calls back for requests, the reboot and journal records. */
void hl_tick(hl_t *l, const hl_in_t *in);

/** Status snapshot. `in` is the last tick's input (for the level). */
void hl_get_status(const hl_t *l, const hl_in_t *in, hl_status_t *out);

/** Stall class from the inputs (exposed for tests and logs). */
hl_class_t hl_classify(const hl_t *l, const hl_in_t *in);

/** Entry step of a class. */
uint8_t hl_entry_step(hl_class_t c);

#ifdef __cplusplus
}
#endif
