/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Station supervisor core, run against a small simulated world: a receiver
 * that sends the profile's RTCM set every second (or not), job outcomes
 * chosen by each test, and the observed-set tracker armed the way station.c
 * arms it. The clock starts near the 32-bit millisecond wrap.
 */
#include <stdint.h>
#include <string.h>

#include "station_core.h"
#include "unity.h"

#define T0 ((int64_t)UINT32_MAX - 2000)
#define MIN_MS 60000
#define HOUR_MS (60 * MIN_MS)

static const um98x_rtcm_out_t k_set[] = {
    { 1005, 1 }, { 1033, 1 }, { 1077, 1 }, { 1087, 1 }, { 1097, 1 }, { 1117, 1 }, { 1127, 1 }, { 1230, 1 },
};
#define K_SET_N ((uint8_t)(sizeof(k_set) / sizeof(k_set[0])))

typedef struct {
    stn_t s;
    stn_in_t in;
    stn_obs_t obs;
    bool rtcm_on;        /* the receiver sends the set every second */
    uint16_t skip;       /* ... except this message (0 = none) */
    bool skip_station;   /* ... and without 1005/1033 (held back while surveying) */
    uint32_t jobs;       /* jobs handed out */
} sim_t;

static void sim_init(sim_t *w, bool journal_open)
{
    memset(w, 0, sizeof(*w));
    stn_init(&w->s, journal_open);
    stn_obs_clear(&w->obs);
    w->in.now_ms = T0;
    w->in.autocfg = true;
    w->in.model_known = true;
    w->in.profile_survey = false;
    w->in.survey_s = 600;
    w->in.rtcm1230_ok = true;
    w->in.obs = &w->obs;
}

/* As station.c does after every step. */
static void sim_rearm(sim_t *w)
{
    if (w->s.obs_rearm) {
        stn_obs_arm(&w->obs, k_set, K_SET_N, w->in.now_ms);
        w->s.obs_rearm = false;
    }
}

static void sim_start(sim_t *w)
{
    stn_start(&w->s, &w->in);
    sim_rearm(w);
}

/* One second of world time. */
static void sim_tick(sim_t *w)
{
    w->in.now_ms += 1000;
    if (!w->rtcm_on) {
        return;
    }
    for (unsigned i = 0; i < K_SET_N; i++) {
        uint16_t m = k_set[i].msg;
        if (m == w->skip || (w->skip_station && (m == 1005 || m == 1033))) {
            continue;
        }
        stn_obs_feed(&w->obs, m, true, (uint32_t)w->in.now_ms, w->in.now_ms + (int64_t)i);   /* one epoch a second */
        w->in.rtcm_frames++;
    }
}

static stn_job_t sim_next(sim_t *w)
{
    stn_job_t j = stn_next(&w->s, &w->in);
    sim_rearm(w);
    if (j.kind != STN_JOB_NONE) {
        w->jobs++;
    }
    return j;
}

/* Run until a job is handed out or max_s seconds pass (then STN_JOB_NONE). */
static stn_job_t sim_until_job(sim_t *w, int max_s)
{
    for (int i = 0; i <= max_s; i++) {
        stn_job_t j = sim_next(w);
        if (j.kind != STN_JOB_NONE) {
            return j;
        }
        sim_tick(w);
    }
    stn_job_t none;
    memset(&none, 0, sizeof(none));
    return none;
}

static void sim_done(sim_t *w, const stn_result_t *r)
{
    stn_done(&w->s, r, &w->in);
    sim_rearm(w);
}

static stn_result_t res(stn_err_t err)
{
    stn_result_t r;
    memset(&r, 0, sizeof(r));
    r.err = (uint8_t)err;
    return r;
}

static void identity_done(sim_t *w, bool supported, bool static_ok)
{
    stn_result_t r = res(STN_OK);
    r.supported = supported;
    r.static_ok = static_ok;
    sim_done(w, &r);
}

/* Boot with a streaming receiver up to the end of the identity query. */
static void boot_streaming(sim_t *w, bool static_ok)
{
    w->rtcm_on = true;
    sim_start(w);
    stn_job_t j = sim_until_job(w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_FALSE(j.after_restart);
    TEST_ASSERT_EQUAL(STN_STREAMING, w->s.state);
    identity_done(w, true, static_ok);
}

/* Boot until the applied check has passed. */
static void boot_applied(sim_t *w)
{
    boot_streaming(w, true);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(w, 40).kind);
    TEST_ASSERT_TRUE(w->s.profile_applied);
}

/* ---- pure helpers ------------------------------------------------------------ */

TEST_CASE("station core: detection candidates and the apply backoff", "[station]")
{
    uint32_t c[8];
    TEST_ASSERT_EQUAL_UINT(4, stn_detect_candidates(0, false, c, 8));
    TEST_ASSERT_EQUAL_UINT32(115200, c[0]);
    TEST_ASSERT_EQUAL_UINT32(460800, c[1]);
    TEST_ASSERT_EQUAL_UINT32(921600, c[2]);
    TEST_ASSERT_EQUAL_UINT32(230400, c[3]);
    /* The stored baud first, no duplicate. */
    TEST_ASSERT_EQUAL_UINT(4, stn_detect_candidates(460800, false, c, 8));
    TEST_ASSERT_EQUAL_UINT32(460800, c[0]);
    TEST_ASSERT_EQUAL_UINT32(115200, c[1]);
    TEST_ASSERT_EQUAL_UINT32(921600, c[2]);
    TEST_ASSERT_EQUAL_UINT(8, stn_detect_candidates(9600, true, c, 8));
    TEST_ASSERT_EQUAL_UINT32(9600, c[0]);
    TEST_ASSERT_EQUAL_UINT32(57600, c[5]);
    TEST_ASSERT_EQUAL_UINT32(19200, c[7]);
    /* A stored value the receiver cannot run at is skipped. */
    TEST_ASSERT_EQUAL_UINT(8, stn_detect_candidates(12345, true, c, 8));
    TEST_ASSERT_EQUAL_UINT32(115200, c[0]);
    TEST_ASSERT_EQUAL_UINT(2, stn_detect_candidates(0, true, c, 2));

    TEST_ASSERT_EQUAL_INT64(10 * MIN_MS, stn_apply_backoff_ms(1));
    TEST_ASSERT_EQUAL_INT64(30 * MIN_MS, stn_apply_backoff_ms(2));
    TEST_ASSERT_EQUAL_INT64(2 * HOUR_MS, stn_apply_backoff_ms(3));
    TEST_ASSERT_EQUAL_INT64(6 * HOUR_MS, stn_apply_backoff_ms(4));
    TEST_ASSERT_EQUAL_INT64(6 * HOUR_MS, stn_apply_backoff_ms(400));
}

/* ---- stream-first boot --------------------------------------------------------- */

TEST_CASE("station core: stream-first boot that matches sends nothing configuring", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);   /* not started */
    boot_applied(&w);
    TEST_ASSERT_TRUE(w.s.identity_ok);
    TEST_ASSERT_FALSE(w.s.paused);
    /* Hours of streaming: no job at all. */
    uint32_t jobs = w.jobs;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 3 * 3600).kind);
    TEST_ASSERT_EQUAL_UINT32(jobs, w.jobs);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

TEST_CASE("station core: two checksum-valid lines are enough to stream", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    w.in.text_lines = 1;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
    w.in.now_ms += 1000;
    w.in.text_lines = 2;
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
}

TEST_CASE("station core: a stored hash or firmware that differs applies at once", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_streaming(&w, false);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_CHECK, j.why);
    TEST_ASSERT_EQUAL(STN_CHANGE_NONE, j.change);
    TEST_ASSERT_FALSE(j.force_mode);
    TEST_ASSERT_EQUAL(STN_CONFIGURING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
    /* The post-apply check passes on the next window. */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 40).kind);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

TEST_CASE("station core: an observed set that differs at boot applies", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.skip = 1097;   /* Galileo MSM missing: signal group or message set drifted */
    boot_streaming(&w, true);
    stn_job_t j = sim_until_job(&w, 40);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_CHECK, j.why);
    TEST_ASSERT_EQUAL_UINT16(1097, w.s.drift_msg);
}

TEST_CASE("station core: autocfg off raises nothing but sends nothing; the user's apply still runs", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 120).kind);
    TEST_ASSERT_FALSE(w.s.profile_applied);
    TEST_ASSERT_TRUE(stn_reconfigure_refused(&w.s, w.in.autocfg));
    TEST_ASSERT_EQUAL(STN_E_FAIL, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, w.in.autocfg));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);

    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
}

TEST_CASE("station core: an unsupported model streams only", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.rtcm_on = true;
    w.skip = 1097;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, false, false);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_UNSUPPORTED_MODEL);
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 200).kind);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.profile_applied);
}

TEST_CASE("station core: a failed version query keeps streaming and retries every 10 min", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_NOT_READY });
    TEST_ASSERT_FALSE(w.s.identity_ok);
    int64_t t = w.in.now_ms;
    stn_job_t j = sim_until_job(&w, 700);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_INT64_WITHIN(1000, 10 * MIN_MS, w.in.now_ms - t);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    /* Without an identity nothing is applied, whatever the stream shows. */
    TEST_ASSERT_FALSE(w.s.apply_wanted);
}

TEST_CASE("station core: with no model ever stored, one leased version query", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.leased);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    j = sim_until_job(&w, 700);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);   /* the lease was tried once */
}

/* ---- drift ------------------------------------------------------------------------- */

TEST_CASE("station core: two drifted windows in a row re-apply, one does not", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);

    /* One bad window, then good again: no apply. */
    w.skip = 1087;
    int guard = 0;
    while (w.s.drift_windows == 0 && guard++ < 80) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
    TEST_ASSERT_EQUAL_UINT8(1, w.s.drift_windows);
    w.skip = 0;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 100).kind);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.drift_windows);
    TEST_ASSERT_TRUE(w.s.profile_applied);

    /* Persistent drift: re-apply. */
    w.skip = 1087;
    stn_job_t j = sim_until_job(&w, 120);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_DRIFT, j.why);
    TEST_ASSERT_FALSE(w.s.profile_applied);
}

TEST_CASE("station core: an apply whose own check fails is not repeated on drift", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    w.skip = 1097;   /* the receiver ignores part of the profile */
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 600).kind);
    TEST_ASSERT_TRUE(w.s.drift_blocked);
    TEST_ASSERT_FALSE(w.s.profile_applied);
    /* A receiver restart lifts it: the applied check runs again. */
    stn_restart_seen(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.after_restart);
    identity_done(&w, true, true);
    TEST_ASSERT_FALSE(w.s.drift_blocked);
    j = sim_until_job(&w, 70);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_CHECK, j.why);
}

TEST_CASE("station core: silence while streaming is a stall, not drift", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    w.rtcm_on = false;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 600).kind);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

/* ---- apply failures ------------------------------------------------------------- */

static void boot_to_failed_apply(sim_t *w, bool answered)
{
    boot_streaming(w, false);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(w).kind);
    w->rtcm_on = false;   /* the failed session left the output silenced */
    stn_result_t r = res(STN_E_FAIL);
    r.answered = answered;
    sim_done(w, &r);
}

TEST_CASE("station core: a failed apply raises CONFIG_FAILED and restores the RTCM output", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_to_failed_apply(&w, true);
    int64_t failed_at = w.in.now_ms;
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_EQUAL_UINT32(600, stn_config_retry_in_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_CONFIGURING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    /* The ladder's reconfigure step is refused meanwhile. */
    TEST_ASSERT_TRUE(stn_reconfigure_refused(&w.s, true));
    TEST_ASSERT_EQUAL(STN_E_FAIL, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    /* 10 s without RTCM, the receiver answered: RTCM outputs only. */
    stn_job_t j = sim_until_job(&w, 15);
    TEST_ASSERT_EQUAL(STN_JOB_RESTORE, j.kind);
    TEST_ASSERT_EQUAL_INT64(STN_LAST_RESORT_MS, w.in.now_ms - failed_at);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
}

TEST_CASE("station core: after a failed apply, RTCM coming back ends the session", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_to_failed_apply(&w, true);
    sim_tick(&w);
    w.rtcm_on = true;
    sim_tick(&w);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
}

TEST_CASE("station core: a receiver that answers nothing after a failed apply is looked for", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_to_failed_apply(&w, false);
    stn_job_t j = sim_until_job(&w, 15);
    TEST_ASSERT_EQUAL(STN_DETECTING, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_FALSE(w.s.paused);

    /* A receiver lost during the session: straight to detection. */
    sim_init(&w, false);
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESYNC, true));   /* queued meanwhile */
    stn_request_lock(&w.s);
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.receiver_lost = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_DETECTING, w.s.state);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    /* A step for the lost receiver is dropped; the user's lock waits for it. */
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_pending);
    TEST_ASSERT_TRUE(w.s.apply_wanted);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, w.s.apply_change);
}

TEST_CASE("station core: apply retries back off 10 min, 30 min, 2 h, then every 6 h", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_to_failed_apply(&w, true);
    int64_t failed_at = w.in.now_ms;
    TEST_ASSERT_EQUAL(STN_JOB_RESTORE, sim_until_job(&w, 15).kind);
    w.rtcm_on = true;
    sim_done(&w, &(stn_result_t){ .err = STN_OK });

    const int64_t expect[] = { 10 * MIN_MS, 30 * MIN_MS, 2 * HOUR_MS, 6 * HOUR_MS, 6 * HOUR_MS };
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        stn_job_t j = sim_until_job(&w, 7 * 3600);
        TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
        TEST_ASSERT_EQUAL(STN_WHY_RETRY, j.why);
        TEST_ASSERT_INT64_WITHIN(1000, expect[i], w.in.now_ms - failed_at);
        failed_at = w.in.now_ms;
        stn_result_t r = res(STN_E_FAIL);
        r.answered = true;
        sim_done(&w, &r);   /* RTCM still flows: the last resort ends at once */
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
    /* A user's apply is never held back by the backoff, and success clears it. */
    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_EQUAL_UINT32(0, stn_config_retry_in_s(&w.s, w.in.now_ms));
    TEST_ASSERT_FALSE(stn_reconfigure_refused(&w.s, true));
}

/* ---- detection ----------------------------------------------------------------- */

static void fail_pass(sim_t *w, bool saw_bytes)
{
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.saw_bytes = saw_bytes;
    sim_done(w, &r);
}

TEST_CASE("station core: nothing in 3 s -> detection; the retry schedule of an absent receiver", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_EQUAL(STN_DETECTING, w.s.state);
    TEST_ASSERT_INT64_WITHIN(1000, STN_LISTEN_MS, w.in.now_ms - T0);
    TEST_ASSERT_FALSE(j.full);
    TEST_ASSERT_TRUE(j.loader_key);
    TEST_ASSERT_FALSE(j.listen_only);
    int64_t episode = w.in.now_ms;
    fail_pass(&w, true);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_TRUE(w.s.detect_saw_bytes);
    TEST_ASSERT_EQUAL_UINT32(1, w.s.detect_failures);

    /* Short, short, full, short, short, full ... every 30 s for 5 min. */
    bool expect_full[] = { false, true, false, false, true, false, false, true };
    for (unsigned i = 0; i < sizeof(expect_full) / sizeof(expect_full[0]); i++) {
        int64_t t = w.in.now_ms;
        j = sim_until_job(&w, 40);
        TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
        TEST_ASSERT_INT64_WITHIN(1000, STN_DETECT_FAST_MS, w.in.now_ms - t);
        TEST_ASSERT_EQUAL(expect_full[i], j.full);
        TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
        fail_pass(&w, false);
    }
    TEST_ASSERT_FALSE(w.s.detect_saw_bytes);   /* the last pass decides */

    /* After 5 min: the full list every 5 min, for days. */
    while (w.in.now_ms - episode < STN_DETECT_FAST_SPAN_MS) {
        j = sim_until_job(&w, 40);
        TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
        fail_pass(&w, false);
    }
    for (int i = 0; i < 3 * 24 * 12; i++) {
        int64_t t = w.in.now_ms;
        j = sim_until_job(&w, 400);
        TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
        TEST_ASSERT_TRUE(j.full);
        TEST_ASSERT_INT64_WITHIN(1000, STN_DETECT_SLOW_MS, w.in.now_ms - t);
        fail_pass(&w, false);
    }
    TEST_ASSERT_TRUE(w.s.detect_failures > 800);
}

TEST_CASE("station core: data between passes, or a found receiver, streams at once", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    fail_pass(&w, false);
    w.in.now_ms += 5000;
    w.in.rtcm_frames += 1;   /* the receiver is back at the stored baud */
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_EQUAL_UINT32(0, w.s.detect_failures);

    /* Found by the probe, with its identity: no separate version query. */
    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    stn_result_t r = res(STN_OK);
    r.identity = true;
    r.supported = true;
    r.static_ok = false;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_TRUE(w.s.identity_ok);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);   /* a silent factory receiver gets configured */
}

TEST_CASE("station core: the loader's restart key goes out once per boot", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_TRUE(j.loader_key);
    /* Prompt, key sent, the application came back: detect again at once. */
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.prompt = true;
    r.key_sent = true;
    r.app_back = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_DETECTING, w.s.state);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_FALSE(j.loader_key);
    /* At the prompt again: no second key; alert and listen only. */
    r = res(STN_E_NOT_FOUND);
    r.prompt = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_RECEIVER_IN_LOADER);
    TEST_ASSERT_TRUE(w.s.detect_saw_bytes);
    for (int i = 0; i < 20; i++) {
        j = sim_until_job(&w, 400);
        TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
        TEST_ASSERT_TRUE(j.listen_only);
        TEST_ASSERT_FALSE(j.loader_key);
        fail_pass(&w, true);
    }
    /* No ladder steps while the station owns the receiver's retries. */
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
}

TEST_CASE("station core: a receiver stuck at its loader waits for the user's restart", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_restart_receiver(&w.s));
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.prompt = true;
    r.key_sent = true;   /* sent, but the application never came */
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_RECEIVER_IN_LOADER);
    TEST_ASSERT_TRUE(w.s.loader_key_sent);

    TEST_ASSERT_EQUAL(STN_OK, stn_request_restart_receiver(&w.s));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_LEAVE_LOADER, j.kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_RECEIVER_IN_LOADER);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);   /* then detection, transmitting again */
    TEST_ASSERT_FALSE(j.listen_only);
    TEST_ASSERT_FALSE(j.loader_key);

    /* Streaming again clears everything. */
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.listen_only);
}

/* ---- the health ladder's requests ------------------------------------------------ */

TEST_CASE("station core: ladder steps run as RECOVERING; weaker ones are ignored", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_recovery(&w.s, STN_REC_RESYNC, true));   /* not running */
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_recovery(&w.s, STN_REC_COUNT, true));

    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESYNC, true));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_RESYNC, j.kind);
    TEST_ASSERT_EQUAL(STN_RECOVERING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESYNC, true));   /* same one running: ignored */
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_pending);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);

    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_LINK_RESTART, true));
    TEST_ASSERT_EQUAL(STN_JOB_LINK_RESTART, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });

    /* A rescan queued, then a resync: only the rescan runs. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESYNC, true));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_TRUE(j.full);
    TEST_ASSERT_TRUE(j.break_first);
    TEST_ASSERT_EQUAL(STN_RECOVERING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    fail_pass(&w, false);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_FALSE(w.s.detect_saw_bytes);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_pending);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_running);
}

TEST_CASE("station core: the ladder's reconfigure and receiver reset", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);

    TEST_ASSERT_EQUAL(STN_E_FAIL, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, false));   /* autocfg off */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_LADDER, j.why);
    TEST_ASSERT_EQUAL(STN_RECOVERING, w.s.state);   /* the ladder's own step: not CONFIGURING */
    TEST_ASSERT_TRUE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_running);

    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECEIVER_RESET, true));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_RECEIVER_RESET, j.kind);
    TEST_ASSERT_EQUAL(STN_RECOVERING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.paused);
    /* As after any receiver restart: identity in restart mode first. */
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.after_restart);
    identity_done(&w, true, true);
    /* The applied check waits 30 s, then needs a window. */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 25).kind);
    TEST_ASSERT_FALSE(w.s.obs_on);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 70).kind);
    TEST_ASSERT_TRUE(w.s.obs_on);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

TEST_CASE("station core: a queued lock rides along in the ladder's reconfigure", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);

    /* The user locks the position; before the station task loops, the
     * health ladder asks for a reconfigure. */
    stn_request_lock(&w.s);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_LADDER, j.why);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);   /* the lock is carried out, not dropped */
    TEST_ASSERT_TRUE(j.force_mode);
    TEST_ASSERT_EQUAL(STN_RECOVERING, w.s.state);
    TEST_ASSERT_FALSE(w.s.apply_wanted);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_running);
    TEST_ASSERT_TRUE(w.s.lock_done);
    TEST_ASSERT_NOT_EQUAL(STN_NEVER, w.s.pos_check_ms);

    /* A re-survey queued the same way. */
    stn_request_resurvey(&w.s);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    TEST_ASSERT_TRUE(j.force_mode);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });

    /* Without a queued request the ladder's apply carries no change. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_CHANGE_NONE, j.change);
    TEST_ASSERT_FALSE(j.force_mode);
}

TEST_CASE("station core: a leased version query pauses the casters while it runs", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    w.rtcm_on = true;
    sim_start(&w);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_FALSE(w.s.paused);   /* a query on the stream holds nothing back */
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    j = sim_next(&w);
    TEST_ASSERT_TRUE(j.leased);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);    /* the lease holds RTCM back */
    identity_done(&w, true, true);
    TEST_ASSERT_FALSE(w.s.paused);
}

/* ---- survey, lock, antenna ------------------------------------------------------- */

TEST_CASE("station core: a running survey found at boot is tracked and locked when done", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.profile_survey = true;
    w.skip_station = true;   /* 1005/1033 held back while surveying */
    boot_streaming(&w, true);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_BOOT, j.mode_why);
    stn_result_t r = res(STN_OK);
    r.mode_survey = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(stn_surveying(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(720, stn_survey_left_s(&w.s, w.in.now_ms));

    /* The applied check passes without the station messages; the base mode is read every minute. */
    j = sim_until_job(&w, 70);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_POLL, j.mode_why);
    TEST_ASSERT_TRUE(w.s.profile_applied);
    r = res(STN_OK);
    r.mode_survey = true;
    sim_done(&w, &r);
    j = sim_until_job(&w, 70);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    /* The read-back shows a fixed base now: survey complete -> lock. */
    r = res(STN_OK);
    r.mode_fixed = true;
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(stn_surveying(&w.s, w.in.now_ms));
    w.skip_station = false;   /* the fixed base sends its station messages */
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_LOCK, j.why);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
    TEST_ASSERT_TRUE(j.force_mode);

    /* No 1005 yet: nothing sent, tried again a minute later. */
    r = res(STN_E_NOT_FOUND);
    r.nothing_sent = true;
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    int64_t t = w.in.now_ms;
    j = sim_until_job(&w, 90);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
    TEST_ASSERT_INT64_WITHIN(1000, STN_LOCK_RETRY_MS, w.in.now_ms - t);

    /* Locked: the profile is fixed now; the next 1005 is compared 30 s later. */
    w.in.profile_survey = false;
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    j = sim_until_job(&w, 40);
    TEST_ASSERT_EQUAL(STN_JOB_CHECK_POSITION, j.kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_MISMATCH });
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_POSITION_MISMATCH);

    /* Re-survey (the user): the alert goes, the survey window opens again. */
    stn_request_resurvey(&w.s);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    TEST_ASSERT_TRUE(j.force_mode);
    w.in.profile_survey = true;
    r = res(STN_OK);
    r.mode_sent = true;
    r.mode_ack_ms = w.in.now_ms;
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_POSITION_MISMATCH);
    TEST_ASSERT_TRUE(stn_surveying(&w.s, w.in.now_ms));
}

TEST_CASE("station core: no automatic lock with autocfg off", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.profile_survey = true;
    w.in.autocfg = false;
    boot_streaming(&w, true);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, sim_next(&w).kind);
    stn_result_t r = res(STN_OK);
    r.mode_fixed = true;   /* the survey finished before this boot */
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 300).kind);
    /* The user's lock still runs. */
    stn_request_lock(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
}

TEST_CASE("station core: a fixed base that stays silent after a restart raises ANTENNA_MOVED", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    w.rtcm_on = false;
    stn_restart_seen(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.after_restart);
    identity_done(&w, true, true);
    j = sim_until_job(&w, 70);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_ANTENNA, j.mode_why);
    stn_result_t r = res(STN_OK);
    r.mode_fixed = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_ANTENNA_MOVED);
    /* Never re-surveyed or re-applied on its own. */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 600).kind);
    /* RTCM back: the alert clears with the next good window. */
    w.rtcm_on = true;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 70).kind);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_ANTENNA_MOVED);
}

TEST_CASE("station core: a restart while surveying opens a new survey window", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.profile_survey = true;
    boot_streaming(&w, true);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });   /* read-back failed: no window */
    TEST_ASSERT_FALSE(stn_surveying(&w.s, w.in.now_ms));
    stn_restart_seen(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_next(&w).kind);
    TEST_ASSERT_TRUE(stn_surveying(&w.s, w.in.now_ms));
    TEST_ASSERT_FALSE(w.s.antenna_check);
}

TEST_CASE("station core: a restart the apply session caused opens a survey window", "[station]")
{
    /* The signal-group reset restores the saved survey-in mode; the base
     * mode read back then matches, so no MODE BASE is sent. */
    sim_t w;
    sim_init(&w, false);
    w.in.profile_survey = true;
    boot_streaming(&w, true);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    TEST_ASSERT_FALSE(stn_surveying(&w.s, w.in.now_ms));
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK, .restarted = true });
    TEST_ASSERT_TRUE(stn_surveying(&w.s, w.in.now_ms));
    TEST_ASSERT_FALSE(w.s.lock_done);

    /* A failed apply whose rollback RESET was answered: surveying again too. */
    sim_init(&w, false);
    w.in.profile_survey = true;
    boot_streaming(&w, true);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_FAIL, .restarted = true, .answered = true });
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_TRUE(stn_surveying(&w.s, w.in.now_ms));

    /* Nothing restarted: no window. */
    sim_init(&w, false);
    w.in.profile_survey = true;
    boot_streaming(&w, true);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(stn_surveying(&w.s, w.in.now_ms));
}

/* ---- flash journal ------------------------------------------------------------- */

TEST_CASE("station core: an open flash journal holds everything until it is cleared", "[station]")
{
    sim_t w;
    sim_init(&w, true);
    TEST_ASSERT_EQUAL(STN_FLASH_RECOVERY, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 3600).kind);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_recovery(&w.s, STN_REC_RESYNC, true));
    stn_request_apply(&w.s);   /* remembered for later */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);

    stn_journal_cleared(&w.s, &w.in);
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    identity_done(&w, true, true);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
}
