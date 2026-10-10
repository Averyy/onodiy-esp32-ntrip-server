/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "station_core.h"

#include <string.h>

/* ---- small helpers -------------------------------------------------------- */

static const uint32_t k_short_list[] = { 115200, 460800, 921600, 230400 };
static const uint32_t k_rest_list[] = { 57600, 38400, 19200, 9600 };

static bool baud_known(uint32_t baud)
{
    for (unsigned i = 0; i < sizeof(k_short_list) / sizeof(k_short_list[0]); i++) {
        if (k_short_list[i] == baud || k_rest_list[i] == baud) {
            return true;
        }
    }
    return false;
}

static unsigned add_candidate(uint32_t baud, uint32_t *out, unsigned n, unsigned cap)
{
    if (n >= cap || !baud_known(baud)) {
        return n;
    }
    for (unsigned i = 0; i < n; i++) {
        if (out[i] == baud) {
            return n;
        }
    }
    out[n] = baud;
    return n + 1;
}

unsigned stn_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, unsigned cap)
{
    unsigned n = add_candidate(stored_baud, out, 0, cap);
    for (unsigned i = 0; i < sizeof(k_short_list) / sizeof(k_short_list[0]); i++) {
        n = add_candidate(k_short_list[i], out, n, cap);
    }
    if (full) {
        for (unsigned i = 0; i < sizeof(k_rest_list) / sizeof(k_rest_list[0]); i++) {
            n = add_candidate(k_rest_list[i], out, n, cap);
        }
    }
    return n;
}

int64_t stn_apply_backoff_ms(uint32_t failures)
{
    static const int64_t k_ms[] = { 10 * 60000, 30 * 60000, 2 * 3600000, 6 * 3600000 };
    const uint32_t last = sizeof(k_ms) / sizeof(k_ms[0]) - 1;
    uint32_t i = failures > 0 ? failures - 1 : 0;
    return k_ms[i > last ? last : i];
}

/* Counters only grow; one that went back (a re-created framer) counts as nothing new. */
static uint64_t grown(uint64_t now, uint64_t base)
{
    return now > base ? now - base : 0;
}

static bool data_since_base(const stn_t *s, const stn_in_t *in)
{
    return grown(in->rtcm_frames, s->base_rtcm) >= 1 || grown(in->text_lines, s->base_lines) >= 2;
}

static bool rtcm_since_base(const stn_t *s, const stn_in_t *in)
{
    return grown(in->rtcm_frames, s->base_rtcm) >= 1;
}

static void take_base(stn_t *s, const stn_in_t *in)
{
    s->base_rtcm = in->rtcm_frames;
    s->base_lines = in->text_lines;
}

static bool config_failed(const stn_t *s)
{
    return (s->alerts & STN_ALERT_CONFIG_FAILED) != 0;
}

/* Automatic applies: the user lets the station configure the receiver, the
 * failure backoff does not own the re-applies, and the receiver is the model
 * we configure. */
static bool auto_apply_allowed(const stn_t *s, const stn_in_t *in)
{
    return in->autocfg && !config_failed(s) && s->identity_ok && s->supported;
}

static void want_apply(stn_t *s, stn_why_t why, stn_change_t change, bool force)
{
    if (s->apply_wanted && change == STN_CHANGE_NONE) {
        s->apply_force = s->apply_force || force;   /* a queued change keeps its reason */
        return;
    }
    s->apply_wanted = true;
    s->apply_why = (uint8_t)why;
    s->apply_change = (uint8_t)change;
    s->apply_force = force || change != STN_CHANGE_NONE;
}

static void raise_config_failed(stn_t *s, int64_t now)
{
    s->alerts |= STN_ALERT_CONFIG_FAILED;
    if (s->apply_failures < UINT32_MAX) {
        s->apply_failures++;
    }
    s->apply_retry_ms = now + stn_apply_backoff_ms(s->apply_failures);
}

static void set_identity(stn_t *s, bool supported, bool static_ok)
{
    s->identity_ok = true;
    s->supported = supported;
    s->static_ok = static_ok;
    s->identify_ms = STN_NEVER;
    s->identify_leased = false;
    if (supported) {
        s->alerts &= ~STN_ALERT_UNSUPPORTED_MODEL;
    } else {
        s->alerts |= STN_ALERT_UNSUPPORTED_MODEL;
        s->profile_applied = false;
    }
}

/* After a fresh identity: what the static part of the applied check says. */
static void after_identity(stn_t *s, const stn_in_t *in)
{
    if (!s->supported) {
        return;   /* stream only: nothing is configured, nothing is flashed */
    }
    if (!s->static_ok) {
        s->profile_applied = false;
        if (auto_apply_allowed(s, in)) {
            want_apply(s, STN_WHY_CHECK, STN_CHANGE_NONE, false);
        }
    }
    if (in->profile_survey && !s->mode_probe_done) {
        s->mode_probe = true;   /* is a survey running already (an ESP32-only reboot)? */
    }
}

typedef enum {
    ENTER_CHECK = 0,   /* the next window is the applied check */
    ENTER_VERIFY,      /* the next window checks the apply that just ran */
    ENTER_KEEP,        /* windows go on as before */
} enter_t;

static void enter_streaming(stn_t *s, const stn_in_t *in, enter_t how)
{
    s->state = STN_STREAMING;
    s->alerts &= ~STN_ALERT_RECEIVER_IN_LOADER;
    s->listen_only = false;
    s->user_restart = false;
    s->detect_failures = 0;
    s->short_fails = 0;
    s->detect_ms = STN_NEVER;
    s->episode_ms = STN_NEVER;
    if (!s->identity_ok && s->identify_ms == STN_NEVER) {
        s->identify_ms = in->now_ms;
    }
    if (s->check_ms == STN_NEVER) {
        s->obs_on = true;
        s->obs_rearm = true;
    }
    s->drift_windows = 0;
    if (how == ENTER_CHECK) {
        s->check_pending = true;
        s->verify_pending = false;
    } else if (how == ENTER_VERIFY) {
        s->verify_pending = true;
        s->check_pending = false;
    }
    take_base(s, in);
}

/* The receiver is gone: queued ladder steps and automatic applies were about
 * a receiver that no longer answers (the user's requests stay). */
static void drop_stale_requests(stn_t *s)
{
    s->rec_pending = 0;
    if (s->apply_wanted && (s->apply_why == STN_WHY_CHECK || s->apply_why == STN_WHY_DRIFT)) {
        s->apply_wanted = false;
    }
}

static void enter_detecting(stn_t *s, const stn_in_t *in)
{
    drop_stale_requests(s);
    s->state = STN_DETECTING;
    s->episode_ms = in->now_ms;
    s->detect_ms = in->now_ms;
    s->short_fails = 0;
    s->obs_on = false;
    s->profile_applied = false;
    s->antenna_check = false;
    s->antenna_probe = false;
    take_base(s, in);
}

/* End of a session (apply, its last resort, a ladder step): casters resume. */
static void back_to_streaming(stn_t *s, const stn_in_t *in)
{
    s->paused = false;
    s->last_resort = false;
    enter_streaming(s, in, ENTER_KEEP);
}

static stn_job_t make_job(stn_job_kind_t kind)
{
    stn_job_t j;
    memset(&j, 0, sizeof(j));
    j.kind = (uint8_t)kind;
    return j;
}

static stn_job_t start_apply(stn_t *s, stn_why_t why, stn_change_t change, bool force)
{
    s->apply_wanted = false;
    s->session_state = (why == STN_WHY_LADDER) ? STN_RECOVERING : STN_CONFIGURING;
    s->state = s->session_state;
    s->paused = true;
    s->obs_on = false;
    stn_job_t j = make_job(STN_JOB_APPLY);
    j.why = (uint8_t)why;
    j.change = (uint8_t)change;
    j.force_mode = force || change != STN_CHANGE_NONE;
    return j;
}

static stn_job_t mode_job(stn_mode_why_t why)
{
    stn_job_t j = make_job(STN_JOB_MODE);
    j.mode_why = (uint8_t)why;
    return j;
}

/* ---- lifecycle ------------------------------------------------------------- */

void stn_init(stn_t *s, bool journal_open)
{
    memset(s, 0, sizeof(*s));
    s->state = journal_open ? STN_FLASH_RECOVERY : STN_IDLE;
    s->paused = journal_open;   /* the receiver belongs to the flash session */
    s->listen_until_ms = STN_NEVER;
    s->identify_ms = STN_NEVER;
    s->check_ms = STN_NEVER;
    s->apply_retry_ms = STN_NEVER;
    s->last_resort_ms = STN_NEVER;
    s->episode_ms = STN_NEVER;
    s->detect_ms = STN_NEVER;
    s->mode_poll_ms = STN_NEVER;
    s->lock_retry_ms = STN_NEVER;
    s->pos_check_ms = STN_NEVER;
}

void stn_start(stn_t *s, const stn_in_t *in)
{
    s->started = true;
    if (s->state != STN_IDLE) {
        return;
    }
    s->listen_until_ms = in->now_ms + STN_LISTEN_MS;
    take_base(s, in);
}

void stn_journal_cleared(stn_t *s, const stn_in_t *in)
{
    if (s->state != STN_FLASH_RECOVERY) {
        return;
    }
    s->state = STN_IDLE;
    s->paused = false;
    stn_start(s, in);
}

/* ---- next job ------------------------------------------------------------- */

static stn_job_t ladder_job(stn_t *s, stn_rec_t a)
{
    switch (a) {
    case STN_REC_RESYNC:
        s->state = STN_RECOVERING;
        return make_job(STN_JOB_RESYNC);
    case STN_REC_LINK_RESTART:
        s->state = STN_RECOVERING;
        return make_job(STN_JOB_LINK_RESTART);
    case STN_REC_RESCAN: {
        s->state = STN_RECOVERING;
        s->paused = true;
        s->obs_on = false;
        stn_job_t j = make_job(STN_JOB_DETECT);
        j.full = true;
        j.break_first = true;
        j.loader_key = !s->loader_key_sent;
        return j;
    }
    case STN_REC_RECONFIGURE:
        return start_apply(s, STN_WHY_LADDER, STN_CHANGE_NONE, false);
    case STN_REC_RECEIVER_RESET:
    default:
        s->state = STN_RECOVERING;
        s->paused = true;
        s->obs_on = false;
        return make_job(STN_JOB_RECEIVER_RESET);
    }
}

static void restart_handling(stn_t *s, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    s->restart_seen = false;
    s->identify_ms = now;
    s->identify_restart = true;
    s->identify_leased = false;
    /* The applied check runs again once the receiver has settled. */
    s->obs_on = false;
    s->check_ms = now + STN_RESTART_CHECK_MS;
    s->check_pending = false;
    s->verify_pending = false;
    s->drift_windows = 0;
    s->drift_blocked = false;
    if (in->profile_survey) {
        um98x_survey_start(&s->survey, now, in->survey_s);   /* the receiver surveys again */
        s->mode_poll_ms = now + STN_MODE_POLL_MS;
        s->lock_done = false;
        s->antenna_check = false;
    } else {
        s->antenna_check = true;
    }
}

/* Window verdict: applied check, post-apply check or drift watch. */
static void judge_window(stn_t *s, const stn_in_t *in)
{
    if (!s->obs_on || s->obs_rearm || in->obs == NULL) {
        return;
    }
    int64_t now = in->now_ms;
    stn_obs_opts_t o = { .rtcm1230_ok = in->rtcm1230_ok, .surveying = stn_surveying(s, now) };
    uint16_t bad = 0;
    stn_obs_verdict_t v = stn_obs_eval(in->obs, &o, now, &bad);
    if (v == STN_OBS_PENDING) {
        return;
    }
    s->obs_rearm = true;   /* the next window starts now */
    if (!s->identity_ok || !s->supported) {
        return;   /* nothing to judge against yet; a pending check waits for the identity */
    }
    bool check = s->check_pending;
    bool verify = s->verify_pending;
    s->check_pending = false;
    s->verify_pending = false;
    s->drift_msg = bad;

    switch (v) {
    case STN_OBS_PASS:
        s->drift_windows = 0;
        s->drift_blocked = false;
        s->profile_applied = s->static_ok;
        s->antenna_check = false;
        s->alerts &= ~STN_ALERT_ANTENNA_MOVED;
        break;
    case STN_OBS_NO_RTCM:
        if (s->antenna_check) {
            s->antenna_check = false;
            s->antenna_probe = true;   /* a fixed base that sends nothing: moved antenna? */
            s->profile_applied = false;
        } else if (verify) {
            s->profile_applied = false;
            s->drift_blocked = true;
        } else if (check) {
            s->profile_applied = false;
            if (auto_apply_allowed(s, in)) {
                want_apply(s, STN_WHY_CHECK, STN_CHANGE_NONE, false);
            }
        }
        /* Otherwise a stall: the health ladder's business, not drift. */
        break;
    case STN_OBS_DRIFT:
    default:
        s->antenna_check = false;
        if (verify) {
            s->profile_applied = false;
            s->drift_blocked = true;   /* re-applying the same profile would not help */
        } else if (check) {
            s->profile_applied = false;
            if (auto_apply_allowed(s, in)) {
                want_apply(s, STN_WHY_CHECK, STN_CHANGE_NONE, false);
            }
        } else if (++s->drift_windows >= STN_DRIFT_WINDOWS) {
            s->drift_windows = 0;
            s->profile_applied = false;
            if (auto_apply_allowed(s, in) && !s->drift_blocked) {
                want_apply(s, STN_WHY_DRIFT, STN_CHANGE_NONE, false);
            }
        }
        break;
    }
}

static stn_job_t next_streaming(stn_t *s, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    stn_job_t none = make_job(STN_JOB_NONE);

    if (s->rec_pending != 0) {
        stn_rec_t a = (stn_rec_t)(s->rec_pending - 1);
        s->rec_pending = 0;
        if (a == STN_REC_RECONFIGURE && stn_reconfigure_refused(s, in->autocfg)) {
            /* became refused while queued: nothing is sent */
        } else {
            s->rec_running = (uint8_t)(a + 1);
            return ladder_job(s, a);
        }
    }

    if (s->restart_seen) {
        restart_handling(s, in);
    }

    if (s->identify_ms != STN_NEVER && now >= s->identify_ms) {
        stn_job_t j = make_job(STN_JOB_IDENTIFY);
        j.after_restart = s->identify_restart;
        j.leased = s->identify_leased;
        return j;
    }

    if (s->apply_wanted) {
        if (s->identity_ok && !s->supported) {
            s->apply_wanted = false;   /* not a model we configure: nothing to apply */
        } else {
            return start_apply(s, (stn_why_t)s->apply_why, (stn_change_t)s->apply_change, s->apply_force);
        }
    }

    if (config_failed(s) && s->apply_retry_ms != STN_NEVER && now >= s->apply_retry_ms) {
        if (in->autocfg && (!s->identity_ok || s->supported)) {
            return start_apply(s, STN_WHY_RETRY, STN_CHANGE_NONE, false);
        }
        /* The user configures the receiver meanwhile: look again later. */
        s->apply_retry_ms = now + stn_apply_backoff_ms(s->apply_failures);
    }

    /* A finished survey is locked (receiver-side fixed position). */
    if (s->survey.active && in->profile_survey && !s->lock_done && um98x_survey_done(&s->survey, now) &&
        auto_apply_allowed(s, in) && (s->lock_retry_ms == STN_NEVER || now >= s->lock_retry_ms)) {
        return start_apply(s, STN_WHY_LOCK, STN_CHANGE_LOCK, true);
    }

    if (s->mode_probe) {
        s->mode_probe = false;
        s->mode_probe_done = true;
        return mode_job(STN_MODE_BOOT);
    }
    if (s->antenna_probe) {
        s->antenna_probe = false;
        return mode_job(STN_MODE_ANTENNA);
    }
    if (stn_surveying(s, now) && (s->mode_poll_ms == STN_NEVER || now >= s->mode_poll_ms)) {
        s->mode_poll_ms = now + STN_MODE_POLL_MS;
        return mode_job(STN_MODE_POLL);
    }

    if (s->pos_check_ms != STN_NEVER && now >= s->pos_check_ms) {
        return make_job(STN_JOB_CHECK_POSITION);
    }

    if (s->check_ms != STN_NEVER && now >= s->check_ms) {
        s->check_ms = STN_NEVER;
        s->obs_on = true;
        s->obs_rearm = true;
        s->check_pending = true;
        s->verify_pending = false;
    }

    judge_window(s, in);
    if (s->apply_wanted) {
        return start_apply(s, (stn_why_t)s->apply_why, (stn_change_t)s->apply_change, s->apply_force);
    }
    return none;
}

/* CONFIGURING / RECOVERING between jobs: only the last resort waits here. */
static stn_job_t next_session(stn_t *s, const stn_in_t *in)
{
    stn_job_t none = make_job(STN_JOB_NONE);
    if (!s->last_resort) {
        back_to_streaming(s, in);
        return next_streaming(s, in);
    }
    if (rtcm_since_base(s, in)) {
        /* RTCM is back: stream, CONFIG_FAILED stays raised (its retry runs). */
        back_to_streaming(s, in);
        return next_streaming(s, in);
    }
    if (in->now_ms < s->last_resort_ms) {
        return none;
    }
    s->last_resort = false;
    if (s->last_resort_answered) {
        return make_job(STN_JOB_RESTORE);
    }
    /* The receiver answers nothing: look for it. */
    s->paused = false;
    enter_detecting(s, in);
    return none;
}

static stn_job_t next_detect(stn_t *s, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    stn_job_t none = make_job(STN_JOB_NONE);
    if (s->state == STN_NO_RECEIVER && data_since_base(s, in)) {
        /* Valid data at the stored baud between passes: it streams again. */
        enter_streaming(s, in, ENTER_CHECK);
        return next_streaming(s, in);
    }
    if (s->user_restart) {
        s->user_restart = false;
        return make_job(STN_JOB_LEAVE_LOADER);
    }
    if (s->detect_ms == STN_NEVER || now < s->detect_ms) {
        return none;
    }
    stn_job_t j = make_job(STN_JOB_DETECT);
    j.full = s->short_fails >= 2 || (s->episode_ms != STN_NEVER && now - s->episode_ms >= STN_DETECT_FAST_SPAN_MS);
    j.listen_only = s->listen_only;
    j.loader_key = !s->listen_only && !s->loader_key_sent;
    return j;
}

stn_job_t stn_next(stn_t *s, const stn_in_t *in)
{
    stn_job_t none = make_job(STN_JOB_NONE);
    if (s->job_busy || !s->started) {
        return none;
    }
    if (s->state == STN_IDLE) {
        if (data_since_base(s, in)) {
            enter_streaming(s, in, ENTER_CHECK);
        } else if (in->now_ms >= s->listen_until_ms) {
            enter_detecting(s, in);
        } else {
            return none;
        }
    }

    stn_job_t j = none;
    switch (s->state) {
    case STN_STREAMING:
        j = next_streaming(s, in);
        break;
    case STN_CONFIGURING:
    case STN_RECOVERING:
        j = next_session(s, in);
        break;
    case STN_DETECTING:
    case STN_NO_RECEIVER:
        j = next_detect(s, in);
        break;
    default:
        break;   /* FLASH_RECOVERY, FLASHING: the flash session decides */
    }
    if (j.kind != STN_JOB_NONE) {
        s->job = j;
        s->job_busy = true;
    }
    return j;
}

/* ---- job outcomes --------------------------------------------------------- */

static void done_identify(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    s->identify_restart = false;
    if (r->err == STN_OK) {
        set_identity(s, r->supported, r->static_ok);
        after_identity(s, in);
        return;
    }
    s->identity_ok = false;   /* the stored identity stays unconfirmed */
    if (s->job.leased) {
        s->leased_tried = true;
    }
    if (!s->job.leased && !in->model_known && !s->leased_tried) {
        /* No model was ever stored: this once, a lease to learn it. */
        s->identify_leased = true;
        s->identify_ms = now;
    } else {
        s->identify_leased = false;
        s->identify_ms = now + STN_IDENTITY_RETRY_MS;
    }
}

static void done_mode(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (r->err != STN_OK) {
        return;   /* probes give up; the survey poll asks again in 60 s */
    }
    switch (s->job.mode_why) {
    case STN_MODE_BOOT:
        if (!in->profile_survey || s->survey.active) {
            break;
        }
        if (r->mode_survey) {
            um98x_survey_start(&s->survey, now, in->survey_s);
            s->mode_poll_ms = now + STN_MODE_POLL_MS;
            s->lock_done = false;
        } else if (r->mode_fixed) {
            /* The survey finished before this boot: lock it. */
            um98x_survey_start(&s->survey, now, in->survey_s);
            s->survey.complete = true;
            s->lock_done = false;
        }
        break;
    case STN_MODE_ANTENNA:
        if (r->mode_fixed) {
            s->alerts |= STN_ALERT_ANTENNA_MOVED;   /* the user re-surveys; never automatic */
        }
        break;
    case STN_MODE_POLL:
    default:
        if (r->mode_fixed) {
            s->survey.complete = true;
        }
        break;
    }
}

static void done_detect(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    bool rescan = (s->state == STN_RECOVERING);
    if (rescan) {
        s->rec_running = 0;
        s->paused = false;
    }
    if (r->err == STN_OK) {
        if (r->identity) {
            set_identity(s, r->supported, r->static_ok);
        } else {
            s->identify_ms = now;   /* found by listening: who is it? */
            s->identify_restart = false;
        }
        enter_streaming(s, in, ENTER_CHECK);
        if (r->identity) {
            after_identity(s, in);
        }
        return;
    }
    s->detect_saw_bytes = r->saw_bytes || r->prompt;
    if (r->key_sent) {
        s->loader_key_sent = true;
    }
    if (s->episode_ms == STN_NEVER || rescan) {
        s->episode_ms = now;
    }
    if (r->prompt && r->app_back) {
        /* Back in its application: look again right away. */
        s->state = STN_DETECTING;
        s->detect_ms = now;
        take_base(s, in);
        return;
    }
    if (s->detect_failures < UINT32_MAX) {
        s->detect_failures++;
    }
    if (r->prompt) {
        s->alerts |= STN_ALERT_RECEIVER_IN_LOADER;
        s->listen_only = true;
    }
    if (s->job.full) {
        s->short_fails = 0;
        } else if (s->short_fails < UINT8_MAX) {
        s->short_fails++;
    }
    drop_stale_requests(s);
    s->state = STN_NO_RECEIVER;
    s->profile_applied = false;
    s->obs_on = false;
    s->detect_ms = now + ((now - s->episode_ms < STN_DETECT_FAST_SPAN_MS) ? STN_DETECT_FAST_MS : STN_DETECT_SLOW_MS);
    take_base(s, in);
}

static void done_leave_loader(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    if (r->err == STN_OK) {
        s->alerts &= ~STN_ALERT_RECEIVER_IN_LOADER;
        s->listen_only = false;
    }
    /* Then detection (listen-only again if it did not come back). */
    s->detect_ms = in->now_ms;
    take_base(s, in);
}

static void done_apply(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (s->job.why == STN_WHY_LADDER) {
        s->rec_running = 0;
    }
    if (r->err == STN_OK) {
        s->alerts &= ~(STN_ALERT_CONFIG_FAILED | STN_ALERT_UNSUPPORTED_MODEL);
        s->apply_failures = 0;
        s->apply_retry_ms = STN_NEVER;
        s->identity_ok = true;
        s->supported = true;
        s->static_ok = true;
        s->identify_ms = STN_NEVER;
        s->drift_blocked = false;
        if (s->job.change == STN_CHANGE_LOCK) {
            s->lock_done = true;
            s->pos_check_ms = now + STN_POS_CHECK_MS;
            s->pos_check_tries = 0;
            s->alerts &= ~(STN_ALERT_ANTENNA_MOVED | STN_ALERT_POSITION_MISMATCH);
        } else if (s->job.change == STN_CHANGE_RESURVEY) {
            s->pos_check_ms = STN_NEVER;
            s->alerts &= ~(STN_ALERT_ANTENNA_MOVED | STN_ALERT_POSITION_MISMATCH);
        }
        if (!in->profile_survey) {
            memset(&s->survey, 0, sizeof(s->survey));
        } else if (r->mode_sent) {
            um98x_survey_start(&s->survey, r->mode_ack_ms != 0 ? r->mode_ack_ms : now, in->survey_s);
            s->mode_poll_ms = now + STN_MODE_POLL_MS;
            s->lock_done = false;
        }
        s->paused = false;
        s->last_resort = false;
        enter_streaming(s, in, ENTER_VERIFY);
        return;
    }

    if (r->nothing_sent) {
        if (r->err == STN_E_UNSUPPORTED) {
            set_identity(s, false, false);
        } else if (s->job.change == STN_CHANGE_LOCK && r->err == STN_E_NOT_FOUND) {
            s->lock_retry_ms = now + STN_LOCK_RETRY_MS;   /* no 1005 yet */
        } else {
            raise_config_failed(s, now);
        }
        back_to_streaming(s, in);
        return;
    }

    raise_config_failed(s, now);
    if (r->receiver_lost) {
        s->paused = false;
        enter_detecting(s, in);
        return;
    }
    if (r->err == STN_E_STATE) {
        back_to_streaming(s, in);   /* no lease or link: nothing reached the receiver */
        return;
    }
    /* Last resort: the receiver must not stay silenced. */
    s->state = s->session_state;
    s->last_resort = true;
    s->last_resort_ms = now + STN_LAST_RESORT_MS;
    s->last_resort_answered = r->answered;
    take_base(s, in);
}

static void done_position(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    switch (r->err) {
    case STN_OK:
        s->alerts &= ~STN_ALERT_POSITION_MISMATCH;
        s->pos_check_ms = STN_NEVER;
        break;
    case STN_E_MISMATCH:
        s->alerts |= STN_ALERT_POSITION_MISMATCH;
        s->pos_check_ms = STN_NEVER;
        break;
    default:
        s->pos_check_ms = (++s->pos_check_tries < STN_POS_CHECK_TRIES) ? in->now_ms + STN_POS_CHECK_MS : STN_NEVER;
        break;
    }
}

void stn_done(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    if (!s->job_busy) {
        return;
    }
    s->job_busy = false;
    switch (s->job.kind) {
    case STN_JOB_IDENTIFY:
        done_identify(s, r, in);
        break;
    case STN_JOB_MODE:
        done_mode(s, r, in);
        break;
    case STN_JOB_DETECT:
        done_detect(s, r, in);
        break;
    case STN_JOB_LEAVE_LOADER:
        done_leave_loader(s, r, in);
        break;
    case STN_JOB_APPLY:
        done_apply(s, r, in);
        break;
    case STN_JOB_RESTORE:
        /* Whatever it achieved: stream; CONFIG_FAILED stays and its retry runs. */
        back_to_streaming(s, in);
        break;
    case STN_JOB_RESYNC:
    case STN_JOB_LINK_RESTART:
        s->rec_running = 0;
        back_to_streaming(s, in);
        break;
    case STN_JOB_RECEIVER_RESET:
        s->rec_running = 0;
        back_to_streaming(s, in);
        s->restart_seen = true;   /* identity, applied check and survey as after any restart */
        break;
    case STN_JOB_CHECK_POSITION:
        done_position(s, r, in);
        break;
    default:
        break;
    }
    memset(&s->job, 0, sizeof(s->job));
}

/* ---- requests --------------------------------------------------------------- */

void stn_request_apply(stn_t *s)
{
    want_apply(s, STN_WHY_USER, STN_CHANGE_NONE, false);
}

void stn_request_lock(stn_t *s)
{
    want_apply(s, STN_WHY_LOCK, STN_CHANGE_LOCK, true);
}

void stn_request_resurvey(stn_t *s)
{
    want_apply(s, STN_WHY_RESURVEY, STN_CHANGE_RESURVEY, true);
}

bool stn_reconfigure_refused(const stn_t *s, bool autocfg)
{
    return !autocfg || config_failed(s);
}

int stn_request_recovery(stn_t *s, uint8_t action, bool autocfg)
{
    if (action >= STN_REC_COUNT) {
        return STN_E_STATE;
    }
    if (action == STN_REC_RECONFIGURE && stn_reconfigure_refused(s, autocfg)) {
        return STN_E_FAIL;
    }
    if (s->state != STN_STREAMING && s->state != STN_CONFIGURING && s->state != STN_RECOVERING) {
        return STN_E_STATE;
    }
    uint8_t want = (uint8_t)(action + 1);
    if (s->rec_running >= want || s->rec_pending >= want) {
        return STN_OK;   /* an equal or stronger step is queued or running */
    }
    s->rec_pending = want;
    return STN_OK;
}

int stn_request_restart_receiver(stn_t *s)
{
    if ((s->alerts & STN_ALERT_RECEIVER_IN_LOADER) == 0) {
        return STN_E_STATE;
    }
    s->user_restart = true;
    return STN_OK;
}

void stn_restart_seen(stn_t *s)
{
    s->restart_seen = true;
}

void stn_on_arp(stn_t *s, const int64_t ecef_01mm[3])
{
    um98x_survey_on_arp(&s->survey, ecef_01mm);
}

/* ---- status ----------------------------------------------------------------- */

bool stn_surveying(const stn_t *s, int64_t now_ms)
{
    return um98x_survey_window_open(&s->survey, now_ms);
}

uint32_t stn_survey_left_s(const stn_t *s, int64_t now_ms)
{
    return um98x_survey_window_left_s(&s->survey, now_ms);
}

uint32_t stn_config_retry_in_s(const stn_t *s, int64_t now_ms)
{
    if (!config_failed(s) || s->apply_retry_ms == STN_NEVER) {
        return 0;
    }
    int64_t left = s->apply_retry_ms - now_ms;
    if (left <= 0) {
        return 0;
    }
    int64_t sec = (left + 999) / 1000;
    return sec > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)sec;
}
