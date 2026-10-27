/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "station_core.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "settings_codec.h"
#include "um98x_profile.h"

bool stn_flash_lockout_reserve(stn_flash_lockout_t *lockout, bool busy)
{
    if (busy || lockout->request_reserved) {
        return false;
    }
    lockout->request_reserved = true;
    return true;
}

int stn_flash_lockout_take(stn_flash_lockout_t *lockout, const stn_flash_lockout_ops_t *ops)
{
    bool claimed_now = false;
    if (!lockout->update_claimed) {
        int err = ops->claim_update(ops->ctx);
        if (err != 0) {
            return err;
        }
        lockout->update_claimed = true;
        claimed_now = true;
    }
    if (!lockout->inhibit_held) {
        int token;
        int err = ops->acquire_inhibit(ops->ctx, &token);
        if (err != 0) {
            if (claimed_now) {
                lockout->update_claimed = false;
                ops->release_update(ops->ctx);
            }
            return err;
        }
        lockout->inhibit = token;
        lockout->inhibit_held = true;
    }
    return 0;
}

void stn_flash_lockout_release(stn_flash_lockout_t *lockout, const stn_flash_lockout_ops_t *ops)
{
    if (lockout->update_claimed) {
        lockout->update_claimed = false;
        ops->release_update(ops->ctx);
    }
    if (lockout->inhibit_held) {
        lockout->inhibit_held = false;
        ops->release_inhibit(ops->ctx, lockout->inhibit);
    }
}

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

static void note(stn_t *s, stn_note_kind_t kind, int64_t a, int32_t b)
{
    if (s->note_n >= STN_NOTES) {
        /* The caller fell behind: the oldest note gives way. */
        memmove(&s->notes[0], &s->notes[1], sizeof(s->notes[0]) * (STN_NOTES - 1u));
        s->note_n = STN_NOTES - 1u;
    }
    stn_note_t *n = &s->notes[s->note_n++];
    n->kind = (uint8_t)kind;
    n->a = (a > INT32_MAX) ? INT32_MAX : (a < INT32_MIN ? INT32_MIN : (int32_t)a);
    n->b = b;
}

/* The user's query will not run: the caller says so. */
static void query_drop(stn_t *s)
{
    if (s->query_want == 0) {
        return;
    }
    note(s, STN_NOTE_QUERY_DROPPED, 0, (int32_t)((uint32_t)(s->query_want - 1u) | ((uint32_t)s->query_seq << 8)));
    s->query_want = 0;
}

static uint32_t secs(int64_t ms)
{
    if (ms <= 0) {
        return 0;
    }
    int64_t sec = (ms + 999) / 1000;
    return sec > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)sec;
}

/* The receiver was reconfigured, reset or restarted: satellite-in-view
 * output we turned on (never saved) is gone. A window that had it on does
 * not turn it on again by itself. */
static void sky_receiver_changed(stn_t *s)
{
    if (!s->sky_on) {
        return;
    }
    s->sky_on = false;
    s->sky_unsure = false;
    s->sky_off_tries = 0;
    s->sky_retry_ms = STN_NEVER;
    if (s->live_open && s->sky_want) {
        s->sky_want = false;
        s->sky_why = STN_SKY_WHY_RECONFIGURED;
    }
}

/* A receiver update owns the receiver: its session, a journal waiting in
 * FLASH_RECOVERY, or the re-apply and check before the journal closes. */
static bool flash_state(const stn_t *s)
{
    return s->state == STN_FLASHING || s->state == STN_FLASH_RECOVERY || s->flash_close != 0;
}

/* Close the journal now (the caller does it on the note). */
static void flash_close_note(stn_t *s, bool ok)
{
    s->flash_close = 0;
    s->flash_close_ms = STN_NEVER;
    s->flash_rtcm_ms = STN_NEVER;
    if (ok) {
        s->alerts &= ~STN_ALERT_RECEIVER_ATTENTION;
    } else {
        s->alerts |= STN_ALERT_RECEIVER_ATTENTION;   /* the station profile did not come back: the user looks */
    }
    note(s, STN_NOTE_FLASH_CLOSE, ok ? 1 : 0, 0);
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
    if (s->relocating && in->fixed_valid && auto_apply_allowed(s, in)) {
        want_apply(s, STN_WHY_RESURVEY, STN_CHANGE_RESURVEY, true);
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
    s->rtcm_differs = false;
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

/* ---- fixed-position check (see the header; the rule is um98x_fixcheck.h) ---- */

/* Metres for a note, rounded half away from zero, clamped. */
static int32_t round_m(double m)
{
    if (!(m < 2.0e9)) {
        return m > 0.0 ? INT32_MAX : 0;   /* NaN as 0 */
    }
    if (!(m > -2.0e9)) {
        return INT32_MIN;
    }
    return (int32_t)(m < 0.0 ? m - 0.5 : m + 0.5);
}

/* A new check STN_FIX_FIRST_MS from now (the receiver may need a fix first);
 * the verdict stays until that check decides. */
static void fix_rearm(stn_t *s, int64_t now)
{
    if (!s->fix_on) {
        return;
    }
    s->fix_due_ms = now + (s->fix_continuous ? STN_FIX_PERIOD_MS : STN_FIX_FIRST_MS);
    s->fix_running = false;
    s->move_samples = 0;
    s->fix_sppnav = false;
}

/* No check any more: verdict, alert and schedule go (noted when the alert
 * stood and the fixed position changed or went). */
static void fix_drop(stn_t *s, bool changed)
{
    if ((s->alerts & STN_ALERT_FIXED_FAR) != 0) {
        s->alerts &= ~STN_ALERT_FIXED_FAR;
        if (changed) {
            note(s, STN_NOTE_FIXED_CHANGED, 0, 0);
        }
    }
    s->fix_on = false;
    s->move_samples = 0;
    s->fix_due_ms = STN_NEVER;
    s->fix_running = false;
    s->fix_tries = 0;
    s->fix_sppnav = false;
    s->fix_usable_seen = false;
    s->fix_end = STN_FIX_END_NONE;
    s->fix_sample_ms = STN_NEVER;
    um98x_fixchk_init(&s->fix);
}

/* Every step: the fixed position the caller passes (none for a survey profile). */
static void fix_track(stn_t *s, const stn_in_t *in)
{
    bool enabled = !s->fix_continuous && in->autocfg;
    if (s->fix_continuous != in->autocfg) s->move_samples = 0;
    s->fix_continuous = in->autocfg;
    if (enabled && s->fix_on && !s->fix_running && s->fix_due_ms > in->now_ms + STN_FIX_PERIOD_MS) {
        s->fix_due_ms = in->now_ms + STN_FIX_PERIOD_MS;
        s->move_samples = 0;
    }
    if ((!in->fixed_valid && !(in->autocfg && in->profile_survey)) || (s->identity_ok && !s->supported)) {
        if (s->fix_on) {
            fix_drop(s, !in->fixed_valid);
        }
        return;
    }
    if (s->fix_on && (s->fix_reference != in->fixed_valid || (in->fixed_valid &&
                     (in->fixed_llh[0] != s->fix_ref[0] || in->fixed_llh[1] != s->fix_ref[1] ||
                      in->fixed_llh[2] != s->fix_ref[2])))) {
        fix_drop(s, true);
    }
    if (!s->fix_on) {
        s->fix_on = true;
        s->fix_reference = in->fixed_valid;
        memcpy(s->fix_ref, in->fixed_llh, sizeof(s->fix_ref));
        fix_rearm(s, in->now_ms);
    }
}

/* The next query when one is due (STREAMING with nothing more urgent). */
static bool fix_job(stn_t *s, const stn_in_t *in, stn_job_t *out)
{
    if (!s->fix_on || !s->identity_ok || !s->supported || s->fix_due_ms == STN_NEVER || in->now_ms < s->fix_due_ms) {
        return false;
    }
    if (!s->fix_running) {
        s->fix_running = true;
        s->fix_tries = 0;
        s->fix_usable_seen = false;
        um98x_fixchk_restart(&s->fix);
    }
    *out = make_job(STN_JOB_OWN_FIX);
    out->sppnav = !s->fix_reference || s->fix_sppnav;
    return true;
}

static void fix_finish(stn_t *s, int64_t now, stn_fix_end_t end)
{
    s->fix_running = false;
    s->fix_end = (uint8_t)end;
    s->fix_due_ms = now + (s->fix_continuous ? STN_FIX_PERIOD_MS : STN_FIX_RECHECK_MS);
}

/* A decision (before: the verdict that stood): the alert follows it, noted
 * when the warning or its reason changes. */
static void fix_decided(stn_t *s, uint8_t before, uint8_t verdict)
{
    bool was = (s->alerts & STN_ALERT_FIXED_FAR) != 0;
    const um98x_fix_offset_t *o = &s->fix.shown;
    if (verdict == UM98X_FIX_FAR || verdict == UM98X_FIX_SEA_LEVEL) {
        s->alerts |= STN_ALERT_FIXED_FAR;
        if (!was || verdict != before) {
            note(s, verdict == UM98X_FIX_SEA_LEVEL ? STN_NOTE_FIXED_SEA_LEVEL : STN_NOTE_FIXED_FAR,
                 round_m(o->horizontal_m), round_m(o->up_m));
        }
    } else {
        s->alerts &= ~STN_ALERT_FIXED_FAR;
        if (was) {
            note(s, STN_NOTE_FIXED_NEAR, round_m(o->horizontal_m), round_m(o->up_m));
        }
    }
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
    s->no_rx_ms = STN_NEVER;
    s->mode_poll_ms = STN_NEVER;
    s->lock_retry_ms = STN_NEVER;
    s->pos_check_ms = STN_NEVER;
    s->maint_until_ms = STN_NEVER;
    s->live_idle_ms = STN_LIVE_IDLE_MS;
    s->live_max_ms = STN_LIVE_MAX_MS;
    s->live_until_ms = STN_NEVER;
    s->sky_retry_ms = STN_NEVER;
    s->flash_close_ms = STN_NEVER;
    s->flash_rtcm_ms = STN_NEVER;
    s->rtcm_change_ms = STN_NEVER;
    s->fix_due_ms = STN_NEVER;
    s->fix_sample_ms = STN_NEVER;
    s->line_change_ms = STN_NEVER;
    s->ask_ms = STN_NEVER;
    um98x_fixchk_init(&s->fix);
}

/* The time of the latest valid RTCM frame, and of the latest text line, as
 * far as the samples show them. */
static void track_rtcm(stn_t *s, const stn_in_t *in)
{
    if (in->rtcm_frames != s->rtcm_seen) {
        s->rtcm_seen = in->rtcm_frames;
        s->rtcm_change_ms = in->now_ms;
    }
    if (in->text_lines != s->lines_seen) {
        s->lines_seen = in->text_lines;
        s->line_change_ms = in->now_ms;
    }
}

/* A valid RTCM frame arrived in the last STN_RTCM_QUIET_MS. */
static bool rtcm_flowing(const stn_t *s, int64_t now)
{
    return s->rtcm_change_ms != STN_NEVER && now - s->rtcm_change_ms < STN_RTCM_QUIET_MS;
}

/* `at` (STN_NEVER = never) came after the last RTCM frame as the samples
 * show it (any time with none this boot). */
static bool since_rtcm(const stn_t *s, int64_t at)
{
    return at != STN_NEVER && (s->rtcm_change_ms == STN_NEVER || at > s->rtcm_change_ms);
}

/* The question to a receiver that sends no RTCM (none this boot, or none
 * for STN_RTCM_QUIET_MS; see the header): at once when nothing came since
 * its last RTCM frame, then while no line comes for STN_ASK_MS, at most
 * once every STN_ASK_MS. */
static bool ask_due(const stn_t *s, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (rtcm_flowing(s, now)) {
        return false;
    }
    if (since_rtcm(s, s->line_change_ms) && now - s->line_change_ms < STN_ASK_MS) {
        return false;   /* it sent a line since: it answers */
    }
    return !since_rtcm(s, s->ask_ms) || now - s->ask_ms >= STN_ASK_MS;
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
    s->flash_want = 0;   /* a retry, restart or recovery queued for the cleared journal */
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
        j.loader_key = !s->loader_key_sent && !s->first_apply_held;
        return j;
    }
    case STN_REC_RECONFIGURE:
        if (s->apply_wanted) {
            /* A queued apply (the user's lock or re-survey, a pending check)
             * rides along in the ladder's session instead of being dropped. */
            return start_apply(s, STN_WHY_LADDER, (stn_change_t)s->apply_change, s->apply_force);
        }
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
    s->rtcm_differs = false;
    if (in->profile_survey) {
        um98x_survey_start(&s->survey, now, in->survey_s);   /* the receiver surveys again */
        s->mode_poll_ms = now + STN_MODE_POLL_MS;
        s->lock_done = false;
        s->antenna_check = false;
    } else {
        s->antenna_check = true;
    }
    fix_rearm(s, now);   /* the receiver computes its solution afresh */
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
    if (verify && s->flash_close == 1) {
        flash_close_note(s, v == STN_OBS_PASS);   /* the post-update re-apply was checked */
    }

    /* Drift needs RTCM that flows and differs; a window without any is a
     * stall (no antenna, a silent receiver), which other alerts explain. */
    s->rtcm_differs = (v == STN_OBS_DRIFT);

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
            s->check_pending = check;  /* judged by a window with RTCM (below) */
        } else if (verify) {
            s->profile_applied = false;
            s->drift_blocked = true;
        } else if (check) {
            s->profile_applied = false;
            if (s->static_ok && !in->ladder_alerts_only) {
                /* The receiver acknowledged this profile on this firmware
                 * (the applied state): a window without RTCM says nothing
                 * about what it runs, and a re-apply could not be checked
                 * without RTCM. The check waits for a window with RTCM; the
                 * stall is the health ladder's (its reconfigure step). */
                s->check_pending = true;
            } else if (auto_apply_allowed(s, in)) {
                /* Never acknowledged; or acknowledged, but the ladder only
                 * alerts and re-applies nothing: once, as its step would
                 * (the next window checks the apply, without repeating it). */
                want_apply(s, STN_WHY_CHECK, STN_CHANGE_NONE, false);
            } else if (s->static_ok) {
                s->check_pending = true;   /* nothing applies: the check still waits for RTCM */
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

/* Satellite-in-view output for the live view: on while the window wants it,
 * off once it does not. Only from STREAMING with nothing more urgent due. */
static bool sky_job(stn_t *s, const stn_in_t *in, stn_job_t *out)
{
    if (in->profile_sky) {
        /* The profile sends it permanently: never toggled here, and what we
         * may have turned on is the profile's output now. */
        s->sky_on = false;
        s->sky_unsure = false;
        s->sky_off_tries = 0;
        s->sky_retry_ms = STN_NEVER;
        return false;
    }
    bool want = s->live_open && s->sky_want;
    bool on;
    if (want && (!s->sky_on || s->sky_unsure)) {
        if (!in->autocfg || !s->identity_ok || !s->supported) {
            return false;   /* the user configures the receiver, or not a model we configure (status says why) */
        }
        on = true;
    } else if (!want && s->sky_on) {
        if (s->sky_unsure && s->live_open) {
            return false;   /* "on" got no answer: it is turned off when the window closes */
        }
        if (s->sky_retry_ms != STN_NEVER && in->now_ms < s->sky_retry_ms) {
            return false;
        }
        on = false;
    } else {
        return false;
    }
    *out = make_job(STN_JOB_SKY);
    out->sky_on = on;
    s->paused = true;   /* the lease holds RTCM back: a short planned gap for the casters */
    return true;
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
        if (s->identify_leased && rtcm_flowing(s, now)) {
            s->identify_leased = false;   /* RTCM came back meanwhile: never hold it back for this */
        }
        /* A leased query runs only while nothing flows: the casters are not
         * paused for it (nothing to hold back; the outage stays unplanned). */
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
        if (stn_config_auto_retry(s, in->autocfg)) {
            return start_apply(s, STN_WHY_RETRY,
                s->relocating && !s->relocation_ready && in->fixed_valid && !s->survey.active ?
                STN_CHANGE_RESURVEY : STN_CHANGE_NONE, false);
        }
        /* The user configures the receiver meanwhile: look again later. */
        s->apply_retry_ms = now + stn_apply_backoff_ms(s->apply_failures);
    }

    /* A finished survey is locked (receiver-side fixed position). */
    if (s->survey.active && in->profile_survey && !s->lock_done && um98x_survey_done(&s->survey, now) &&
        auto_apply_allowed(s, in) && (s->lock_retry_ms == STN_NEVER || now >= s->lock_retry_ms)) {
        return start_apply(s, STN_WHY_LOCK, STN_CHANGE_LOCK, true);
    }

    stn_job_t sky;
    if (sky_job(s, in, &sky)) {
        return sky;
    }

    if (s->query_want != 0) {
        stn_job_t q = make_job(STN_JOB_QUERY);
        q.query = (uint8_t)(s->query_want - 1u);
        q.leased = s->query_leased;
        s->query_want = 0;
        if (q.leased) {
            s->paused = true;   /* the lease holds RTCM back: a short planned gap for the casters */
        }
        return q;
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
    stn_job_t fix;
    if (fix_job(s, in, &fix)) {
        return fix;
    }
    if (ask_due(s, in)) {
        s->ask_ms = now;
        return make_job(STN_JOB_ASK);
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
    /* Held: queries only; a receiver at its loader waits for the user's restart. */
    j.loader_key = !s->listen_only && !s->loader_key_sent && !s->first_apply_held;
    return j;
}

static void live_close(stn_t *s, int64_t now_ms, stn_live_end_t why)
{
    if (!s->live_open) {
        return;
    }
    s->live_open = false;
    s->sky_want = false;
    s->live_last_end = (uint8_t)why;
    s->live_until_ms = STN_NEVER;
    note(s, STN_NOTE_LIVE_CLOSED, secs(now_ms - s->live_opened_ms), (int32_t)why);
}

/* The live view's time limits, and a flash session taking the receiver. */
static void live_tick(stn_t *s, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (flash_state(s)) {
        live_close(s, now, STN_LIVE_END_FLASH);
        s->sky_on = false;   /* the flash restarts the receiver */
        s->sky_unsure = false;
        return;
    }
    if (!s->live_open) {
        return;
    }
    if (now >= s->live_until_ms) {
        live_close(s, now, STN_LIVE_END_EXPIRED);
    } else if (now - s->live_touch_ms >= (int64_t)s->live_idle_ms) {
        live_close(s, now, STN_LIVE_END_IDLE);
    }
}

/* The stream-first path, as at boot: listen, identity, the applied check.
 * restarted: the receiver may have restarted meanwhile. */
static void stream_first(stn_t *s, const stn_in_t *in, bool restarted)
{
    int64_t now = in->now_ms;
    s->state = STN_IDLE;
    s->paused = false;
    s->last_resort = false;
    s->identity_ok = false;
    s->identify_ms = STN_NEVER;
    s->identify_restart = false;
    s->identify_leased = false;
    s->restart_seen = false;
    s->check_ms = STN_NEVER;
    s->check_pending = false;
    s->verify_pending = false;
    s->obs_on = false;
    s->drift_windows = 0;
    s->drift_blocked = false;
    s->profile_applied = false;
    s->rtcm_differs = false;
    s->mode_probe = false;
    s->mode_probe_done = false;
    s->antenna_check = false;
    s->antenna_probe = false;
    s->detect_ms = STN_NEVER;
    s->episode_ms = STN_NEVER;
    if (restarted && in->profile_survey) {
        um98x_survey_start(&s->survey, now, in->survey_s);   /* the receiver surveys again */
        s->mode_poll_ms = now + STN_MODE_POLL_MS;
        s->lock_done = false;
    }
    s->listen_until_ms = now + STN_LISTEN_MS;
    take_base(s, in);
}

/* Maintenance ends (why: stn_maint_end_t): the stream-first path, as at boot. */
static void maint_exit(stn_t *s, const stn_in_t *in, stn_maint_end_t why)
{
    note(s, STN_NOTE_MAINT_OFF, secs(in->now_ms - s->maint_start_ms), (int32_t)why);
    bool restarted = s->maint_restart_seen;
    s->maint = false;
    s->maint_until_ms = STN_NEVER;
    s->maint_restart_seen = false;
    s->maint_resumed = false;
    s->maint_end_unix = 0;
    stream_first(s, in, restarted);
}

/* The re-apply after an update: close the journal once its check is in (or
 * the abort path's RTCM is back), at the latest after STN_FLASH_CLOSE_MAX_MS. */
static void flash_close_tick(stn_t *s, const stn_in_t *in)
{
    if (s->flash_close == 0) {
        return;
    }
    int64_t now = in->now_ms;
    if (now >= s->flash_close_ms) {
        flash_close_note(s, false);
        return;
    }
    if (s->flash_close == 2 && s->state == STN_STREAMING && s->flash_rtcm_ms != STN_NEVER) {
        if (rtcm_since_base(s, in)) {
            flash_close_note(s, true);
        } else if (now >= s->flash_rtcm_ms) {
            flash_close_note(s, false);
        }
    }
}

/* Receiver update, bench operation or package check, when one is queued and
 * the state allows it (they run before anything else of the state). */
static bool special_job(stn_t *s, const stn_in_t *in, stn_job_t *j)
{
    if (s->state == STN_FLASH_RECOVERY) {
        if (s->flash_want == STN_FLASH_RETRY || s->flash_want == STN_FLASH_RESTART || s->flash_want == STN_FLASH_RECOVER) {
            *j = make_job(STN_JOB_FLASH);
            j->flash_kind = s->flash_want;
            s->flash_want = 0;
            s->paused = true;
            return true;
        }
        return false;
    }
    if (s->state == STN_STREAMING && s->flash_want == STN_FLASH_NEW && s->flash_close == 0) {
        s->flash_want = 0;
        s->state = STN_FLASHING;
        s->paused = true;
        s->obs_on = false;
        live_tick(s, in);   /* the live view closes as the update begins */
        *j = make_job(STN_JOB_FLASH);
        j->flash_kind = STN_FLASH_NEW;
        return true;
    }
    if (s->state != STN_STREAMING && s->state != STN_NO_RECEIVER && s->state != STN_DETECTING) {
        return false;
    }
    if (s->bench_want) {
        s->bench_want = false;
        s->paused = true;
        s->obs_on = false;
        *j = make_job(STN_JOB_BENCH);
        return true;
    }
    if (s->check_want) {
        s->check_want = false;
        *j = make_job(STN_JOB_PKG_CHECK);
        return true;
    }
    return false;
}

stn_job_t stn_next(stn_t *s, const stn_in_t *in)
{
    stn_job_t none = make_job(STN_JOB_NONE);
    track_rtcm(s, in);
    if (s->job_busy || !s->started) {
        return none;
    }
    fix_track(s, in);
    live_tick(s, in);
    flash_close_tick(s, in);
    if (s->query_want != 0 && in->now_ms >= s->query_until_ms) {
        query_drop(s);
    }
    if (s->maint) {
        if (in->now_ms < s->maint_until_ms) {
            return none;   /* hands off: nothing is sent, nothing is decided */
        }
        maint_exit(s, in, STN_MAINT_END_TIME);
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
    if (special_job(s, in, &j)) {
        s->job = j;
        s->job_busy = true;
        return j;
    }
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
    if (!s->job.leased && !in->model_known && !s->leased_tried && !rtcm_flowing(s, now)) {
        /* No model was ever stored and nothing flows: this once per boot, a
         * lease to learn it. While RTCM flows the casters keep it, and the
         * query is retried without a lease. */
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
    if (s->detect_passes < UINT32_MAX) {
        s->detect_passes++;   /* with detect_saw_bytes below: health reads the two together */
    }
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
    s->no_rx_ms = now;
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

/* The receiver restarted within a session of ours (the signal-group reset,
 * the rollback's RESET): with a survey profile it surveys again. */
static void survey_restarted(stn_t *s, const stn_in_t *in)
{
    um98x_survey_start(&s->survey, in->now_ms, in->survey_s);
    s->mode_poll_ms = in->now_ms + STN_MODE_POLL_MS;
    s->lock_done = false;
}

static void done_apply(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (s->job.why == STN_WHY_LADDER) {
        s->rec_running = 0;
    }
    if (!r->nothing_sent) {
        sky_receiver_changed(s);   /* the session quieted the port (and saved, or rolled back) */
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
        s->rtcm_differs = false;   /* the apply's own check window judges again */
        if (s->job.change == STN_CHANGE_LOCK) {
            s->lock_done = true;
            s->pos_check_ms = now + STN_POS_CHECK_MS;
            s->pos_check_tries = 0;
            s->alerts &= ~(STN_ALERT_ANTENNA_MOVED | STN_ALERT_POSITION_MISMATCH);
        } else if (s->job.change == STN_CHANGE_RESURVEY) {
            s->pos_check_ms = STN_NEVER;
            s->alerts &= ~(STN_ALERT_ANTENNA_MOVED | STN_ALERT_POSITION_MISMATCH);
        }
        if (s->relocating && in->fixed_valid && (s->job.change == STN_CHANGE_LOCK || s->survey.active)) {
            s->relocation_ready = true;
            s->pos_check_ms = now + STN_POS_CHECK_MS;
            s->pos_check_tries = 0;
        }
        if (!in->profile_survey) {
            memset(&s->survey, 0, sizeof(s->survey));
        } else if (r->mode_sent) {
            um98x_survey_start(&s->survey, r->mode_ack_ms != 0 ? r->mode_ack_ms : now, in->survey_s);
            s->mode_poll_ms = now + STN_MODE_POLL_MS;
            s->lock_done = false;
        } else if (r->restarted) {
            survey_restarted(s, in);   /* the session's own receiver restart */
        }
        s->paused = false;
        s->last_resort = false;
        fix_rearm(s, now);   /* the profile may be new, the receiver restarted */
        enter_streaming(s, in, ENTER_VERIFY);
        if (s->job.why == STN_WHY_FLASH && s->flash_close == 2) {
            s->flash_rtcm_ms = now + STN_FLASH_RTCM_MS;   /* the abort path: RTCM must come back */
        }
        return;
    }

    if (s->job.why == STN_WHY_FLASH && s->flash_close != 0) {
        flash_close_note(s, false);   /* the re-apply after the update failed: closed, the user looks */
    }
    if (r->sg_back && !r->nothing_sent) {
        /* A refused signal group: the session rolled back, the profile holds
         * a group the receiver runs again. Re-apply that at once; no
         * CONFIG_FAILED and no backoff retry of the refused pair. */
        if (r->restarted && in->profile_survey) {
            survey_restarted(s, in);
        }
        if (r->receiver_lost) {
            s->paused = false;
            enter_detecting(s, in);
            return;
        }
        uint8_t why = s->job.why == STN_WHY_SG_TEST ? STN_WHY_SG_TEST : STN_WHY_SG_BACK;
        back_to_streaming(s, in);
        stn_request_apply_why(s, why);
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
    /* What follows, for the log: the station's own retry only with
     * automatic configuration (else the user applies again). */
    note(s, STN_NOTE_APPLY_FAILED, stn_config_auto_retry(s, in->autocfg) ? (int64_t)secs(s->apply_retry_ms - now) : 0,
         r->receiver_lost ? 1 : 0);
    if (r->restarted && in->profile_survey) {
        survey_restarted(s, in);   /* the rollback's restart: the receiver surveys again */
    }
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

static void done_sky(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    s->paused = false;
    const bool on = s->job.sky_on;
    note(s, on ? STN_NOTE_SKY_ON : STN_NOTE_SKY_OFF, r->lease_ms, (int32_t)r->err);
    if (r->err == STN_OK) {
        s->sky_on = on;
        s->sky_unsure = false;
        s->sky_off_tries = 0;
        s->sky_retry_ms = STN_NEVER;
        return;
    }
    if (on) {
        /* Not on: the window drops its wish until the user starts it again.
         * Without an answer it may have taken: it counts as on, unconfirmed,
         * and is turned off when the window closes (not at once: that would
         * hold RTCM back a second time right away). */
        s->sky_want = false;
        switch (r->err) {
        case STN_E_FAIL:
            s->sky_why = STN_SKY_WHY_REJECTED;
            break;
        case STN_E_TIMEOUT:
            s->sky_why = STN_SKY_WHY_NO_ANSWER;
            s->sky_on = true;
            s->sky_unsure = true;
            break;
        default:
            s->sky_why = STN_SKY_WHY_BUSY;   /* nothing was sent */
            break;
        }
        return;
    }
    if (r->err == STN_E_STATE) {
        s->sky_retry_ms = in->now_ms + STN_SKY_RETRY_MS;   /* nothing was sent: try again later */
        return;
    }
    if (++s->sky_off_tries >= STN_SKY_OFF_TRIES) {
        note(s, STN_NOTE_SKY_LEFT_ON, 0, 0);
        s->sky_on = false;   /* given up: it ends with the receiver's next restart */
        s->sky_unsure = false;
        s->sky_off_tries = 0;
        s->sky_retry_ms = STN_NEVER;
        return;
    }
    s->sky_retry_ms = in->now_ms + STN_SKY_RETRY_MS;
}

static void done_position(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    switch (r->err) {
    case STN_OK:
        s->alerts &= ~STN_ALERT_POSITION_MISMATCH;
        if (s->relocating && !s->relocation_ready) {
            s->pos_check_ms = STN_NEVER;   /* an old reference check cannot release relocation */
            break;
        }
        s->relocating = false;
        s->relocation_ready = false;
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

static void done_own_fix(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (!s->fix_on || !s->fix_running) {
        return;   /* dropped or re-armed meanwhile: the answer belongs to no check */
    }
    if (r->err != STN_OK || !um98x_own_fix_usable(&r->own)) s->move_samples = 0;
    switch (r->err) {
    case STN_E_STATE:
        s->fix_due_ms = now + STN_FIX_BUSY_MS;   /* nothing was sent: not a try */
        return;
    case STN_E_FAIL:
        if (!s->job.sppnav) {
            s->fix_sppnav = true;   /* no base-position log on this firmware: the single-point one */
            s->fix_due_ms = now + STN_FIX_BUSY_MS;
            return;
        }
        fix_finish(s, now, STN_FIX_END_REFUSED);
        return;
    case STN_OK: {
        if (um98x_own_fix_usable(&r->own)) {
            s->own_fix = r->own;
            s->own_fix_ms = now;
            if (!in->fixed_valid) {
                fix_finish(s, now, STN_FIX_END_NONE);
                return;
            }
        }
        if (!s->job.sppnav && r->own.pos_type == UM98X_OWN_POS_FIXED) {
            s->fix_sppnav = true;   /* it only echoes the set position: ask the single-point log */
        }
        um98x_fix_offset_t o;
        if (um98x_fix_offset(s->fix_ref[0], s->fix_ref[1], s->fix_ref[2], &r->own, &o)) {
            s->fix_usable_seen = true;
            s->fix_sample_ms = now;
            /* Do not reduce uncertainty by averaging correlated single-point fixes.
             * A candidate must stay displaced and near its original location for
             * five minutes. Missing fixes and long gaps break the observation. */
            double sigma = hypot(r->own.sd_lat_m, r->own.sd_lon_m);
            double limit = fmax(STN_MOVE_FLOOR_M, 4.0 * sigma);
            if (o.horizontal_m > limit && auto_apply_allowed(s, in) && !s->maint && !s->relocating) {
                um98x_fix_offset_t drift;
                double anchor_sigma = hypot(s->move_anchor.sd_lat_m, s->move_anchor.sd_lon_m);
                bool consistent = s->move_samples && now - s->move_last_ms <= STN_MOVE_MAX_GAP_MS &&
                    um98x_fix_offset(s->move_anchor.lat_deg, s->move_anchor.lon_deg,
                                    s->move_anchor.msl_height_m + s->move_anchor.separation_m, &r->own, &drift) &&
                    drift.horizontal_m <= fmax(STN_MOVE_FLOOR_M, 2.0 * hypot(sigma, anchor_sigma));
                if (!consistent) {
                    s->move_anchor = r->own;
                    s->move_started_ms = now;
                    s->move_samples = 1;
                }
                s->move_last_ms = now;
                if (now - s->move_started_ms >= STN_MOVE_CONFIRM_MS) {
                    s->move_samples = 0;
                    s->relocating = true;
                    s->relocation_ready = false;
                    s->pos_check_ms = STN_NEVER;
                    want_apply(s, STN_WHY_RESURVEY, STN_CHANGE_RESURVEY, true);
                }
            } else {
                s->move_samples = 0;
            }
            uint8_t before = s->fix.verdict;
            uint8_t v = um98x_fixchk_feed(&s->fix, &o);
            if (v != UM98X_FIX_NONE) {
                fix_decided(s, before, v);
                fix_finish(s, now, STN_FIX_END_NONE);
                return;
            }
        }
        break;
    }
    default:
        s->move_samples = 0;
        break;   /* no log, or one this firmware does not read */
    }
    if (++s->fix_tries >= STN_FIX_TRIES) {
        fix_finish(s, now, s->fix_usable_seen ? STN_FIX_END_UNDECIDED : STN_FIX_END_NO_SOLUTION);
        return;
    }
    if (!s->fix_sppnav && !s->fix_usable_seen && s->fix_tries >= STN_FIX_BASEPOS_TRIES) {
        s->fix_sppnav = true;   /* nothing usable from the base-position log: the single-point one */
    }
    s->fix_due_ms = now + STN_FIX_PERIOD_MS;
}

static void done_flash(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    int64_t now = in->now_ms;
    if (r->loader_key_sent) {
        s->loader_key_sent = true;   /* the identification used the boot's one restart from the loader */
    }
    switch (r->flash_end) {
    case STN_FLASH_END_REAPPLY:
    case STN_FLASH_END_REAPPLY_RTCM:
        /* The application answers: re-apply the profile, whatever gnss/autocfg says, then check it. */
        sky_receiver_changed(s);
        s->paused = false;
        s->last_resort = false;
        set_identity(s, r->supported, false);
        s->profile_applied = false;
        s->flash_close = (r->flash_end == STN_FLASH_END_REAPPLY) ? 1 : 2;
        s->flash_close_ms = now + STN_FLASH_CLOSE_MAX_MS;
        s->flash_rtcm_ms = STN_NEVER;
        s->check_ms = STN_NEVER;
        s->obs_on = false;
        enter_streaming(s, in, ENTER_KEEP);
        if (r->supported) {
            want_apply(s, STN_WHY_FLASH, STN_CHANGE_NONE, true);
        } else {
            flash_close_note(s, false);   /* not a model we configure: nothing to re-apply */
        }
        if (in->profile_survey) {
            survey_restarted(s, in);   /* the receiver restarted */
        }
        break;
    case STN_FLASH_END_NEEDS_USER:
        s->state = STN_FLASH_RECOVERY;
        s->paused = true;
        s->obs_on = false;
        break;
    default:
        if (s->state == STN_FLASH_RECOVERY) {
            break;   /* a user action that changed nothing: the journal still waits */
        }
        if (r->in_loader) {
            /* The receiver sat at its loader and did not come back: as detection would. */
            s->alerts |= STN_ALERT_RECEIVER_IN_LOADER;
            s->listen_only = true;
            s->paused = false;
            drop_stale_requests(s);
            s->state = STN_NO_RECEIVER;
            s->no_rx_ms = now;
            s->profile_applied = false;
            s->obs_on = false;
            s->episode_ms = now;
            s->detect_ms = now + STN_DETECT_FAST_MS;
            take_base(s, in);
            break;
        }
        back_to_streaming(s, in);
        break;
    }
}

void stn_done(stn_t *s, const stn_result_t *r, const stn_in_t *in)
{
    track_rtcm(s, in);
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
        if (r->err != STN_E_STATE) {
            sky_receiver_changed(s);
        }
        break;
    case STN_JOB_CHECK_POSITION:
        done_position(s, r, in);
        break;
    case STN_JOB_SKY:
        done_sky(s, r, in);
        break;
    case STN_JOB_FLASH:
        done_flash(s, r, in);
        break;
    case STN_JOB_BENCH:
        s->paused = false;
        sky_receiver_changed(s);
        stream_first(s, in, true);   /* the receiver may have been reset: look again, as at boot */
        break;
    case STN_JOB_OWN_FIX:
        done_own_fix(s, r, in);
        break;
    case STN_JOB_QUERY:
        if (s->job.leased) {
            s->paused = false;
        }
        note(s, STN_NOTE_QUERY, r->lease_ms, (int32_t)r->err | (int32_t)((uint32_t)s->job.query << 8));
        break;
    case STN_JOB_ASK:   /* its answer is a line like any other (stn_responding) */
    case STN_JOB_PKG_CHECK:
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

void stn_request_apply_why(stn_t *s, uint8_t why)
{
    want_apply(s, (stn_why_t)why, STN_CHANGE_NONE, false);
    if (s->apply_change == STN_CHANGE_NONE) {
        s->apply_why = why;   /* the comparison follows its own re-apply */
    }
}

bool stn_sg_fallback(uint8_t refused_m, uint8_t refused_s, bool before_known, uint8_t before_m, uint8_t before_s,
                     bool ok_known, uint16_t ok_pair, uint8_t *out_m, uint8_t *out_s)
{
    if (before_known && !(before_m == refused_m && before_s == refused_s)) {
        *out_m = before_m;
        *out_s = before_s;
        return true;
    }
    uint8_t m = (uint8_t)(ok_pair >> 8);
    uint8_t sl = (uint8_t)(ok_pair & 0xFFu);
    if (ok_known && !(m == refused_m && sl == refused_s)) {
        *out_m = m;
        *out_s = sl;
        return true;
    }
    return false;
}

int stn_request_flash(stn_t *s, uint8_t kind)
{
    if (s->maint || s->flash_want != 0) {
        return STN_E_STATE;
    }
    switch (kind) {
    case STN_FLASH_NEW:
        if (s->state != STN_STREAMING || s->flash_close != 0) {
            return STN_E_STATE;
        }
        break;
    case STN_FLASH_RETRY:
    case STN_FLASH_RESTART:
    case STN_FLASH_RECOVER:
        if (s->state != STN_FLASH_RECOVERY || (s->job_busy && s->job.kind == STN_JOB_FLASH)) {
            return STN_E_STATE;
        }
        break;
    default:
        return STN_E_STATE;
    }
    s->flash_want = kind;
    return STN_OK;
}

bool stn_flash_pending(const stn_t *s)
{
    return s->flash_want != 0 || (s->job_busy && s->job.kind == STN_JOB_FLASH);
}

int stn_request_bench(stn_t *s)
{
    if (s->maint || flash_state(s) || s->job_busy || s->bench_want || s->flash_want != 0 ||
        (s->state != STN_STREAMING && s->state != STN_NO_RECEIVER)) {
        return STN_E_STATE;
    }
    s->bench_want = true;
    return STN_OK;
}

int stn_request_query(stn_t *s, uint8_t query, bool leased, int64_t now_ms)
{
    if (s->maint || flash_state(s) || s->state != STN_STREAMING || stn_query_pending(s)) {
        return STN_E_STATE;
    }
    s->query_want = (uint8_t)(query + 1u);
    s->query_leased = leased;
    s->query_until_ms = now_ms + STN_QUERY_WAIT_MS;
    s->query_seq++;
    return STN_OK;
}

bool stn_query_pending(const stn_t *s)
{
    return s->query_want != 0 || (s->job_busy && s->job.kind == STN_JOB_QUERY);
}

int stn_request_pkg_check(stn_t *s)
{
    if (flash_state(s) || s->check_want || s->flash_want != 0) {
        return STN_E_STATE;
    }
    s->check_want = true;
    return STN_OK;
}

void stn_request_lock(stn_t *s)
{
    want_apply(s, STN_WHY_LOCK, STN_CHANGE_LOCK, true);
}

void stn_request_resurvey(stn_t *s)
{
    want_apply(s, STN_WHY_RESURVEY, STN_CHANGE_RESURVEY, true);
}

void stn_profile_changed(stn_t *s)
{
    s->static_ok = false;
    s->profile_applied = false;
}

bool stn_profile_acknowledged(const stn_t *s)
{
    /* Not while the station looks for the receiver or updates it: what is
     * there then is not known to hold anything. */
    bool present = s->state != STN_DETECTING && s->state != STN_NO_RECEIVER && s->state != STN_FLASHING &&
                   s->state != STN_FLASH_RECOVERY;
    return present && s->identity_ok && s->supported && s->static_ok;
}

bool stn_reconfigure_refused(const stn_t *s, bool autocfg)
{
    return !autocfg || config_failed(s);
}

int stn_request_recovery(stn_t *s, uint8_t action, bool autocfg)
{
    if (action >= STN_REC_COUNT || s->maint) {
        return STN_E_STATE;   /* in maintenance nothing is sent to the receiver */
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

int stn_request_detect(stn_t *s, int64_t now_ms)
{
    if (s->state != STN_NO_RECEIVER || s->maint) {
        return STN_E_STATE;
    }
    if (s->job_busy && s->job.kind == STN_JOB_DETECT) {
        return STN_OK;   /* the search runs now */
    }
    if (s->detect_ms == STN_NEVER || s->detect_ms > now_ms) {
        s->detect_ms = now_ms;   /* episode_ms and short_fails stay: the schedule goes on from where it is */
    }
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
    sky_receiver_changed(s);
    if (s->maint) {
        s->maint_restart_seen = true;   /* noted; the exit's stream-first path looks at the receiver again */
        return;
    }
    s->restart_seen = true;
}

void stn_on_arp(stn_t *s, const int64_t ecef_01mm[3])
{
    um98x_survey_on_arp(&s->survey, ecef_01mm);
}

/* ---- first-apply hold --------------------------------------------------------- */

bool stn_hold_at_boot(bool option, bool settings_ok, bool have_applied_hash, bool have_autocfg, bool have_recovery,
                      stn_hold_writes_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!option) {
        return false;
    }
    if (!settings_ok) {
        /* Whether this device was ever configured cannot be read: hold for
         * this boot (fail closed); nothing can be written. */
        out->autocfg_off = true;
        out->hold = true;
        out->ram_only = true;
        return true;
    }
    if (have_applied_hash || have_autocfg) {
        return false;
    }
    out->autocfg_off = true;
    out->hold = true;
    out->recovery_alerts = !have_recovery;   /* a recovery mode the user chose stays */
    return true;
}

void stn_set_first_apply_held(stn_t *s, bool held)
{
    s->first_apply_held = held;
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

bool stn_survey_unfinished(const stn_t *s, int64_t now_ms)
{
    return s->survey.active && !um98x_survey_done(&s->survey, now_ms);
}

/* ---- power-on reset: did the receiver keep running? -------------------------- */

stn_rxboot_t stn_rxboot_judge(const stn_rxboot_in_t *in, stn_rxboot_by_t *by)
{
    stn_rxboot_t v = STN_RXBOOT_PENDING;
    stn_rxboot_by_t how = STN_RXBOOT_BY_NONE;
    int64_t at = STN_NEVER;   /* when the verdict so far was decided */

    /* Its restart banner before any data: it restarted with the ESP32 (a
     * restart after data came is a later one, the receiver's own). */
    if (in->restart_ms != STN_NEVER && in->restart_ms <= STN_RXBOOT_WINDOW_MS &&
        (in->data_ms == STN_NEVER || in->restart_ms <= in->data_ms)) {
        v = STN_RXBOOT_RESTARTED;
        how = STN_RXBOOT_BY_RESTART;
        at = in->restart_ms;
    }
    /* Data at once, before any restart evidence: it was running. */
    if (in->data_ms != STN_NEVER && in->data_ms <= STN_RXBOOT_DATA_MS &&
        (in->restart_ms == STN_NEVER || in->data_ms < in->restart_ms) && in->data_ms < at) {
        v = STN_RXBOOT_KEPT;
        how = STN_RXBOOT_BY_STREAM;
        at = in->data_ms;
    }
    /* Its own clock: when did it start, on the ESP32's time line? The
     * receiver's word on itself wins a tie. */
    if (in->uptime_at_ms != STN_NEVER && in->uptime_at_ms <= STN_RXBOOT_WINDOW_MS && in->uptime_at_ms <= at) {
        int64_t started = in->uptime_at_ms - (int64_t)in->uptime_ms;
        v = started < -(int64_t)STN_RXBOOT_MARGIN_MS ? STN_RXBOOT_KEPT : STN_RXBOOT_RESTARTED;
        how = STN_RXBOOT_BY_UPTIME;
        at = in->uptime_at_ms;
    }
    if (v == STN_RXBOOT_PENDING && in->now_ms >= STN_RXBOOT_WINDOW_MS) {
        v = STN_RXBOOT_UNKNOWN;
    }
    if (by != NULL) {
        *by = how;
    }
    return v;
}

bool stn_rxboot_judged(stn_rxboot_reset_t reset)
{
    return reset == STN_RXBOOT_RESET_POWER_ON || reset == STN_RXBOOT_RESET_BROWNOUT;
}

void stn_rxboot_follow(stn_rxboot_reset_t reset, stn_rxboot_t verdict, stn_rxboot_follow_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!stn_rxboot_judged(reset)) {
        return;
    }
    /* The receiver restarted with the ESP32: the power went for both, a cut
     * or a dip deep enough for the receiver too. */
    out->maint_ends = verdict == STN_RXBOOT_RESTARTED;
    /* It kept running: after a power-on reset that was the EN pin, no power
     * loss at all; after a brownout the ESP32's own supply did dip. */
    out->power_kept = verdict == STN_RXBOOT_KEPT && reset == STN_RXBOOT_RESET_POWER_ON;
}

bool stn_line_is_nmea(const char *line, size_t len)
{
    if (line == NULL || len < 7 || line[0] != '$' || line[6] != ',') {
        return false;
    }
    for (size_t i = 1; i < 6; i++) {
        if (line[i] < 'A' || line[i] > 'Z') {
            return false;
        }
    }
    return true;
}

size_t stn_apply_next_text(uint32_t retry_in_s, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    int n;
    if (retry_in_s == 0) {
        n = snprintf(out, cap, "nothing retries it on its own (automatic configuration is off): apply it again from "
                               "the receiver page");
    } else if (retry_in_s < 120u) {
        n = snprintf(out, cap, "the station tries again in %u s", (unsigned)retry_in_s);
    } else if (retry_in_s < 7200u) {
        n = snprintf(out, cap, "the station tries again in %u min", (unsigned)((retry_in_s + 30u) / 60u));
    } else {
        n = snprintf(out, cap, "the station tries again in %u h", (unsigned)((retry_in_s + 1800u) / 3600u));
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1u;
}

bool stn_config_auto_retry(const stn_t *s, bool autocfg)
{
    return autocfg && (!s->identity_ok || s->supported);
}

uint32_t stn_config_retry_in_s(const stn_t *s, bool autocfg, int64_t now_ms)
{
    /* Without automatic configuration the backoff only looks again later
     * and never applies: no countdown that leads nowhere. */
    if (!config_failed(s) || s->apply_retry_ms == STN_NEVER || !stn_config_auto_retry(s, autocfg)) {
        return 0;
    }
    int64_t left = s->apply_retry_ms - now_ms;
    if (left <= 0) {
        return 0;
    }
    int64_t sec = (left + 999) / 1000;
    return sec > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)sec;
}

void stn_note_signals(stn_t *s, int verdict)
{
    if (verdict == 0) {
        s->alerts |= STN_ALERT_SIGNALS_MISMATCH;
    } else {
        s->alerts &= ~STN_ALERT_SIGNALS_MISMATCH;
    }
}

uint32_t stn_alerts(const stn_t *s)
{
    uint32_t a = s->alerts & ~STN_ALERT_NO_RECEIVER;
    if (s->state == STN_NO_RECEIVER && !s->maint && (a & STN_ALERT_RECEIVER_IN_LOADER) == 0) {
        a |= STN_ALERT_NO_RECEIVER;
    }
    return a;
}

void stn_fix_status(const stn_t *s, int64_t now_ms, stn_fix_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->checked_age_s = UINT32_MAX;
    out->own_age_s = UINT32_MAX;
    if (um98x_own_fix_usable(&s->own_fix)) {
        int64_t age = (now_ms - s->own_fix_ms) / 1000;
        out->own_age_s = age < 0 ? 0 : (age > UINT32_MAX ? UINT32_MAX : (uint32_t)age);
        out->own_valid = out->own_age_s <= 90;
        out->own = s->own_fix;
    }
    if (!s->fix_on || !s->fix_reference) {
        out->state = STN_FIX_OFF;
        return;
    }
    if (s->fix_sample_ms != STN_NEVER) {
        int64_t age = (now_ms - s->fix_sample_ms) / 1000;
        out->checked_age_s = age < 0 ? 0 : (age >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age);
    }
    switch (s->fix.verdict) {
    case UM98X_FIX_FAR:
        out->state = STN_FIX_FAR;
        out->offset = s->fix.shown;
        out->height_only = um98x_fix_height_only(&s->fix.shown);
        break;
    case UM98X_FIX_SEA_LEVEL:
        out->state = STN_FIX_FAR;
        out->offset = s->fix.shown;
        out->height_only = true;   /* the horizontal distance is within the limit */
        out->sea_level = true;
        break;
    case UM98X_FIX_NEAR:
        out->state = STN_FIX_NEAR;
        out->offset = s->fix.shown;
        break;
    default:
        /* No verdict: how the last check ended, while the next one runs too. */
        out->state = s->fix_end == STN_FIX_END_NO_SOLUTION ? STN_FIX_NO_SOLUTION
                     : s->fix_end == STN_FIX_END_REFUSED   ? STN_FIX_REFUSED
                                                           : STN_FIX_CHECKING;
        break;
    }
}

uint32_t stn_next_detect_in_s(const stn_t *s, int64_t now_ms)
{
    if ((s->state != STN_NO_RECEIVER && s->state != STN_DETECTING) || s->maint) {
        return UINT32_MAX;
    }
    if (s->job_busy && s->job.kind == STN_JOB_DETECT) {
        return 0;   /* a pass runs now */
    }
    if (s->detect_ms == STN_NEVER) {
        return UINT32_MAX;
    }
    uint32_t left = secs(s->detect_ms - now_ms);
    return left == UINT32_MAX ? UINT32_MAX - 1u : left;
}

bool stn_responding(const stn_t *s, int64_t now_ms, uint32_t rtcm_age_ms, int64_t line_ms)
{
    if (s->state == STN_NO_RECEIVER) {
        return false;
    }
    if (rtcm_age_ms <= STN_FRESH_MS) {
        return true;   /* it streams */
    }
    if (line_ms == STN_NEVER || (s->no_rx_ms != STN_NEVER && line_ms <= s->no_rx_ms)) {
        return false;
    }
    if (now_ms - line_ms > STN_ANSWER_FRESH_MS) {
        return false;   /* asked every STN_ASK_MS while it sends no RTCM: an answer is this fresh */
    }
    /* With RTCM this boot: its last line came after its last RTCM frame. */
    return rtcm_age_ms == UINT32_MAX || line_ms - (now_ms - (int64_t)rtcm_age_ms) > STN_ANSWER_SLACK_MS;
}

/* ---- receiver maintenance --------------------------------------------------- */

bool stn_maint_duration_ok(uint32_t dur_s)
{
    static const uint32_t k_choices[] = { 15u * 60u, 30u * 60u, 3600u, 2u * 3600u, 4u * 3600u };
    for (unsigned i = 0; i < sizeof(k_choices) / sizeof(k_choices[0]); i++) {
        if (k_choices[i] == dur_s) {
            return true;
        }
    }
    return false;
}

int stn_maint_enter(stn_t *s, uint32_t dur_s, bool lease_held, int64_t now_ms)
{
    if (!stn_maint_duration_ok(dur_s)) {
        return STN_MAINT_DURATION;
    }
    if (s->maint) {
        /* Extension: added to the time left, at most the longest choice. */
        int64_t left = s->maint_until_ms - now_ms;
        if (left < 0) {
            left = 0;
        }
        int64_t next = left + (int64_t)dur_s * 1000;
        if (next > (int64_t)STN_MAINT_MAX_S * 1000) {
            next = (int64_t)STN_MAINT_MAX_S * 1000;
        }
        int64_t was = s->maint_until_ms;
        s->maint_until_ms = now_ms + next;
        s->maint_chosen_s = secs(s->maint_until_ms - s->maint_start_ms);
        if (s->maint_end_unix > 0 && s->maint_until_ms > was) {
            s->maint_end_unix += (s->maint_until_ms - was) / 1000;   /* the end by the clock moves as much */
        }
        s->maint_resumed = false;   /* the user's own now: a power-cut verdict no longer ends it */
        return STN_MAINT_OK;
    }
    if (flash_state(s)) {
        return STN_MAINT_FLASH;
    }
    if (lease_held || s->job_busy || s->state == STN_CONFIGURING || s->state == STN_RECOVERING) {
        return STN_MAINT_SESSION;
    }
    if (s->sky_on && s->state != STN_DETECTING && s->state != STN_NO_RECEIVER) {
        /* The live view's satellite-in-view output may be on (unsaved). In
         * maintenance nothing would turn it off, and a save made through the
         * receiver's other port would keep it for good: it goes off first
         * (at the next free moment; the window stays open without it), and
         * the user offers maintenance again. */
        if (s->live_open && s->sky_want) {
            s->sky_want = false;
            s->sky_why = STN_SKY_WHY_MAINTENANCE;
        }
        s->sky_unsure = false;          /* off now, not only when the window closes */
        s->sky_retry_ms = STN_NEVER;    /* the user waits for it */
        return STN_MAINT_SKY_OUTPUT;
    }
    s->maint = true;
    s->maint_start_ms = now_ms;
    s->maint_until_ms = now_ms + (int64_t)dur_s * 1000;
    s->maint_chosen_s = dur_s;
    s->maint_restart_seen = false;
    s->maint_resumed = false;
    s->maint_end_unix = 0;
    drop_stale_requests(s);   /* queued ladder steps and automatic re-applies */
    query_drop(s);            /* nothing is sent in maintenance */
    /* Listening only, as in the first seconds of a boot: the health
     * monitor's ladder stays suspended, no drift is judged. */
    s->state = STN_IDLE;
    s->paused = false;
    s->obs_on = false;
    note(s, STN_NOTE_MAINT_ON, dur_s, 0);
    return STN_MAINT_OK;
}

int stn_maint_end(stn_t *s, const stn_in_t *in)
{
    if (!s->maint) {
        return STN_MAINT_NOT_ON;
    }
    maint_exit(s, in, STN_MAINT_END_USER);
    return STN_MAINT_OK;
}

uint32_t stn_maint_left_s(const stn_t *s, int64_t now_ms)
{
    return s->maint ? secs(s->maint_until_ms - now_ms) : 0;
}

#define MAINT_STORE_SHORT_MS ((int64_t)STN_MAINT_STORE_SHORT_S * 1000)

void stn_maint_record(const stn_t *s, int64_t now_ms, int64_t utc_now, bool store, stn_maint_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    if (!s->maint) {
        return;
    }
    int64_t exact = s->maint_until_ms - now_ms;
    if (exact < 0) {
        exact = 0;
    }
    int64_t left = store ? exact - MAINT_STORE_SHORT_MS : exact;
    if (left < 0) {
        left = 0;
    }
    if (left > (int64_t)STN_MAINT_MAX_S * 1000) {
        left = (int64_t)STN_MAINT_MAX_S * 1000;
    }
    rec->on = true;
    rec->left_ms = (uint32_t)left;
    rec->chosen_s = s->maint_chosen_s;
    /* The end by the clock: from the clock now, never past a resumed end
     * that no clock has checked yet. */
    rec->end_unix = s->maint_end_unix;
    if (utc_now > 0 && (rec->end_unix <= 0 || utc_now + exact / 1000 < rec->end_unix)) {
        rec->end_unix = utc_now + exact / 1000;
    }
}

bool stn_maint_store_due(const stn_t *s, uint32_t saved_chosen_s, int64_t saved_ms, int64_t now_ms)
{
    if (!s->maint) {
        return false;
    }
    if (saved_ms == STN_NEVER || saved_chosen_s != s->maint_chosen_s) {
        return true;
    }
    return now_ms - saved_ms >= (int64_t)STN_MAINT_SAVE_S * 1000;
}

#define MAINT_RTC_MAGIC 0x324E4D52u   /* "RMN2" (the earlier layout was "RMNT") */

/* CRC-32 (reflected 0xEDB88320) of n words, low byte first. */
static uint32_t crc_words(const uint32_t *w, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        for (unsigned k = 0; k < 4; k++) {
            crc ^= (w[i] >> (8u * k)) & 0xFFu;
            for (int b = 0; b < 8; b++) {
                crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            }
        }
    }
    return ~crc;
}

/* The record's words before the CRC. */
static uint32_t rtc_crc(const stn_maint_rtc_t *r)
{
    const uint32_t w[9] = { r->magic,  r->stamp,  r->on,     r->left_ms, r->chosen_s,
                            r->rtc_lo, r->rtc_hi, r->end_lo, r->end_hi };
    return crc_words(w, 9);
}

void stn_maint_rtc_write(stn_maint_rtc_t *r, const stn_maint_rec_t *rec)
{
    r->magic = MAINT_RTC_MAGIC;
    r->stamp = rec->stamp;
    r->on = rec->on ? 1u : 0u;
    r->left_ms = rec->on ? rec->left_ms : 0;
    r->chosen_s = rec->on ? rec->chosen_s : 0;
    r->rtc_lo = (uint32_t)rec->rtc_us;
    r->rtc_hi = (uint32_t)(rec->rtc_us >> 32);
    r->end_lo = (uint32_t)(uint64_t)rec->end_unix;
    r->end_hi = (uint32_t)((uint64_t)rec->end_unix >> 32);
    r->crc = rtc_crc(r);
}

/* Time left within bounds and within the chosen duration. */
static bool maint_fields_ok(uint32_t left_ms, uint32_t chosen_s)
{
    return left_ms <= STN_MAINT_MAX_S * 1000u && (uint64_t)left_ms <= (uint64_t)chosen_s * 1000u &&
           chosen_s <= STN_MAINT_MAX_S * 64u;
}

bool stn_maint_rtc_read(const stn_maint_rtc_t *r, stn_maint_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    if (r->magic != MAINT_RTC_MAGIC || r->crc != rtc_crc(r) || r->on > 1u ||
        (r->on == 1u && !maint_fields_ok(r->left_ms, r->chosen_s))) {
        return false;
    }
    rec->on = r->on == 1u;
    rec->stamp = r->stamp;
    rec->left_ms = r->left_ms;
    rec->chosen_s = r->chosen_s;
    rec->rtc_us = ((uint64_t)r->rtc_hi << 32) | r->rtc_lo;
    rec->end_unix = (int64_t)(((uint64_t)r->end_hi << 32) | r->end_lo);
    return true;
}

/* What the station knew, in the stored copy (version 2): its flag bits. */
#define KNOWN_IDENTIFIED   (1u << 0)
#define KNOWN_CONFIRMED    (1u << 1)
#define KNOWN_APPLIED      (1u << 2)
#define KNOWN_HEIGHT_ONLY  (1u << 3)
#define KNOWN_SURVEY       (1u << 4)
#define KNOWN_COMPLETE     (1u << 5)
#define KNOWN_LOCK_DONE    (1u << 6)
#define KNOWN_SG           (1u << 7)
/* ... and in its second flag byte, the last one (a copy written before a
 * bit existed reads it as 0). */
#define KNOWN_SEA_LEVEL    (1u << 0)
#define KNOWN_ACKNOWLEDGED (1u << 1)

static void known_put(blob_writer_t *w, const stn_maint_known_t *k)
{
    uint8_t f = (uint8_t)((k->receiver.valid ? KNOWN_IDENTIFIED : 0u) | (k->identity_confirmed ? KNOWN_CONFIRMED : 0u) |
                          (k->profile_applied ? KNOWN_APPLIED : 0u) | (k->fix_height_only ? KNOWN_HEIGHT_ONLY : 0u) |
                          (k->survey_active ? KNOWN_SURVEY : 0u) | (k->survey_complete ? KNOWN_COMPLETE : 0u) |
                          (k->lock_done ? KNOWN_LOCK_DONE : 0u) | (k->sg_valid ? KNOWN_SG : 0u));
    blob_put_u32(w, k->profile_hash);
    blob_put_u8(w, f);
    blob_put_str(w, k->receiver.valid ? k->receiver.model : "");
    blob_put_str(w, k->receiver.valid ? k->receiver.firmware : "");
    blob_put_u32(w, k->receiver.valid ? k->receiver.build : 0u);
    blob_put_str(w, k->receiver.valid ? k->receiver.serial : "");
    blob_put_str(w, k->receiver.valid ? k->receiver.pn : "");
    blob_put_str(w, k->receiver.valid ? k->receiver.efuse_id : "");
    blob_put_str(w, k->receiver.valid ? k->receiver.compile_date : "");
    blob_put_u32(w, k->alerts & ~STN_ALERT_NO_RECEIVER);
    blob_put_u8(w, k->fix_state);
    blob_put_f32(w, k->fix_horizontal_m);
    blob_put_f32(w, k->fix_up_m);
    blob_put_f32(w, k->fix_distance_m);
    blob_put_u32(w, k->survey_s);
    blob_put_u32(w, (uint32_t)k->survey_end_rel_ms);
    blob_put_u8(w, k->sg_valid ? k->sg_master : 0u);
    blob_put_u8(w, k->sg_valid ? k->sg_slave : 0u);
    blob_put_u8(w, (uint8_t)((k->fix_sea_level ? KNOWN_SEA_LEVEL : 0u) |
                             (k->profile_acknowledged ? KNOWN_ACKNOWLEDGED : 0u)));
}

/* False (k untouched) for a part that does not read or does not make sense. */
static bool known_get(blob_reader_t *r, stn_maint_known_t *k)
{
    stn_maint_known_t n;
    memset(&n, 0, sizeof(n));
    n.profile_hash = blob_get_u32(r);
    uint8_t f = blob_get_u8(r);
    blob_get_str(r, n.receiver.model, sizeof(n.receiver.model));
    blob_get_str(r, n.receiver.firmware, sizeof(n.receiver.firmware));
    n.receiver.build = blob_get_u32(r);
    blob_get_str(r, n.receiver.serial, sizeof(n.receiver.serial));
    blob_get_str(r, n.receiver.pn, sizeof(n.receiver.pn));
    blob_get_str(r, n.receiver.efuse_id, sizeof(n.receiver.efuse_id));
    blob_get_str(r, n.receiver.compile_date, sizeof(n.receiver.compile_date));
    n.alerts = blob_get_u32(r) & ~STN_ALERT_NO_RECEIVER;
    n.fix_state = blob_get_u8(r);
    n.fix_horizontal_m = blob_get_f32(r);
    n.fix_up_m = blob_get_f32(r);
    n.fix_distance_m = blob_get_f32(r);
    n.survey_s = blob_get_u32(r);
    n.survey_end_rel_ms = (int32_t)blob_get_u32(r);
    n.sg_master = blob_get_u8(r);
    n.sg_slave = blob_get_u8(r);
    uint8_t f2 = blob_get_u8(r);
    if (!blob_reader_ok(r) || n.fix_state > STN_FIX_REFUSED || n.survey_s > UM98X_SURVEY_MAX_S ||
        ((f & KNOWN_IDENTIFIED) != 0 && n.receiver.model[0] == '\0')) {
        return false;
    }
    n.valid = true;
    n.receiver.valid = (f & KNOWN_IDENTIFIED) != 0;
    n.identity_confirmed = (f & KNOWN_CONFIRMED) != 0;
    n.profile_applied = (f & KNOWN_APPLIED) != 0;
    n.fix_height_only = (f & KNOWN_HEIGHT_ONLY) != 0;
    n.survey_active = (f & KNOWN_SURVEY) != 0;
    n.survey_complete = (f & KNOWN_COMPLETE) != 0;
    n.lock_done = (f & KNOWN_LOCK_DONE) != 0;
    n.sg_valid = (f & KNOWN_SG) != 0;
    n.fix_sea_level = (f2 & KNOWN_SEA_LEVEL) != 0 && n.fix_state == STN_FIX_FAR;
    n.profile_acknowledged = (f2 & KNOWN_ACKNOWLEDGED) != 0;
    *k = n;
    return true;
}

size_t stn_maint_store_encode(const stn_maint_rec_t *rec, const stn_maint_known_t *known, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, STN_MAINT_STORE_VERSION);
    blob_put_u32(&w, rec->stamp);
    blob_put_u32(&w, rec->left_ms);
    blob_put_u32(&w, rec->chosen_s);
    blob_put_i64(&w, rec->end_unix);
    bool have = known != NULL && known->valid;
    blob_put_u8(&w, have ? 1u : 0u);
    if (have) {
        known_put(&w, known);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

bool stn_maint_store_decode(stn_maint_rec_t *rec, stn_maint_known_t *known, const uint8_t *buf, size_t len)
{
    memset(rec, 0, sizeof(*rec));
    if (known != NULL) {
        memset(known, 0, sizeof(*known));
    }
    if (buf == NULL || len < 1 || buf[0] < 1u) {
        return false;
    }
    uint8_t version = buf[0];
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    (void)blob_get_u8(&r);
    uint32_t stamp = blob_get_u32(&r);
    uint32_t left_ms = blob_get_u32(&r);
    uint32_t chosen_s = blob_get_u32(&r);
    int64_t end_unix = blob_get_i64(&r);
    if (!blob_reader_ok(&r) || !maint_fields_ok(left_ms, chosen_s) || end_unix < 0) {
        return false;
    }
    rec->on = true;
    rec->stamp = stamp;
    rec->left_ms = left_ms;
    rec->chosen_s = chosen_s;
    rec->end_unix = end_unix;
    /* What the station knew: best effort, the time left stands without it. */
    if (known != NULL && version >= 2u && blob_reader_remaining(&r) > 0 && blob_get_u8(&r) == 1u) {
        (void)known_get(&r, known);
    }
    return true;
}

void stn_maint_known_survey(const stn_t *s, stn_maint_known_t *known)
{
    known->survey_active = false;
    known->survey_complete = false;
    known->lock_done = false;
    known->survey_s = 0;
    known->survey_end_rel_ms = 0;
    if (!s->maint || !s->survey.active) {
        return;
    }
    int64_t end = s->survey.start_ms + ((int64_t)s->survey.survey_s + UM98X_SURVEY_GRACE_S) * 1000;
    int64_t rel = end - s->maint_until_ms;
    known->survey_active = true;
    known->survey_complete = s->survey.complete;
    known->lock_done = s->lock_done;
    known->survey_s = s->survey.survey_s;
    known->survey_end_rel_ms = rel < INT32_MIN ? INT32_MIN : (rel > INT32_MAX ? INT32_MAX : (int32_t)rel);
}

void stn_maint_boot(const stn_maint_boot_in_t *in, stn_maint_boot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in->blocked) {
        return;
    }
    const stn_maint_rec_t *r;
    uint8_t from;
    int64_t elapsed_ms = in->uptime_ms > 0 ? in->uptime_ms : 0;   /* the least the reset took */
    if (in->rtc_ok) {
        if (!in->rtc.on) {
            return;   /* it had ended: a stored copy left behind is stale */
        }
        r = &in->rtc;
        from = STN_MAINT_FROM_RTC;
        if (in->rtc_timer_kept && in->rtc_now_us >= in->rtc.rtc_us) {
            uint64_t us = in->rtc_now_us - in->rtc.rtc_us;
            elapsed_ms = us >= (uint64_t)INT64_MAX - 999u ? INT64_MAX : (int64_t)((us + 999u) / 1000u);
            out->elapsed_known = true;
        }
    } else if (in->store_ok && in->store.on) {
        r = &in->store;
        from = STN_MAINT_FROM_STORE;
    } else {
        return;
    }
    out->chosen_s = r->chosen_s;
    if (elapsed_ms >= (int64_t)r->left_ms) {
        out->expired = true;   /* its time ran out across the reset */
        return;
    }
    out->from = from;
    out->left_ms = (uint32_t)((int64_t)r->left_ms - elapsed_ms);
    out->end_unix = out->elapsed_known ? 0 : r->end_unix;
    /* The stored copy is written whichever record gives the time. */
    out->known = in->store_ok && in->store.on && in->store_known;
}

void stn_maint_restore(stn_t *s, const stn_maint_boot_t *b, int64_t now_ms)
{
    if (b->from == STN_MAINT_FROM_NONE || b->left_ms == 0) {
        return;
    }
    int64_t left = b->left_ms;
    if (left > (int64_t)STN_MAINT_MAX_S * 1000) {
        left = (int64_t)STN_MAINT_MAX_S * 1000;
    }
    uint32_t chosen = b->chosen_s;
    if ((int64_t)chosen * 1000 < left) {
        chosen = secs(left);
    }
    s->maint = true;
    s->maint_until_ms = now_ms + left;
    s->maint_start_ms = s->maint_until_ms - (int64_t)chosen * 1000;   /* the time spent before counts */
    s->maint_chosen_s = chosen;
    s->maint_restart_seen = false;
    s->maint_resumed = true;
    s->maint_end_unix = b->end_unix > 0 ? b->end_unix : 0;
}

void stn_maint_restore_survey(stn_t *s, const stn_maint_known_t *known)
{
    if (known == NULL || !known->valid || !known->survey_active || !s->maint || !s->maint_resumed) {
        return;
    }
    int64_t end = s->maint_until_ms + known->survey_end_rel_ms;
    memset(&s->survey, 0, sizeof(s->survey));
    s->survey.active = true;
    s->survey.complete = known->survey_complete;
    s->survey.survey_s = known->survey_s;
    s->survey.start_ms = end - ((int64_t)known->survey_s + UM98X_SURVEY_GRACE_S) * 1000;
    s->lock_done = known->lock_done;
    s->lock_retry_ms = STN_NEVER;
    s->mode_poll_ms = STN_NEVER;
}

void stn_maint_clock(stn_t *s, int64_t utc_now, int64_t now_ms)
{
    if (!s->maint || s->maint_end_unix <= 0 || utc_now <= 0) {
        return;
    }
    /* The clock counts whole seconds: one second off, so it never ends late. */
    int64_t left = (s->maint_end_unix - utc_now - 1) * 1000;
    s->maint_end_unix = 0;
    if (left < 0) {
        left = 0;   /* its time ran out while the ESP32 was off: ends at the next step */
    }
    if (now_ms + left < s->maint_until_ms) {
        int64_t cut = s->maint_until_ms - (now_ms + left);
        s->maint_until_ms = now_ms + left;
        s->maint_start_ms = s->maint_until_ms - (int64_t)s->maint_chosen_s * 1000;
        /* A survey window restored with it (none starts during maintenance):
         * the receiver surveyed on while the ESP32 was held in reset. */
        if (s->survey.active) {
            s->survey.start_ms -= cut;
        }
    }
}

bool stn_maint_power_cut(stn_t *s, const stn_in_t *in)
{
    if (!s->maint || !s->maint_resumed) {
        return false;
    }
    maint_exit(s, in, STN_MAINT_END_POWER);
    return true;
}

#define SKY_RTC_MAGIC 0x594B5352u   /* "RSKY" */

static uint32_t sky_rtc_crc(const stn_sky_rtc_t *r)
{
    const uint32_t w[2] = { r->magic, r->stamp };
    return crc_words(w, 2);
}

void stn_sky_rtc_write(stn_sky_rtc_t *r, uint32_t stamp, bool on)
{
    if (!on) {
        memset(r, 0, sizeof(*r));
        return;
    }
    r->magic = SKY_RTC_MAGIC;
    r->stamp = stamp;
    r->crc = sky_rtc_crc(r);
}

bool stn_sky_rtc_read(const stn_sky_rtc_t *r, uint32_t *stamp)
{
    if (r->magic != SKY_RTC_MAGIC || r->crc != sky_rtc_crc(r)) {
        return false;
    }
    *stamp = r->stamp;
    return true;
}

void stn_sky_restore(stn_t *s)
{
    /* No window: sky_job turns it off once the station streams. */
    s->sky_on = true;
    s->sky_unsure = false;
    s->sky_off_tries = 0;
    s->sky_retry_ms = STN_NEVER;
}

/* ---- live view ----------------------------------------------------------------- */

int stn_live_start(stn_t *s, uint32_t dur_ms, int64_t now_ms, uint8_t *why)
{
    if (why != NULL) {
        *why = STN_SKY_WHY_NONE;
    }
    if (flash_state(s)) {
        if (why != NULL) {
            *why = STN_SKY_WHY_FLASH;
        }
        return STN_E_STATE;
    }
    const int64_t max = (int64_t)s->live_max_ms;
    if (dur_ms == 0) {
        dur_ms = STN_LIVE_DEFAULT_MS;
    }
    if (s->live_open) {
        int64_t until = (s->live_until_ms > now_ms ? s->live_until_ms : now_ms) + (int64_t)dur_ms;
        s->live_until_ms = until > now_ms + max ? now_ms + max : until;
        note(s, STN_NOTE_LIVE_EXTENDED, secs(s->live_until_ms - now_ms), 0);
    } else {
        s->live_open = true;
        s->live_opened_ms = now_ms;
        int64_t len = (int64_t)dur_ms > max ? max : (int64_t)dur_ms;
        s->live_until_ms = now_ms + len;
        s->live_last_end = STN_LIVE_END_NONE;
        note(s, STN_NOTE_LIVE_OPENED, secs(len), 0);
    }
    s->live_touch_ms = now_ms;
    s->sky_want = true;
    s->sky_why = STN_SKY_WHY_NONE;
    return STN_OK;
}

void stn_live_touch(stn_t *s, int64_t now_ms)
{
    if (s->live_open && now_ms > s->live_touch_ms) {
        s->live_touch_ms = now_ms;
    }
}

void stn_live_stop(stn_t *s, int64_t now_ms)
{
    live_close(s, now_ms, STN_LIVE_END_USER);
}

/* Why the output is not on now although the window wants it. */
static stn_sky_why_t sky_pending_why(const stn_t *s, const stn_in_t *in)
{
    if (s->maint) {
        return STN_SKY_WHY_MAINTENANCE;
    }
    switch (s->state) {
    case STN_FLASHING:
    case STN_FLASH_RECOVERY:
        return STN_SKY_WHY_FLASH;
    case STN_IDLE:
    case STN_DETECTING:
    case STN_NO_RECEIVER:
        return STN_SKY_WHY_NOT_STREAMING;
    case STN_STREAMING:
        break;
    default:
        return STN_SKY_WHY_BUSY;
    }
    if (!in->autocfg) {
        return STN_SKY_WHY_AUTOCFG_OFF;
    }
    if (s->identity_ok && !s->supported) {
        return STN_SKY_WHY_UNSUPPORTED;
    }
    return STN_SKY_WHY_BUSY;
}

void stn_live_status(const stn_t *s, const stn_in_t *in, stn_live_status_t *out)
{
    memset(out, 0, sizeof(*out));
    int64_t now = in->now_ms;
    out->last_end = s->live_last_end;
    out->open = s->live_open;
    if (s->live_open) {
        out->left_s = secs(s->live_until_ms - now);
        int64_t idle_end = s->live_touch_ms + (int64_t)s->live_idle_ms;
        int64_t end = idle_end < s->live_until_ms ? idle_end : s->live_until_ms;
        out->idle_left_s = secs(end - now);
        out->open_s = (uint32_t)(secs(now - s->live_opened_ms));
    }
    if (in->profile_sky) {
        out->sky = STN_SKY_ON_PROFILE;
    } else if (!s->live_open) {
        out->sky = s->sky_on ? STN_SKY_STOPPING : STN_SKY_OFF;
    } else if (s->sky_on && s->sky_want && !s->sky_unsure) {
        out->sky = STN_SKY_ON;
    } else if (!s->sky_want) {
        /* Unconfirmed (no answer to "on"): left until the window closes. */
        out->sky = (s->sky_on && !s->sky_unsure) ? STN_SKY_STOPPING : STN_SKY_BLOCKED;
        out->why = s->sky_why;
    } else {
        stn_sky_why_t why = sky_pending_why(s, in);
        out->why = (uint8_t)why;
        out->sky = (why == STN_SKY_WHY_AUTOCFG_OFF || why == STN_SKY_WHY_UNSUPPORTED || why == STN_SKY_WHY_FLASH)
                       ? STN_SKY_BLOCKED
                       : STN_SKY_PENDING;
    }
}

unsigned stn_take_notes(stn_t *s, stn_note_t *out, unsigned cap)
{
    unsigned n = s->note_n < cap ? s->note_n : cap;
    memcpy(out, s->notes, sizeof(s->notes[0]) * n);
    /* Whatever did not fit stays for the next call. */
    memmove(&s->notes[0], &s->notes[n], sizeof(s->notes[0]) * (s->note_n - n));
    s->note_n = (uint8_t)(s->note_n - n);
    return n;
}
