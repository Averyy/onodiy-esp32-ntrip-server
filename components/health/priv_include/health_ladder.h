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
 *                                          (not for a receiver that answers and sent
 *                                          no RTCM frame this boot: 90 s)
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
 * the rule of its failure for a receiver that answered since the RESET
 * went out: 7 with its reason; otherwise the byte rule on the stall class).
 * Only right after that second abandonment: a re-run that succeeded, or
 * RTCM back meanwhile, makes a later stall an ordinary one (after step 5:
 * the byte rule on its class).
 * Between steps (IDLE after an abandoned step, or before a stall is
 * declared again, while no RTCM comes and nothing but a lease suspends the
 * ladder) the status stays RECOVERING (DEGRADED in ALERTS mode) with its
 * reason, so the antenna hint does not drop out while the station's own
 * sessions run.
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
 *            pass at the class entry step. A step the station runs when
 *            the user chooses this mode completes; once it has run, its
 *            grace is only watched like a journalled step's (level
 *            DEGRADED, the reason shown, no step in progress): nothing is
 *            sent after it, so the status never says the station is still
 *            fixing the stall.
 *
 * Step in progress (hl_status_t.doing, .waiting; what the UI says the
 * station does now): the step that is due or that the station runs
 * (waiting false), or the step whose grace runs after the station ran it
 * (waiting true); REBOOT while 6a is requested; none in IDLE, in 7, for a
 * step that was only journalled and for a grace in ALERTS mode.
 *
 * Step 4 and the survey window: its grace is held while the survey window
 * is open, so a reconfigure that restarted a running survey is not judged
 * (and followed by a receiver reset, which would restart it again) before
 * the survey had its time. Not for a receiver that answers and has sent no
 * RTCM frame at all this boot: this boot's own configuration opened the
 * window, nothing has streamed that a reset could interrupt, and a survey
 * makes no progress without satellites, so waiting out the window (up to
 * survey_in_s + 120 s) would only hold back the verdict. Its grace is the
 * plain 90 s. A receiver that holds RTCM back while surveying is
 * CONFIG_HEALTH_SURVEY_HOLDS_RTCM's case: no stall is declared inside the
 * window at all.
 *
 * Why the receiver path is down (hl_status_t.why, the reason health's
 * alerts carry, so a FAILED level is never shown without one):
 *   HELD      the first configuration is held (hl_in_t.rx_held) and the
 *             receiver answers (text lines or replies within the stall
 *             threshold): it was never set to send RTCM. No stall is
 *             declared for it (no step, no journal record); a FAILED ladder
 *             that meets it goes back to idle. The level stays DEGRADED.
 *   NO_ANSWER 6b: bytes but no answers (the power-cycle alert).
 *   NO_SKY    the receiver answers and its own position sentence says it
 *             uses no satellites (hl_in_t.no_sky): the antenna.
 *   NO_RTCM   the receiver answers but sends no RTCM: text lines or
 *             replies in this episode, text lines when it began, or replies
 *             to the station within HL_ALIVE_MS before it began (replies
 *             come only when asked, so their age is the evidence; a text
 *             output that stopped is silence). A receiver that sent no RTCM
 *             frame this boot answers only while its last line or reply is
 *             at most HL_ANSWER_FRESH_MS old: the station asks such a
 *             receiver once a minute while nothing comes, so one that
 *             answers when asked stays fresh, and one that hung after
 *             answering at boot does not answer for ever.
 *   SILENT    nothing usable and no answer.
 * While the ladder is in 7 or (ALERTS mode) in a journalled or only
 * watched step's grace, never in NO_RECEIVER (the station's own alert says
 * that). NO_RTCM and NO_SKY also while a step is in progress (above), as
 * soon as the receiver answered after its last RTCM frame, more than
 * HL_ANSWER_SLACK_MS after it (text lines or replies; with no RTCM frame
 * this boot, within HL_ANSWER_FRESH_MS): the antenna is the likely cause,
 * and the hint comes with the recovery, not after it. The level stays
 * RECOVERING then. Line and reply times are exact (hl_in_t.line_age_ms,
 * answer_age_ms), so this is the same evidence, by the same rule, as the
 * station's "the receiver answers" (receiver_responding).
 *
 * Restart pending (hl_status_t.restart_possible; what the status says of
 * the restart hold and the reboot cap): an episode without RTCM, unless the
 * receiver answers by this evidence: `why` is NO_RTCM, NO_SKY or HELD, or,
 * where no reason is given (a suspension such as the station's own session,
 * a stall not declared again yet), it answered after its last RTCM frame
 * (never in NO_RECEIVER or while 6a runs). 6a needs a step whose window saw
 * no byte at all, and a receiver that answers gets 7 with its reason after
 * step 5 instead, so a restart comes only if it stops answering; the status
 * says so once the evidence changes (a rescan that finds nothing, a RESET
 * with no answer, or, with no RTCM frame this boot, no answer for
 * HL_ANSWER_FRESH_MS).
 *
 * The receiver answers but sends no RTCM (amends the byte rule and the
 * retry pass; 6b is "bytes but no answers"):
 *  - after step 5, a receiver that answered (replies from the RESET's start:
 *    its banner, its version log; text lines in the step's grace) goes to 7
 *    with NO_RTCM (or NO_SKY), not to 6b: a power cycle is not the advice;
 *  - step 5 is not taken while the receiver answers and says it uses no
 *    satellites (no_sky): a reset cannot bring satellites; straight to 7;
 *  - in 7 after either, the retry pass does not run steps 4 and 5 again: a
 *    TEXT_ONLY stall holds 7, and a silent one goes up to step 3 only (its
 *    rescan asks the receiver again); found there, back to 7. A rescan that
 *    finds nothing ends that (the byte rule as usual).
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
#define HL_ALIVE_MS               600000     /**< replies this recent when a stall begins: the receiver is alive */
#define HL_ANSWER_SLACK_MS        1000       /**< a line this soon after the last RTCM frame may be that output's own: no answer after it */
#define HL_ANSWER_FRESH_MS        90000      /**< no RTCM frame this boot: a line or reply this recent is an answer (the station asks every 60 s) */

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

/** Why 6a is blocked (code 9 outcome STARTED: the journal detail; hl_in_t.reboot_cap_why for the cap). */
typedef enum {
    HL_CAP_UNKNOWN = 0,           /**< not recorded (journal records from before this was kept) */
    HL_CAP_HOUR = 1,              /**< the reboot cap: at most one an hour */
    HL_CAP_DAY = 2,               /**< the reboot cap: at most 3 in 24 hours */
    HL_CAP_LOCK = 3,              /**< a reboot inhibit is held (another job holds restarts back) */
} hl_cap_why_t;

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

/** Why the receiver path is down (hl_status_t.why; see above). */
typedef enum {
    HL_WHY_NONE = 0,
    HL_WHY_HELD,                  /**< the first configuration is held; the receiver answers */
    HL_WHY_SILENT,                /**< nothing usable from the receiver and no answer */
    HL_WHY_NO_ANSWER,             /**< 6b: bytes but no answers (power-cycle alert) */
    HL_WHY_NO_SKY,                /**< it answers and uses no satellites */
    HL_WHY_NO_RTCM,               /**< it answers but sends no RTCM */
} hl_why_t;

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
    /** Journal one ladder record; `detail` = the hold bits of HL_OUT_HELD, hl_cap_why_t for code 9
     *  outcome STARTED, else 0. */
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
    uint8_t reboot_cap_why;       /**< hl_cap_why_t: HL_CAP_HOUR or HL_CAP_DAY when the cap blocks (else ignored) */
    bool reboot_inhibited;        /**< a reboot inhibit is held */
    uint8_t reboot_hold;          /**< HL_HOLD_* reasons that hold 6a back now (0 = none) */
    uint64_t answers;             /**< the receiver's checksum-valid replies to the command layer (acks, logs, banners,
                                       restarting answers; monotonic) */
    uint32_t line_age_ms;         /**< since the last checksum-valid text line of any kind (replies included), exact;
                                       UINT32_MAX = none this boot */
    uint32_t answer_age_ms;       /**< since the last of `answers`, exact; UINT32_MAX = none this boot */
    bool no_sky;                  /**< the receiver's own position sentence within the stall threshold: no fix, no satellites */
    bool rx_held;                 /**< the first configuration is held: the receiver was never set to send RTCM */
} hl_in_t;

typedef struct {
    hl_ops_t ops;
    hl_phase_t phase;
    uint8_t idx;                  /**< last step of this episode (0 = none) */
    uint8_t step;                 /**< step wanted, running or in its grace */
    uint8_t last_step;            /**< last action started (1-5, 6 = reboot), 0 = none */
    uint8_t abandoned;            /**< bit n: step n ran and was abandoned in this episode */
    bool abandoned_twice;         /**< the last step was abandoned a second time and counts as failed (until a step
                                       starts, a step succeeds or RTCM comes back) */
    bool power_alert;

    int64_t last_now;
    int64_t active_ms;            /**< ladder clock: stops while suspended */
    int64_t stall_ref_ms;         /**< wall time the stall timer counts from */
    bool was_suspended;
    uint8_t susp;                 /**< reasons of the last tick */

    /* text-line liveness: new ones counted from the samples, their times from the ages (exact) */
    bool lines_valid;
    uint64_t lines_last;
    int64_t line_ms;              /**< wall time of the last line (HL_NEVER = none this boot) */
    uint64_t answers_last;
    int64_t answer_ms;            /**< wall time of the last reply (HL_NEVER = none this boot) */

    /* the receiver answers but sends no RTCM */
    bool ep_alive;                /**< it answers: in this episode, or (replies) within HL_ALIVE_MS before it began */
    bool no_rtcm;                 /**< 7 reached while it answers: the retry pass stops after step 3 */
    bool held_wait;               /**< stalled, and the held first configuration explains it: no stall declared */

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
    uint64_t win_answers0;        /**< answers at the start of the step */
    int64_t win_from_ms;          /**< text lines after this count as answers (step 5: its grace) */
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
    uint32_t retry_in_s;          /**< FAILED: until the next retry pass (0 = none scheduled; also in ALERTS mode, whose pass
                                       only journals) */
    uint8_t mode;                 /**< hl_mode_t the ladder last decided with */
    bool reboot_recheck;          /**< NO_RECEIVER: a blocked 6a waits to be looked at again */
    bool episode;                 /**< a stall or NO_RECEIVER episode is in progress (the ladder has not reset since) */
    bool restart_possible;        /**< ... and an ESP32 restart (6a) can come of it now (see "Restart pending") */
    uint8_t why;                  /**< hl_why_t: why the receiver path is down (see above) */
    uint8_t doing;                /**< step in progress: 1-5, 6 = reboot, 0 = none (see above) */
    bool waiting;                 /**< ... the station ran it and waits for RTCM in its grace */
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
