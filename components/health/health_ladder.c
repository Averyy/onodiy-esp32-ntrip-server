/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS recovery ladder: pure state machine, see health_ladder.h.
 * PROVISIONAL, to be confirmed on hardware.
 */
#include "health_ladder.h"

#include <string.h>

#define HL_LAST_STEP 5u

static int64_t grace_ms(uint8_t step)
{
    switch (step) {
    case HL_CODE_RESYNC:
        return HL_GRACE_RESYNC_MS;
    case HL_CODE_LINK_RESTART:
        return HL_GRACE_LINK_RESTART_MS;
    case HL_CODE_RESCAN:
        return HL_GRACE_RESCAN_MS;
    case HL_CODE_RECONFIGURE:
        return HL_GRACE_RECONFIGURE_MS;
    case HL_CODE_RECEIVER_RESET:
    default:
        return HL_GRACE_RECEIVER_RESET_MS;
    }
}

static uint8_t max_u8(uint8_t a, uint8_t b)
{
    return a > b ? a : b;
}

static void journal_detail(hl_t *l, uint8_t code, uint8_t outcome, uint8_t detail)
{
    if (l->ops.journal != NULL) {
        l->ops.journal(l->ops.ctx, code, outcome, detail);
    }
}

static void journal(hl_t *l, uint8_t code, uint8_t outcome)
{
    journal_detail(l, code, outcome, 0);
}

static uint8_t mode_of(const hl_in_t *in)
{
    return (in->recovery_mode == HL_MODE_NO_REBOOT || in->recovery_mode == HL_MODE_ALERTS) ? in->recovery_mode
                                                                                            : (uint8_t)HL_MODE_FULL;
}

static bool rtcm_fresh(const hl_in_t *in)
{
    return in->rtcm_age_ms != UINT32_MAX && in->rtcm_age_ms <= in->stall_ms;
}

static bool survey_hold(const hl_in_t *in)
{
    return in->survey_holds_rtcm && in->surveying;
}

/* The receiver showed within `ms` that it answers: checksum-valid text
 * lines, or replies to the command layer. */
static bool answered_within(const hl_t *l, const hl_in_t *in, int64_t ms)
{
    return (l->line_ms != HL_NEVER && in->now_ms - l->line_ms <= ms) ||
           (l->answer_ms != HL_NEVER && in->now_ms - l->answer_ms <= ms);
}

/* The receiver answered this boot (text lines or replies) and has sent no
 * RTCM frame at all. */
static bool answers_never_streamed(const hl_in_t *in)
{
    return in->rtcm_age_ms == UINT32_MAX && (in->answers != 0 || in->text_lines != 0);
}

/* `at` came after the last RTCM frame, more than HL_ANSWER_SLACK_MS after
 * it (a line that close may be that output's own); any time with no RTCM
 * frame this boot. */
static bool after_rtcm(const hl_in_t *in, int64_t at)
{
    if (at == HL_NEVER) {
        return false;
    }
    return in->rtcm_age_ms == UINT32_MAX || at - (in->now_ms - (int64_t)in->rtcm_age_ms) > HL_ANSWER_SLACK_MS;
}

/* The receiver answers now (see the header): its last line or reply came
 * after its last RTCM frame and is at most HL_ANSWER_FRESH_MS old (the
 * station asks a receiver that sends no RTCM every minute). The times are
 * exact, as the station's own rule reads them. */
static bool answered_since_rtcm(const hl_t *l, const hl_in_t *in)
{
    int64_t seen = l->line_ms > l->answer_ms ? l->line_ms : l->answer_ms;
    return seen != HL_NEVER && in->now_ms - seen <= HL_ANSWER_FRESH_MS && after_rtcm(in, seen);
}

/* The episode's "it answers" (ep_alive) still holds now: only while the
 * receiver answers by that rule. */
static bool still_answers(const hl_t *l, const hl_in_t *in)
{
    return l->ep_alive && answered_since_rtcm(l, in);
}

/* Wall time of an event `age_ms` before now, HL_NEVER for UINT32_MAX. */
static int64_t at_of(int64_t now, uint32_t age_ms)
{
    return age_ms == UINT32_MAX ? HL_NEVER : now - (int64_t)age_ms;
}

/* A grace the station no longer acts on: the step was only journalled
 * (ALERTS mode), or the user chose ALERTS after the station ran it. */
static bool watched_grace(const hl_t *l)
{
    return l->phase == HL_PH_GRACE && (l->dry || l->mode == HL_MODE_ALERTS);
}

/* A step is due, runs, or the station ran it and waits for RTCM. */
static bool step_in_progress(const hl_t *l)
{
    return (l->phase == HL_PH_WANT || l->phase == HL_PH_RUN || l->phase == HL_PH_GRACE) && !watched_grace(l);
}

/* A suspension made of the station's own sessions only: its apply
 * (CONFIGURING), a lease or a job that is not the ladder's step, and the
 * grace after their release. */
static bool sessions_only(uint8_t s, const hl_in_t *in)
{
    if (s == 0 || (s & (uint8_t) ~(HL_SUSP_STATION | HL_SUSP_LEASE | HL_SUSP_POST_LEASE)) != 0) {
        return false;
    }
    return (s & HL_SUSP_STATION) == 0 || in->stn_state == HL_STN_CONFIGURING;
}

/* Between two steps of an episode: IDLE after an abandoned step, or after
 * a step whose RTCM stopped again, while no RTCM comes; suspended at most
 * by the station's own sessions. */
static bool between_steps(const hl_t *l, const hl_in_t *in)
{
    return l->phase == HL_PH_IDLE && (l->idx != 0 || l->abandoned != 0) && !l->held_wait && !rtcm_fresh(in) &&
           (l->susp == 0 || sessions_only(l->susp, in)) && in->stn_state != HL_STN_NO_RECEIVER;
}

/* The first configuration is held and the receiver answers: no RTCM is
 * expected yet, so there is no stall to recover from. */
static bool held_explains(const hl_t *l, const hl_in_t *in)
{
    return in->rx_held && answered_within(l, in, in->stall_ms);
}

/* Wall time of the last lease release, HL_NEVER if none. */
static int64_t release_at(const hl_in_t *in)
{
    if (in->since_release_ms == UINT32_MAX) {
        return HL_NEVER;
    }
    return in->now_ms - (int64_t)in->since_release_ms;
}

/* A release inside the window of the ladder's own step (its lease). */
static bool own_release(const hl_t *l, int64_t r)
{
    if (r == HL_NEVER || l->own_from_ms == HL_NEVER || r < l->own_from_ms) {
        return false;
    }
    return l->own_to_ms == HL_NEVER || r <= l->own_to_ms;
}

static uint8_t suspension(const hl_t *l, const hl_in_t *in)
{
    uint8_t s = 0;
    switch (in->stn_state) {
    case HL_STN_IDLE:
    case HL_STN_DETECTING:
    case HL_STN_CONFIGURING:
    case HL_STN_FLASHING:
    case HL_STN_FLASH_RECOVERY:
        s |= HL_SUSP_STATION;
        break;
    default:
        break;
    }
    if (in->journal_open || in->journal_unknown) {
        s |= HL_SUSP_JOURNAL;
    }
    if (in->safe_mode) {
        s |= HL_SUSP_SAFE_MODE;
    }
    /* In NO_RECEIVER the leases are the station's own detection passes,
     * which replace the ladder's retries there: they suspend nothing. */
    if (in->stn_state != HL_STN_NO_RECEIVER && l->phase != HL_PH_RUN) {
        if (in->lease_held || in->stn_state == HL_STN_RECOVERING) {
            s |= HL_SUSP_LEASE;
        }
        int64_t r = release_at(in);
        if (r != HL_NEVER && in->now_ms - r < HL_POST_LEASE_MS && !own_release(l, r)) {
            s |= HL_SUSP_POST_LEASE;
        }
    }
    return s;
}

/* Stall: no RTCM within the threshold, counted from the later of the last
 * frame and the end of the last suspension. */
static bool stalled(const hl_t *l, const hl_in_t *in)
{
    if (rtcm_fresh(in) || survey_hold(in)) {
        return false;
    }
    return in->now_ms - l->stall_ref_ms > (int64_t)in->stall_ms;
}

/* From what the receiver sends unasked (see the header): its replies to the
 * station's own commands are no part of it. */
hl_class_t hl_classify(const hl_t *l, const hl_in_t *in)
{
    if (l->unasked_ms != HL_NEVER && in->now_ms - l->unasked_ms <= 2 * (int64_t)in->stall_ms &&
        after_rtcm(in, l->unasked_ms)) {
        return HL_CLASS_TEXT_ONLY;
    }
    if (l->garbage_ms != HL_NEVER && in->now_ms - l->garbage_ms <= (int64_t)in->stall_ms) {
        return HL_CLASS_GARBAGE;
    }
    return HL_CLASS_NO_BYTES;
}

uint8_t hl_entry_step(hl_class_t c)
{
    switch (c) {
    case HL_CLASS_GARBAGE:
        return HL_CODE_RESCAN;
    case HL_CLASS_TEXT_ONLY:
        return HL_CODE_RECONFIGURE;
    case HL_CLASS_NO_BYTES:
    default:
        return HL_CODE_RESYNC;
    }
}

/* ---- transitions -------------------------------------------------------- */

static void enter_failed(hl_t *l, const hl_in_t *in)
{
    if (l->ep_alive) {
        l->no_rtcm = true;   /* it answers: steps 4 and 5 are not run again (the retry pass) */
    }
    journal(l, HL_CODE_FAILED, HL_OUT_STARTED);
    l->phase = HL_PH_FAILED;
    l->step = 0;
    l->retry_at = l->active_ms + HL_FAILED_RETRY_MS;
    l->hold_no_rx = (in->stn_state == HL_STN_NO_RECEIVER);
    l->healthy = false;
}

/* Which limit blocks 6a (journal detail of code 9, outcome STARTED): the
 * cap's own limit when it blocks, else the inhibit. */
static uint8_t blocked_why(const hl_in_t *in)
{
    if (!in->reboot_allowed) {
        return (in->reboot_cap_why == HL_CAP_HOUR || in->reboot_cap_why == HL_CAP_DAY) ? in->reboot_cap_why
                                                                                       : HL_CAP_UNKNOWN;
    }
    return HL_CAP_LOCK;
}

/* After a failed step 3 (not found) or 5, and for a NO_RECEIVER episode.
 * ALERTS mode: neither 6a nor 6b, straight to 7. NO_REBOOT mode: 6a is
 * journalled as not run and the ladder goes to 7. The cap and an inhibit
 * block 6a first (outcome STARTED of code 9, as before the hold existed,
 * with the limit that blocks it as the detail: hl_cap_why_t); otherwise a
 * hold postpones it (outcome HELD with the hold bits). A 6a that could not
 * be taken during a NO_RECEIVER episode is looked at again (no_receiver()). */
static void byte_rule(hl_t *l, const hl_in_t *in, bool bytes)
{
    uint8_t mode = mode_of(in);
    l->blocked_6a = false;
    if (mode == HL_MODE_ALERTS) {
        enter_failed(l, in);
        return;
    }
    if (!bytes) {
        if (mode == HL_MODE_NO_REBOOT) {
            journal(l, HL_CODE_REBOOT_BLOCKED, HL_OUT_NOT_RUN);
        } else if (!in->reboot_allowed || in->reboot_inhibited) {
            journal_detail(l, HL_CODE_REBOOT_BLOCKED, HL_OUT_STARTED, blocked_why(in));
        } else if (in->reboot_hold != 0) {
            journal_detail(l, HL_CODE_REBOOT_BLOCKED, HL_OUT_HELD, in->reboot_hold);
        } else {
            journal(l, HL_CODE_REBOOT, HL_OUT_STARTED);
            if (l->ops.reboot == NULL || l->ops.reboot(l->ops.ctx)) {
                l->recoveries++;
                l->last_step = HL_CODE_REBOOT;
                l->phase = HL_PH_REBOOTING;
                l->step = 0;
                l->reboot_ms = in->now_ms;
                return;
            }
            /* Blocked after all, by an inhibit taken since this tick's
             * sample (the cap only ever opens up within a boot). */
            journal_detail(l, HL_CODE_REBOOT_BLOCKED, HL_OUT_STARTED, HL_CAP_LOCK);
        }
        if (in->stn_state == HL_STN_NO_RECEIVER) {
            l->blocked_6a = true;
            l->blocked_at_pass = in->detect_passes;
        }
    } else {
        journal(l, HL_CODE_POWER_ALERT, HL_OUT_STARTED);
        l->power_alert = true;
    }
    enter_failed(l, in);
}

static void start_run(hl_t *l, const hl_in_t *in)
{
    journal(l, l->step, HL_OUT_STARTED);
    l->abandoned_twice = false;
    l->recoveries++;
    l->last_step = l->step;
    l->phase = HL_PH_RUN;
    l->req_ms = in->now_ms;
    l->enters0 = in->rec_enters;
    l->exits0 = in->rec_exits;
    l->seen_running = false;
    l->seen_ms = HL_NEVER;
    l->win_bytes0 = in->rx_bytes;
    l->win_answers0 = in->answers;
    l->win_from_ms = in->now_ms;
    l->win_saw_bytes = false;
    l->not_found = false;
    l->own_from_ms = in->now_ms;
    l->own_to_ms = HL_NEVER;
}

/* A refused RECONFIGURE fails at once; the ladder goes on to step 5. In
 * ALERTS mode the full ladder would not have started it either: no record. */
static void refused(hl_t *l, const hl_in_t *in)
{
    if (mode_of(in) != HL_MODE_ALERTS) {
        journal(l, HL_CODE_RECONFIGURE, HL_OUT_FAILED);
    }
    l->idx = HL_CODE_RECONFIGURE;
    l->step = HL_CODE_RECEIVER_RESET;
}

/* ALERTS mode: the step is only journalled (once per episode: one that
 * comes due again after a suspension is not journalled again); its grace
 * runs as if it had. */
static void dry_start(hl_t *l)
{
    uint8_t bit = (uint8_t)(1u << l->step);
    if ((l->dry_logged & bit) == 0) {
        l->dry_logged |= bit;
        journal(l, l->step, HL_OUT_NOT_RUN);
    }
    l->dry = true;
    l->phase = HL_PH_GRACE;
    l->grace_start = l->active_ms;
    l->not_found = false;
}

static void try_request(hl_t *l, const hl_in_t *in)
{
    for (int guard = 0; guard < 2 && l->phase == HL_PH_WANT; guard++) {
        if (l->step == HL_CODE_RECEIVER_RESET && in->no_sky && answered_within(l, in, in->stall_ms)) {
            /* It answers and says it uses no satellites: a reset cannot
             * bring satellites back, the antenna can. */
            l->ep_alive = true;
            enter_failed(l, in);
            return;
        }
        if (l->step == HL_CODE_RECONFIGURE && in->reconfigure_refused) {
            refused(l, in);
            continue;
        }
        if (mode_of(in) == HL_MODE_ALERTS) {
            dry_start(l);
            return;
        }
        hl_req_t r = (l->ops.request != NULL) ? l->ops.request(l->ops.ctx, l->step) : HL_REQ_REJECTED;
        if (r == HL_REQ_ACCEPTED) {
            start_run(l, in);
            return;
        }
        if (r == HL_REQ_REFUSED && l->step == HL_CODE_RECONFIGURE) {
            refused(l, in);
            continue;
        }
        return;   /* rejected: asked again on the next tick */
    }
}

static void want(hl_t *l, const hl_in_t *in, uint8_t step)
{
    l->step = step;
    l->phase = HL_PH_WANT;
    l->healthy = false;
    l->dry = false;
    try_request(l, in);
}

/* Continue after the step index `idx` with the stall classed as `c`. */
/* Replies since the last step went out (step 5: the RESET's banner, its
 * version log) or text lines in its window (step 5: its grace): the
 * receiver answers, it is not a power-cycle or a no-receiver case. */
static bool step_answered(const hl_t *l, const hl_in_t *in)
{
    return in->answers != l->win_answers0 || (l->line_ms != HL_NEVER && l->line_ms >= l->win_from_ms);
}

static void escalate(hl_t *l, const hl_in_t *in, hl_class_t c)
{
    uint8_t next = l->idx == 0 ? hl_entry_step(c) : max_u8((uint8_t)(l->idx + 1), hl_entry_step(c));
    bool twice = l->abandoned_twice;
    l->abandoned_twice = false;
    if (l->no_rtcm && next >= HL_CODE_RECONFIGURE) {
        /* The retry pass of a receiver that answers without RTCM: steps 4
         * and 5 were tried (a class that turned TEXT_ONLY on the way). */
        enter_failed(l, in);
        return;
    }
    if (next > HL_LAST_STEP) {
        /* After step 5, the rule of its failure: right after it was
         * abandoned a second time (it counts as failed), a receiver that
         * answered since the RESET went out; in a stall right after a step
         * 5 that brought RTCM back, a receiver that answers now. Neither
         * gets the power-cycle advice nor an ESP32 restart; one that sends
         * nothing gets the byte rule on its class. */
        bool answered = twice ? step_answered(l, in) : answered_since_rtcm(l, in);
        if (l->idx == HL_CODE_RECEIVER_RESET && answered) {
            l->ep_alive = true;
            enter_failed(l, in);
            return;
        }
        byte_rule(l, in, c != HL_CLASS_NO_BYTES);
        return;
    }
    want(l, in, next);
}

static void step_failed(hl_t *l, const hl_in_t *in)
{
    uint8_t step = l->step;
    journal(l, step, HL_OUT_FAILED);
    l->idx = step;
    switch (step) {
    case HL_CODE_RESCAN:
        if (l->not_found) {
            l->ep_alive = false;   /* asked at every baud: nothing answered */
            l->no_rtcm = false;
            byte_rule(l, in, in->rx_bytes != l->win_bytes0 || l->win_saw_bytes);
        } else if (l->no_rtcm) {
            enter_failed(l, in);   /* found, and it answered before without RTCM: not steps 4 and 5 again */
        } else {
            want(l, in, HL_CODE_RECONFIGURE);
        }
        break;
    case HL_CODE_RECONFIGURE:
        want(l, in, HL_CODE_RECEIVER_RESET);
        break;
    case HL_CODE_RECEIVER_RESET: {
        bool answered = step_answered(l, in);
        l->ep_alive = answered;
        if (answered) {
            enter_failed(l, in);
        } else {
            byte_rule(l, in, in->rx_bytes != l->win_bytes0);
        }
        break;
    }
    default:
        escalate(l, in, hl_classify(l, in));
        break;
    }
}

/* ALERTS mode: the grace of a step that was only journalled ran out. Where
 * the byte rule would run (after step 3 or 5) the ladder goes to 7. */
static void dry_failed(hl_t *l, const hl_in_t *in)
{
    uint8_t step = l->step;
    l->idx = step;
    l->dry = false;
    if (step == HL_CODE_RESCAN || step == HL_CODE_RECEIVER_RESET) {
        enter_failed(l, in);
        return;
    }
    escalate(l, in, hl_classify(l, in));
}

static void step_healthy(hl_t *l)
{
    if (!l->dry) {
        journal(l, l->step, HL_OUT_HEALTHY);   /* a step that was not run has no result */
    }
    l->dry = false;
    l->idx = l->step;
    l->step = 0;
    l->phase = HL_PH_IDLE;
    l->healthy = true;
    l->healthy_since = l->active_ms;
    l->abandoned_twice = false;
}

/* A suspension other than the station's own sessions began (keep_step()
 * keeps a step through those): a step in progress is abandoned and tried
 * again later. A step that ran and is abandoned a second time in the same
 * episode counts as failed instead: the ladder goes on past it once the
 * suspension ends, so the same step is never abandoned and re-run for as
 * long as the fault lasts. */
static void abandon(hl_t *l)
{
    bool repeat = false;
    if (l->dry) {
        /* only journalled: nothing ran that could be abandoned */
        l->dry = false;
    } else if (l->phase == HL_PH_RUN || l->phase == HL_PH_GRACE) {
        journal(l, l->step, HL_OUT_ABANDONED);
        if (l->phase == HL_PH_RUN) {
            l->own_to_ms = l->last_now;   /* the step's window closes here */
        }
        if (l->step >= 1 && l->step <= HL_LAST_STEP) {
            uint8_t bit = (uint8_t)(1u << l->step);
            repeat = (l->abandoned & bit) != 0;
            l->abandoned |= bit;
        }
    }
    if (l->phase == HL_PH_WANT || l->phase == HL_PH_RUN || l->phase == HL_PH_GRACE) {
        l->abandoned_twice = repeat;
        if (repeat) {
            l->idx = l->step;
        } else {
            l->idx = (uint8_t)(l->step > 0 ? l->step - 1 : 0);
        }
        l->step = 0;
        l->phase = HL_PH_IDLE;
        l->healthy = false;
    }
}

static void ladder_reset(hl_t *l)
{
    if (l->phase == HL_PH_FAILED) {
        journal(l, HL_CODE_FAILED, HL_OUT_HEALTHY);
    }
    l->phase = HL_PH_IDLE;
    l->idx = 0;
    l->step = 0;
    l->power_alert = false;
    l->hold_no_rx = false;
    l->abandoned = 0;
    l->abandoned_twice = false;
    l->blocked_6a = false;
    l->dry = false;
    l->dry_logged = 0;
    l->ep_alive = false;
    l->no_rtcm = false;
    l->held_wait = false;
}

/* Healthy-for-60-s tracking (IDLE and FAILED). */
static void track_health(hl_t *l, const hl_in_t *in)
{
    if (!rtcm_fresh(in)) {
        l->healthy = false;
        return;
    }
    l->abandoned_twice = false;   /* RTCM came back: a later stall is a new one */
    if (!l->healthy) {
        l->healthy = true;
        l->healthy_since = l->active_ms;
        return;
    }
    if (l->active_ms - l->healthy_since >= HL_HEALTHY_RESET_MS &&
        (l->idx != 0 || l->phase == HL_PH_FAILED || l->power_alert)) {
        ladder_reset(l);
    }
}

/* ---- phases -------------------------------------------------------------- */

/* The station has run the requested step (or must have, unseen). */
static bool run_ended(hl_t *l, const hl_in_t *in)
{
    if (in->stn_state == HL_STN_RECOVERING && !l->seen_running) {
        l->seen_running = true;
        l->seen_ms = in->now_ms;
    }
    bool started = l->seen_running || in->rec_enters != l->enters0;
    if (started) {
        return in->stn_state != HL_STN_RECOVERING ||
               (l->seen_ms != HL_NEVER && in->now_ms - l->seen_ms >= HL_RUN_CAP_MS);
    }
    return in->now_ms - l->req_ms >= HL_PICKUP_TIMEOUT_MS;   /* ran between two samples, events lost */
}

/* The step ran: its grace begins. */
static void end_run(hl_t *l, const hl_in_t *in)
{
    l->own_to_ms = in->now_ms;
    l->grace_start = l->active_ms;
    l->phase = HL_PH_GRACE;
    if (l->step == HL_CODE_RESCAN && in->stn_state == HL_STN_NO_RECEIVER) {
        /* Not found: this NO_RECEIVER episode is the rescan's failure. */
        l->not_found = true;
        l->no_rx = true;
        l->no_rx_handled = true;
        l->win_saw_bytes = in->detect_saw_bytes;
    }
    if (l->step == HL_CODE_RECEIVER_RESET) {
        /* Its byte window is the grace, and so is its text-line window: a
         * line from before the RESET took effect says nothing about after.
         * Replies count from the step's start (the RESET's own banner and
         * version log come while it runs). */
        l->win_bytes0 = in->rx_bytes;
        l->win_from_ms = in->now_ms;
    }
}

static void run_phase(hl_t *l, const hl_in_t *in)
{
    if (run_ended(l, in)) {
        end_run(l, in);
    }
}

/* The station's own session suspends the ladder: a step whose action is
 * done keeps its evidence, and its grace waits (the ladder clock stops)
 * instead of being abandoned and run again. A step that only ran until now
 * (the station went from the ladder's job straight into a session of its
 * own) enters its grace first. A grace in ALERTS mode (a step only
 * journalled) waits the same way. */
static bool keep_step(hl_t *l, const hl_in_t *in)
{
    if (l->phase == HL_PH_RUN && run_ended(l, in)) {
        end_run(l, in);
    }
    return l->phase == HL_PH_GRACE;
}

static void grace_phase(hl_t *l, const hl_in_t *in)
{
    if (l->not_found && in->stn_state == HL_STN_NO_RECEIVER && in->detect_saw_bytes) {
        l->win_saw_bytes = true;
    }
    if (rtcm_fresh(in)) {
        step_healthy(l);
        return;
    }
    /* Step 4 waits out the survey window, except for a receiver that
     * answers and never streamed this boot (see the header). */
    bool hold = survey_hold(in) ||
                (l->step == HL_CODE_RECONFIGURE && in->surveying && !answers_never_streamed(in));
    if (l->active_ms - l->grace_start >= grace_ms(l->step) && !hold) {
        if (l->dry) {
            dry_failed(l, in);
        } else {
            step_failed(l, in);
        }
    }
}

static void failed_phase(hl_t *l, const hl_in_t *in)
{
    track_health(l, in);
    if (l->phase != HL_PH_FAILED || l->hold_no_rx) {
        return;
    }
    if (held_explains(l, in)) {
        /* It answers again and was never set to send RTCM: waiting for the
         * user's configuration is no failure (and no recovery either, so the
         * journal gets no "healthy again"). */
        l->phase = HL_PH_IDLE;
        ladder_reset(l);
        l->held_wait = true;
        return;
    }
    if (l->active_ms < l->retry_at || !stalled(l, in)) {
        return;
    }
    hl_class_t c = hl_classify(l, in);
    if (c == HL_CLASS_TEXT_ONLY && (in->reconfigure_refused || l->no_rtcm)) {
        /* The station's apply backoff (or the user) owns the re-applies; or
         * the receiver answers without RTCM and steps 4-5 were tried. */
        l->retry_at = l->active_ms + HL_FAILED_RETRY_MS;
        return;
    }
    l->retry_at = l->active_ms + HL_FAILED_RETRY_MS;
    uint8_t step = max_u8(HL_CODE_LINK_RESTART, hl_entry_step(c));
    if (mode_of(in) == HL_MODE_ALERTS) {
        journal(l, step, HL_OUT_NOT_RUN);   /* the pass it would start; nothing is sent */
        return;
    }
    want(l, in, step);
}

static void idle_phase(hl_t *l, const hl_in_t *in)
{
    track_health(l, in);
    if (l->phase != HL_PH_IDLE || in->stn_state == HL_STN_NO_RECEIVER) {
        l->held_wait = false;
        return;
    }
    if (!stalled(l, in)) {
        l->held_wait = false;
        return;
    }
    if (held_explains(l, in)) {
        l->held_wait = true;   /* no stall: the receiver was never set to send RTCM */
        return;
    }
    l->held_wait = false;
    l->stalls++;
    /* Alive: it sends text now, or it replied to the station not long ago
     * (replies come only when asked, so their age is the evidence; a text
     * output that stopped is silence). */
    l->ep_alive = answered_within(l, in, in->stall_ms) ||
                  (l->answer_ms != HL_NEVER && in->now_ms - l->answer_ms <= HL_ALIVE_MS);
    escalate(l, in, hl_classify(l, in));
}

/* A 6a this episode could not take, looked at again: full mode, the cap
 * allows a reboot now, no inhibit is held, nothing holds it back, and the
 * station has completed a detection pass since the block (its result is
 * what the rule reads). */
static bool recheck_due(const hl_t *l, const hl_in_t *in)
{
    return l->blocked_6a && mode_of(in) == HL_MODE_FULL && in->reboot_allowed && !in->reboot_inhibited &&
           in->reboot_hold == 0 && (int32_t)(in->detect_passes - l->blocked_at_pass) > 0;
}

/* NO_RECEIVER episodes: the byte rule once on the station's whole pass,
 * and again on a later pass for a 6a that was blocked. */
static bool no_receiver(hl_t *l, const hl_in_t *in)
{
    if (in->stn_state != HL_STN_NO_RECEIVER) {
        if (l->no_rx) {
            l->no_rx = false;
            l->no_rx_handled = false;
            l->blocked_6a = false;
            if (l->phase == HL_PH_FAILED && l->hold_no_rx) {
                l->hold_no_rx = false;
                l->retry_at = l->active_ms + HL_FAILED_RETRY_MS;
            }
        }
        return false;
    }
    if (!l->no_rx) {
        l->no_rx = true;
        l->no_rx_handled = false;
    }
    if (l->no_rx_handled || l->phase == HL_PH_RUN || l->phase == HL_PH_GRACE) {
        if (l->phase == HL_PH_FAILED) {
            l->hold_no_rx = true;
            if (recheck_due(l, in)) {
                byte_rule(l, in, in->detect_saw_bytes);
                return true;
            }
        }
        return false;
    }
    l->no_rx_handled = true;
    if (l->phase == HL_PH_IDLE && l->idx == 0) {
        l->stalls++;   /* a new episode, not part of a declared stall */
    }
    l->step = 0;
    l->ep_alive = false;   /* the station's search found nothing that answers */
    l->no_rtcm = false;
    l->held_wait = false;
    byte_rule(l, in, in->detect_saw_bytes);
    return true;
}

/* The user left the ALERTS mode: a stall in progress starts a fresh pass at
 * its class entry step (a step that was only journalled is dropped). In a
 * NO_RECEIVER episode the byte rule runs again on the station's last pass. */
static void leave_alerts(hl_t *l, const hl_in_t *in)
{
    if (in->stn_state == HL_STN_NO_RECEIVER) {
        if (l->no_rx && l->phase == HL_PH_FAILED) {
            l->no_rx_handled = false;
        }
        return;
    }
    bool dry_step = l->dry && l->phase == HL_PH_GRACE;
    if (!dry_step && l->phase != HL_PH_FAILED && l->phase != HL_PH_IDLE) {
        return;   /* a step that really runs completes first */
    }
    if (!dry_step && !stalled(l, in)) {
        return;
    }
    l->dry = false;
    l->dry_logged = 0;
    l->phase = HL_PH_IDLE;
    l->idx = 0;
    l->step = 0;
    l->abandoned = 0;
    l->abandoned_twice = false;
    l->hold_no_rx = false;
    if (stalled(l, in)) {
        escalate(l, in, hl_classify(l, in));
    }
}

/* ---- API ----------------------------------------------------------------- */

void hl_init(hl_t *l, const hl_ops_t *ops, int64_t now_ms)
{
    memset(l, 0, sizeof(*l));
    if (ops != NULL) {
        l->ops = *ops;
    }
    l->phase = HL_PH_IDLE;
    l->last_now = now_ms;
    l->stall_ref_ms = now_ms;
    l->line_ms = HL_NEVER;
    l->answer_ms = HL_NEVER;
    l->unasked_ms = HL_NEVER;
    l->garbage_ms = HL_NEVER;
    l->win_from_ms = HL_NEVER;
    l->req_ms = HL_NEVER;
    l->seen_ms = HL_NEVER;
    l->own_from_ms = HL_NEVER;
    l->own_to_ms = HL_NEVER;
    l->reboot_ms = HL_NEVER;
}

void hl_tick(hl_t *l, const hl_in_t *in)
{
    int64_t now = in->now_ms;
    int64_t dt = now - l->last_now;
    if (dt < 0) {
        dt = 0;
    } else if (dt > HL_MAX_TICK_MS) {
        dt = HL_MAX_TICK_MS;
    }
    l->last_now = now;

    /* Text-line and reply liveness: new ones counted from the samples,
     * their times exact (a tick-sampled time would be up to a tick late,
     * and count a line just after the last RTCM frame as an answer). */
    if (!l->lines_valid) {
        l->lines_valid = true;
        l->lines_last = in->text_lines;
        l->answers_last = in->answers;
        l->discarded_last = in->discarded;
    } else {
        if (in->text_lines != l->lines_last) {
            l->lines_last = in->text_lines;
            l->ep_alive = true;
        }
        if (in->answers != l->answers_last) {
            l->answers_last = in->answers;
            l->ep_alive = true;
        }
        if (in->discarded != l->discarded_last) {
            if (in->discarded > l->discarded_last) {
                l->garbage_ms = now;   /* a counter that went back (a new framer) is no garbage */
            }
            l->discarded_last = in->discarded;
        }
    }
    l->line_ms = at_of(now, in->line_age_ms);
    l->answer_ms = at_of(now, in->answer_age_ms);
    l->unasked_ms = at_of(now, in->unasked_line_age_ms);

    if (l->phase == HL_PH_REBOOTING) {
        if (now - l->reboot_ms >= HL_REBOOT_WAIT_MS) {
            enter_failed(l, in);   /* still alive: nothing else to try now */
        }
        return;
    }

    uint8_t prev = l->susp;
    uint8_t s = suspension(l, in);
    l->susp = s;
    if (s != 0) {
        if (!(sessions_only(s, in) && keep_step(l, in))) {
            abandon(l);
        }
        l->was_suspended = true;
        return;
    }
    l->active_ms += dt;
    if (l->was_suspended) {
        /* The stall timer restarts at the end of the suspension, or at the
         * release itself after a post-lease grace. */
        l->was_suspended = false;
        int64_t r = release_at(in);
        l->stall_ref_ms = (prev == HL_SUSP_POST_LEASE && r != HL_NEVER) ? r : now;
    }
    if (survey_hold(in)) {
        l->stall_ref_ms = now;
    }

    uint8_t mode = mode_of(in);
    if (l->mode == HL_MODE_ALERTS && mode != HL_MODE_ALERTS) {
        l->mode = mode;
        leave_alerts(l, in);
    }
    l->mode = mode;

    if (no_receiver(l, in)) {
        return;
    }

    switch (l->phase) {
    case HL_PH_IDLE:
        idle_phase(l, in);
        break;
    case HL_PH_WANT:
        try_request(l, in);
        break;
    case HL_PH_RUN:
        run_phase(l, in);
        break;
    case HL_PH_GRACE:
        grace_phase(l, in);
        break;
    case HL_PH_FAILED:
        failed_phase(l, in);
        break;
    default:
        break;
    }
}

/* Why the receiver path is down, from the evidence at hand (see the header). */
static uint8_t why_of(const hl_t *l, const hl_in_t *in, bool fresh)
{
    if (in == NULL || fresh) {
        return HL_WHY_NONE;
    }
    if (l->held_wait) {
        return HL_WHY_HELD;
    }
    /* Between steps: as while one runs (ALERTS mode: as while one is watched). */
    bool between = between_steps(l, in);
    bool watched = watched_grace(l) || (between && l->mode == HL_MODE_ALERTS);
    bool fixing = step_in_progress(l) || (between && l->mode != HL_MODE_ALERTS);
    if (l->phase != HL_PH_FAILED && !watched && !fixing) {
        return HL_WHY_NONE;   /* healthy, rebooting, or not declared yet */
    }
    if (l->power_alert) {
        return HL_WHY_NO_ANSWER;
    }
    if (l->hold_no_rx || in->stn_state == HL_STN_NO_RECEIVER) {
        return HL_WHY_NONE;   /* the station's no-receiver alert says it */
    }
    if (fixing) {
        /* While the station works on it: only the antenna hint, and only
         * once the receiver answered since its last RTCM frame. */
        if (!answered_since_rtcm(l, in)) {
            return HL_WHY_NONE;
        }
        return in->no_sky ? HL_WHY_NO_SKY : HL_WHY_NO_RTCM;
    }
    if (still_answers(l, in)) {
        return in->no_sky ? HL_WHY_NO_SKY : HL_WHY_NO_RTCM;
    }
    return HL_WHY_SILENT;
}

/* The receiver answers by the ladder's evidence: the reason says so, or,
 * where none is given (a suspension, a stall not declared again yet), it
 * answered after its last RTCM frame. Not in NO_RECEIVER or while 6a runs. */
static bool answers_now(const hl_t *l, const hl_in_t *in, uint8_t why)
{
    if (why == HL_WHY_NO_RTCM || why == HL_WHY_NO_SKY || why == HL_WHY_HELD) {
        return true;
    }
    return why == HL_WHY_NONE && in != NULL && l->phase != HL_PH_REBOOTING && !l->hold_no_rx &&
           in->stn_state != HL_STN_NO_RECEIVER && answered_since_rtcm(l, in);
}

void hl_get_status(const hl_t *l, const hl_in_t *in, hl_status_t *out)
{
    memset(out, 0, sizeof(*out));
    bool fresh = in != NULL && rtcm_fresh(in);
    switch (l->phase) {
    case HL_PH_WANT:
    case HL_PH_RUN:
    case HL_PH_GRACE:
        out->rung = l->step;
        if (watched_grace(l)) {
            out->level = HL_LEVEL_DEGRADED;   /* ALERTS mode: nothing recovers, the stall is only reported */
        } else {
            out->level = HL_LEVEL_RECOVERING;
            out->doing = l->step;
            out->waiting = l->phase == HL_PH_GRACE;
        }
        break;
    case HL_PH_REBOOTING:
        out->level = HL_LEVEL_RECOVERING;
        out->rung = HL_CODE_REBOOT;
        out->doing = HL_CODE_REBOOT;
        break;
    case HL_PH_FAILED:
        out->level = fresh ? HL_LEVEL_DEGRADED : HL_LEVEL_FAILED;
        out->rung = l->power_alert ? HL_CODE_POWER_ALERT : HL_CODE_FAILED;
        if (!l->hold_no_rx && l->mode != HL_MODE_ALERTS) {   /* ALERTS: the pass only journals, nothing is attempted */
            int64_t left = l->retry_at - l->active_ms;
            out->retry_in_s = left <= 0 ? 0 : (uint32_t)((left + 999) / 1000);
        }
        break;
    case HL_PH_IDLE:
    default:
        if (in != NULL && between_steps(l, in)) {
            out->level = l->mode == HL_MODE_ALERTS ? HL_LEVEL_DEGRADED : HL_LEVEL_RECOVERING;
        } else {
            out->level = fresh ? HL_LEVEL_OK : HL_LEVEL_DEGRADED;
        }
        break;
    }
    out->phase = (uint8_t)l->phase;
    out->last_step = l->last_step;
    out->suspended = l->susp != 0;
    out->susp = l->susp;
    out->power_alert = l->power_alert;
    out->stalls = l->stalls;
    out->recoveries = l->recoveries;
    out->mode = l->mode;
    out->reboot_recheck = l->blocked_6a && l->mode == HL_MODE_FULL;
    out->episode = l->phase != HL_PH_IDLE || l->idx != 0 || l->no_rx || l->power_alert;
    out->why = why_of(l, in, fresh);
    out->restart_possible = out->episode && !fresh && !answers_now(l, in, out->why);
}
