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

#include "lifecycle.h"
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

/* The question to a receiver that never streamed (STN_JOB_ASK), answered:
 * its reply is a line like any other. */
static void ask_answered(sim_t *w)
{
    w->in.text_lines++;
    stn_result_t r;
    memset(&r, 0, sizeof(r));
    r.err = STN_OK;
    sim_done(w, &r);
}

/* As sim_until_job, the station's questions answered on the way (counted in *asks when given). */
static stn_job_t sim_until_job_asked(sim_t *w, int max_s, unsigned *asks)
{
    for (int i = 0; i <= max_s; i++) {
        stn_job_t j = sim_next(w);
        if (j.kind == STN_JOB_ASK) {
            if (asks != NULL) {
                (*asks)++;
            }
            ask_answered(w);
        } else if (j.kind != STN_JOB_NONE) {
            return j;
        }
        sim_tick(w);
    }
    stn_job_t none;
    memset(&none, 0, sizeof(none));
    return none;
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

TEST_CASE("station core: RTCM that differs is drift; no RTCM at all never is", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;   /* as on the bench: nothing is applied on its own */
    /* The receiver answers (two lines) but sends no RTCM: no antenna. */
    sim_start(&w);
    w.in.text_lines = 2;
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_next(&w).kind);
    identity_done(&w, true, true);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    unsigned asks = 0;
    for (int i = 0; i < 300; i++) {
        stn_job_t j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            asks++;   /* the station's question: it answers when asked */
            ask_answered(&w);
        } else {
            TEST_ASSERT_EQUAL(STN_JOB_NONE, j.kind);
        }
        TEST_ASSERT_FALSE(w.s.rtcm_differs);
        sim_tick(&w);
    }
    TEST_ASSERT_EQUAL_UINT(4, asks);
    TEST_ASSERT_FALSE(w.s.profile_applied);   /* not applied, but nothing differs either */

    /* RTCM flows with a message missing: that differs (and no question once it flowed). */
    w.rtcm_on = true;
    w.skip = 1097;
    sim_tick(&w);
    for (int i = 0; i < 70 && !w.s.rtcm_differs; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(w.s.rtcm_differs);
    TEST_ASSERT_EQUAL_UINT16(1097, w.s.drift_msg);

    /* The whole set again: the next window passes. */
    w.skip = 0;
    for (int i = 0; i < 70 && w.s.rtcm_differs; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
    TEST_ASSERT_FALSE(w.s.rtcm_differs);

    /* RTCM stops: a stall, not drift (the station asks the receiver). */
    w.skip = 1097;
    for (int i = 0; i < 70 && !w.s.rtcm_differs; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(w.s.rtcm_differs);
    w.rtcm_on = false;
    for (int i = 0; i < 70 && w.s.rtcm_differs; i++) {
        stn_job_t j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            ask_answered(&w);
        } else {
            TEST_ASSERT_EQUAL(STN_JOB_NONE, j.kind);
        }
        sim_tick(&w);
    }
    TEST_ASSERT_FALSE(w.s.rtcm_differs);
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

/* Boot into STREAMING on text lines alone (a receiver that answers but sends no RTCM). */
static void boot_lines_only(sim_t *w)
{
    w->rtcm_on = false;
    sim_start(w);
    w->in.text_lines += 2;
}

TEST_CASE("station core: with no model ever stored and nothing flowing, one leased version query", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    boot_lines_only(&w);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.leased);
    TEST_ASSERT_FALSE(w.s.paused);   /* nothing flows: nothing to hold back, nothing planned */
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    TEST_ASSERT_FALSE(w.s.paused);
    /* Exactly one per boot: every later query goes without a lease. */
    unsigned leased = 0;
    unsigned queries = 0;
    for (int k = 0; k < 6; k++) {
        j = sim_until_job_asked(&w, 700, NULL);
        TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
        leased += j.leased ? 1u : 0u;
        queries++;
        TEST_ASSERT_FALSE(w.s.paused);
        sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    }
    TEST_ASSERT_EQUAL_UINT(6, queries);
    TEST_ASSERT_EQUAL_UINT(0, leased);
}

TEST_CASE("station core: while RTCM flows the identity query never takes a lease or pauses the casters", "[station]")
{
    /* No model ever stored, the receiver streams but never answers the
     * version query: three hours of unleased retries every 10 min. */
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    w.rtcm_on = true;
    sim_start(&w);
    int64_t start = w.in.now_ms;
    int64_t last = -1;
    unsigned queries = 0;
    while (w.in.now_ms - start < 3 * HOUR_MS) {
        stn_job_t j = sim_next(&w);
        TEST_ASSERT_FALSE(w.s.paused);
        if (j.kind == STN_JOB_IDENTIFY) {
            TEST_ASSERT_FALSE(j.leased);
            if (last >= 0) {
                TEST_ASSERT_INT64_WITHIN(1000, STN_IDENTITY_RETRY_MS, w.in.now_ms - last);
            }
            last = w.in.now_ms;
            queries++;
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
            TEST_ASSERT_FALSE(w.s.paused);
        } else {
            TEST_ASSERT_EQUAL(STN_JOB_NONE, j.kind);
        }
        sim_tick(&w);
    }
    TEST_ASSERT_EQUAL_UINT(18, queries);   /* at 0, 10, ... 170 min */
    TEST_ASSERT_FALSE(w.s.leased_tried);
    TEST_ASSERT_FALSE(w.s.identity_ok);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);

    /* RTCM stops for good: the next failure takes the boot's one lease
     * (nothing left to hold back), still without pausing. (The station's
     * question, without a lease, goes unanswered meanwhile.) */
    w.rtcm_on = false;
    /* The query due now still saw the last frames arrive: no lease yet. */
    stn_job_t j = sim_until_job(&w, 700);
    if (j.kind == STN_JOB_ASK) {
        TEST_ASSERT_FALSE(w.s.paused);
        sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        j = sim_until_job(&w, 700);
    }
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    /* Ten minutes later nothing has flowed since. */
    j = sim_until_job(&w, 700);
    while (j.kind == STN_JOB_ASK) {
        TEST_ASSERT_FALSE(w.s.paused);
        sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        j = sim_until_job(&w, 700);
    }
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.leased);
    TEST_ASSERT_FALSE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
}

TEST_CASE("station core: RTCM flowing after the leased query: still no lease", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_TRUE(j.leased);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    w.rtcm_on = true;
    for (int k = 0; k < 3; k++) {
        j = sim_until_job(&w, 700);
        TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
        TEST_ASSERT_FALSE(j.leased);
        TEST_ASSERT_FALSE(w.s.paused);
        sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    }

    /* A lease due while nothing flowed, handed out after RTCM came back:
     * the query goes without it. */
    sim_init(&w, false);
    w.in.model_known = false;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    TEST_ASSERT_TRUE(w.s.identify_leased);
    w.in.rtcm_frames += 3;   /* frames arrive before the job is handed out */
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_FALSE(w.s.paused);
}

TEST_CASE("station core: a stored model is never queried with a lease", "[station]")
{
    for (int flowing = 0; flowing < 2; flowing++) {
        sim_t w;
        sim_init(&w, false);
        w.in.model_known = true;
        if (flowing) {
            w.rtcm_on = true;
            sim_start(&w);
        } else {
            boot_lines_only(&w);
        }
        for (int k = 0; k < 4; k++) {
            stn_job_t j = sim_until_job_asked(&w, 700, NULL);
            TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
            TEST_ASSERT_FALSE(j.leased);
            TEST_ASSERT_FALSE(w.s.paused);
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        }
    }
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
    unsigned asks = 0;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job_asked(&w, 600, &asks).kind);   /* only the station's question */
    TEST_ASSERT_TRUE(asks >= 1);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

static bool answers_now(const sim_t *w, int64_t rtcm_at, int64_t line_at);

/* No antenna (the board, 2026-10-16): the receiver answers whatever it is
 * asked and sends no RTCM, and it acknowledged this profile on this firmware
 * at an earlier apply (the applied state matches). Every job the station
 * hands out until `until` (or `max_s` seconds) is run: its questions are
 * answered, the ladder's steps find the receiver and answer, and every apply
 * is counted (and succeeds). */
static unsigned noant_run(sim_t *w, int max_s, unsigned *asks)
{
    unsigned applies = 0;
    for (int i = 0; i < max_s; i++) {
        stn_job_t j = sim_next(w);
        switch (j.kind) {
        case STN_JOB_NONE:
            break;
        case STN_JOB_ASK:
            if (asks != NULL) {
                (*asks)++;
            }
            ask_answered(w);
            break;
        case STN_JOB_APPLY:
            applies++;
            w->in.text_lines += 8;   /* its replies */
            sim_done(w, &(stn_result_t){ .err = STN_OK, .answered = true });
            break;
        case STN_JOB_IDENTIFY: {
            w->in.text_lines++;
            identity_done(w, true, true);
            break;
        }
        case STN_JOB_DETECT: {
            w->in.text_lines += 2;
            stn_result_t r = res(STN_OK);
            r.identity = true;
            r.supported = true;
            r.static_ok = true;
            sim_done(w, &r);
            break;
        }
        default:
            w->in.text_lines++;
            sim_done(w, &(stn_result_t){ .err = STN_OK });
            break;
        }
        sim_tick(w);
    }
    return applies;
}

TEST_CASE("station core: no RTCM from a receiver that acknowledged the profile: the check waits, nothing re-applied", "[station]")
{
    /* The board's run: the check found no RTCM after each rescan and after
     * the receiver reset, and re-applied every time (each apply cut the
     * ladder's step short). A window without RTCM says nothing about what the
     * receiver runs, and a re-apply could not be checked: the check waits
     * for a window with RTCM; the stall is the ladder's. */
    sim_t w;
    sim_init(&w, false);
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;
    identity_done(&w, true, true);   /* the applied state matches */
    unsigned asks = 0;
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 10 * 60, &asks));
    TEST_ASSERT_TRUE(asks >= 9);
    TEST_ASSERT_TRUE(w.s.check_pending);
    TEST_ASSERT_FALSE(w.s.apply_wanted);

    /* The ladder's rescan finds it (a new check), its link restart, its
     * receiver reset (restart evidence, the check 30 s later): still nothing. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 5 * 60, NULL));
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECEIVER_RESET, true));
    TEST_ASSERT_EQUAL(STN_JOB_RECEIVER_RESET, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    stn_restart_seen(&w.s);   /* its banner */
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 10 * 60, NULL));
    TEST_ASSERT_TRUE(w.s.check_pending);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);

    /* The ladder's own reconfigure still runs (one apply, its step). */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECONFIGURE, true));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_LADDER, j.why);
    sim_done(&w, &(stn_result_t){ .err = STN_OK, .answered = true });
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 10 * 60, NULL));

    /* The antenna back, RTCM flows and differs from the profile: the
     * check that waited decides at its first window (no second drifted
     * window needed). */
    stn_restart_seen(&w.s);
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 40, NULL));
    w.rtcm_on = true;
    w.skip = 1097;
    j = sim_until_job_asked(&w, 70, NULL);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_CHECK, j.why);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });

    /* The same receiver, the whole set once RTCM flows: applied, nothing sent. */
    sim_init(&w, false);
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;
    identity_done(&w, true, true);
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 5 * 60, NULL));
    TEST_ASSERT_FALSE(w.s.profile_applied);
    w.rtcm_on = true;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job_asked(&w, 70, NULL).kind);
    TEST_ASSERT_TRUE(w.s.profile_applied);
    TEST_ASSERT_FALSE(w.s.check_pending);

    /* A receiver that never acknowledged this profile (the applied state
     * differs): applied at once, whatever flows. */
    sim_init(&w, false);
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, true, false);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_CHECK, j.why);
}

TEST_CASE("station core: a profile the receiver acknowledged reads so without RTCM; a saved one that differs does not",
          "[station]")
{
    /* The board without an antenna: the recovery's reconfigure applied the
     * profile and the receiver acknowledged it; no window can check it. */
    sim_t w;
    sim_init(&w, false);
    boot_lines_only(&w);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));   /* not identified yet */
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;
    identity_done(&w, true, false);   /* never acknowledged this profile */
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    w.in.text_lines += 8;
    sim_done(&w, &(stn_result_t){ .err = STN_OK, .answered = true });
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 10 * 60, NULL));
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));
    TEST_ASSERT_FALSE(w.s.profile_applied);              /* not checked: no RTCM */

    /* The user saves a profile that differs: not what the receiver acknowledged. The save's own apply runs. */
    stn_profile_changed(&w.s);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
    TEST_ASSERT_FALSE(w.s.profile_applied);
    stn_request_apply(&w.s);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
    sim_done(&w, &(stn_result_t){ .err = STN_OK, .answered = true });
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));

    /* Saved while nothing could apply it (maintenance refuses the request): the next check without RTCM applies it,
     * where it waited for RTCM for the profile the receiver acknowledged. */
    stn_restart_seen(&w.s);
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 5 * 60, NULL));
    TEST_ASSERT_TRUE(w.s.check_pending);
    stn_profile_changed(&w.s);
    TEST_ASSERT_EQUAL_UINT(1, noant_run(&w, 2 * 60, NULL));
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));

    /* RTCM with the whole set: applied, and still acknowledged. */
    w.rtcm_on = true;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job_asked(&w, 70, NULL).kind);
    TEST_ASSERT_TRUE(w.s.profile_applied);
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));

    /* The receiver lost (a rescan finds nothing): not known to hold anything, until it is found and identified. */
    w.rtcm_on = false;
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_E_NOT_FOUND });
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
    stn_result_t found = res(STN_OK);
    found.identity = true;
    found.supported = true;
    found.static_ok = true;
    stn_job_t d = sim_until_job(&w, 60);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, d.kind);
    sim_done(&w, &found);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));

    /* A model the station does not configure is never acknowledged. */
    sim_init(&w, false);
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, false, true);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
}

TEST_CASE("station core: no RTCM from an acknowledged profile with the recovery on alerts only: one apply per check",
          "[station]")
{
    /* The ladder only alerts and re-applies nothing, so the check does, once: a receiver that lost its configuration
     * (a factory reset over its other port) would stay unconfigured. The window after the apply is its own check,
     * which a window without RTCM does not repeat. */
    sim_t w;
    sim_init(&w, false);
    w.in.ladder_alerts_only = true;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;
    identity_done(&w, true, true);   /* the applied state matches */
    TEST_ASSERT_EQUAL_UINT(1, noant_run(&w, 10 * 60, NULL));
    TEST_ASSERT_FALSE(w.s.check_pending);
    TEST_ASSERT_FALSE(w.s.apply_wanted);
    TEST_ASSERT_FALSE(w.s.profile_applied);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);

    /* A receiver restart (its banner): a new check, one apply again. */
    stn_restart_seen(&w.s);
    TEST_ASSERT_EQUAL_UINT(1, noant_run(&w, 10 * 60, NULL));

    /* The configuration back after it, RTCM flows: nothing more. */
    stn_restart_seen(&w.s);
    w.rtcm_on = true;
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 5 * 60, NULL));
    TEST_ASSERT_TRUE(w.s.profile_applied);

    /* Without automatic configuration nothing applies: the check waits for RTCM. */
    sim_init(&w, false);
    w.in.ladder_alerts_only = true;
    w.in.autocfg = false;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;
    identity_done(&w, true, true);
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 10 * 60, NULL));
    TEST_ASSERT_TRUE(w.s.check_pending);
}

TEST_CASE("station core: a receiver whose RTCM stopped is asked at once, then once a minute while nothing comes", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    w.rtcm_on = false;
    int64_t stop = w.in.now_ms;
    stn_job_t j = sim_until_job(&w, 10);
    TEST_ASSERT_EQUAL(STN_JOB_ASK, j.kind);
    TEST_ASSERT_TRUE(w.in.now_ms - stop >= STN_RTCM_QUIET_MS && w.in.now_ms - stop <= STN_RTCM_QUIET_MS + 1000);
    TEST_ASSERT_FALSE(w.s.paused);
    ask_answered(&w);
    int64_t line = w.in.now_ms;
    int64_t rtcm_at = stop - 1000;   /* the last tick that fed frames */

    /* It answers each question: answering for an hour, a question a minute. */
    unsigned asks = 0;
    for (int i = 0; i < 3600; i++) {
        j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            asks++;
            TEST_ASSERT_TRUE(w.in.now_ms - line >= STN_ASK_MS);
            ask_answered(&w);
            line = w.in.now_ms;
        } else {
            TEST_ASSERT_EQUAL(STN_JOB_NONE, j.kind);
        }
        TEST_ASSERT_TRUE_MESSAGE(answers_now(&w, rtcm_at, line), "answers between two questions");
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(asks >= 58 && asks <= 60);

    /* Its NMEA every 10 s: no question while it comes. */
    for (int i = 0; i < 600; i++) {
        if (i % 10 == 0) {
            w.in.text_lines++;
            line = w.in.now_ms;
        }
        TEST_ASSERT_NOT_EQUAL(STN_JOB_ASK, sim_next(&w).kind);
        sim_tick(&w);
    }

    /* Unplugged: the questions go on unanswered; it stops answering
     * STN_ANSWER_FRESH_MS after its last answer. */
    unsigned unanswered = 0;
    for (int i = 0; i < 5 * 60; i++) {
        j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            unanswered++;
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        }
        TEST_ASSERT_EQUAL(w.in.now_ms - line <= STN_ANSWER_FRESH_MS, answers_now(&w, rtcm_at, line));
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(unanswered >= 4 && unanswered <= 5);

    /* RTCM back: no question while it flows. */
    w.rtcm_on = true;
    sim_tick(&w);
    for (int i = 0; i < 600; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
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
    TEST_ASSERT_EQUAL_UINT32(600, stn_config_retry_in_s(&w.s, true, w.in.now_ms));
    TEST_ASSERT_TRUE(stn_config_auto_retry(&w.s, true));
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

/* The APPLY_FAILED note among the notes taken now (NULL if none). */
static const stn_note_t *apply_failed_note(sim_t *w, stn_note_t *buf)
{
    unsigned c = stn_take_notes(&w->s, buf, STN_NOTES);
    for (unsigned i = 0; i < c; i++) {
        if (buf[i].kind == STN_NOTE_APPLY_FAILED) {
            return &buf[i];
        }
    }
    return NULL;
}

TEST_CASE("station core: a failed apply's log says what follows: the station's retry, or none", "[station]")
{
    stn_note_t buf[STN_NOTES];
    char text[128];

    /* Automatic configuration on: the station retries in 10 min. */
    sim_t w;
    sim_init(&w, false);
    boot_to_failed_apply(&w, true);
    const stn_note_t *n = apply_failed_note(&w, buf);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT32(600, n->a);
    TEST_ASSERT_EQUAL_INT32(0, n->b);
    stn_apply_next_text((uint32_t)n->a, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("the station tries again in 10 min", text);

    /* Off (the bench's case, the user's apply failing on a refused type):
     * no retry is pending, and the log must not promise one. */
    sim_init(&w, false);
    w.in.autocfg = false;
    boot_streaming(&w, false);
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    stn_result_t r = res(STN_E_FAIL);
    r.answered = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_FALSE(stn_config_auto_retry(&w.s, w.in.autocfg));
    TEST_ASSERT_EQUAL_UINT32(0, stn_config_retry_in_s(&w.s, w.in.autocfg, w.in.now_ms));
    n = apply_failed_note(&w, buf);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT32(0, n->a);
    stn_apply_next_text((uint32_t)n->a, text, sizeof(text));
    TEST_ASSERT_NULL(strstr(text, "later"));
    TEST_ASSERT_NULL(strstr(text, "tries again"));
    TEST_ASSERT_EQUAL_STRING("nothing retries it on its own (automatic configuration is off): apply it again from the "
                             "receiver page",
                             text);

    /* A receiver lost in the session says so. */
    sim_init(&w, false);
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    r = res(STN_E_NOT_FOUND);
    r.receiver_lost = true;
    sim_done(&w, &r);
    n = apply_failed_note(&w, buf);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT32(1, n->b);

    /* Nothing sent (no lease): no such note. */
    sim_init(&w, false);
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    r = res(STN_E_STATE);
    r.nothing_sent = true;
    sim_done(&w, &r);
    TEST_ASSERT_NULL(apply_failed_note(&w, buf));

    /* The later steps of the backoff, in words. */
    stn_apply_next_text(1800, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("the station tries again in 30 min", text);
    stn_apply_next_text(7200, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("the station tries again in 2 h", text);
    stn_apply_next_text(21600, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING("the station tries again in 6 h", text);
    TEST_ASSERT_EQUAL_size_t(4, stn_apply_next_text(600, text, 5));
    TEST_ASSERT_EQUAL_STRING("the ", text);
}

TEST_CASE("station core: a refused signal group goes back without CONFIG_FAILED and is re-applied at once", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_request_apply(&w.s);   /* the user saved a profile with 7 0 */
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_TRUE(w.s.paused);
    /* The receiver refused the group; the caller wrote the group it runs
     * back into the profile. */
    stn_result_t r = res(STN_E_FAIL);
    r.sg_back = true;
    r.restarted = true;
    r.answered = true;
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_EQUAL_INT64(STN_NEVER, w.s.apply_retry_ms);
    TEST_ASSERT_FALSE(w.s.last_resort);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    /* The re-apply runs right away, with its own reason. */
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_SG_BACK, j.why);
    TEST_ASSERT_TRUE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    /* No backoff retry of the refused pair, ever. */
    for (int k = 0; k < 7 * 3600; k++) {
        stn_job_t n = sim_next(&w);
        TEST_ASSERT_NOT_EQUAL(STN_JOB_APPLY, n.kind);
        if (n.kind != STN_JOB_NONE) {
            sim_done(&w, &(stn_result_t){ .err = STN_OK });
        }
        sim_tick(&w);
    }

    /* The comparison's own switch keeps its reason for the re-apply. */
    sim_init(&w, false);
    boot_applied(&w);
    stn_request_apply_why(&w.s, STN_WHY_SG_TEST);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_SG_TEST, j.why);
    sim_done(&w, &r);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_SG_TEST, j.why);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);

    /* An internal reason replaces the reason of a re-apply already queued. */
    sim_init(&w, false);
    boot_applied(&w);
    stn_request_apply(&w.s);
    stn_request_apply_why(&w.s, STN_WHY_SG_TEST);
    TEST_ASSERT_EQUAL(STN_WHY_SG_TEST, sim_next(&w).why);
}

TEST_CASE("station core: the group a refused signal group goes back to", "[station]")
{
    uint8_t m = 0, sl = 0;
    /* The session's read-back first. */
    TEST_ASSERT_TRUE(stn_sg_fallback(7, 0, true, 3, 6, true, (4u << 8) | 5u, &m, &sl));
    TEST_ASSERT_EQUAL_UINT8(3, m);
    TEST_ASSERT_EQUAL_UINT8(6, sl);
    /* Not read: the group of the last successful apply. */
    TEST_ASSERT_TRUE(stn_sg_fallback(7, 0, false, 0, 0, true, (4u << 8) | 5u, &m, &sl));
    TEST_ASSERT_EQUAL_UINT8(4, m);
    TEST_ASSERT_EQUAL_UINT8(5, sl);
    /* Never the refused pair itself. */
    TEST_ASSERT_TRUE(stn_sg_fallback(7, 0, true, 7, 0, true, (3u << 8) | 6u, &m, &sl));
    TEST_ASSERT_EQUAL_UINT8(3, m);
    TEST_ASSERT_FALSE(stn_sg_fallback(7, 0, true, 7, 0, true, (7u << 8) | 0u, &m, &sl));
    /* Neither known: an ordinary failed apply. */
    TEST_ASSERT_FALSE(stn_sg_fallback(7, 0, false, 0, 0, false, 0, &m, &sl));

    /* ... which raises CONFIG_FAILED as before. */
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    stn_result_t r = res(STN_E_FAIL);
    r.answered = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_NOT_EQUAL(STN_NEVER, w.s.apply_retry_ms);
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
        stn_job_t j = sim_until_job_asked(&w, 7 * 3600, NULL);   /* the question while RTCM comes back */
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
    TEST_ASSERT_EQUAL_UINT32(0, stn_config_retry_in_s(&w.s, true, w.in.now_ms));
    TEST_ASSERT_FALSE(stn_reconfigure_refused(&w.s, true));
}

TEST_CASE("station core: with automatic configuration off (held) a failed apply is never retried, no countdown",
          "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;   /* as the first-apply hold writes it */
    stn_set_first_apply_held(&w.s, true);
    boot_streaming(&w, false);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 60).kind);   /* held: nothing applied on its own */

    /* The user's "Configure the receiver now" fails. */
    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
    stn_result_t r = res(STN_E_FAIL);
    r.answered = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_FALSE(stn_config_auto_retry(&w.s, false));
    TEST_ASSERT_EQUAL_UINT32(0, stn_config_retry_in_s(&w.s, false, w.in.now_ms));

    /* Seven hours: no apply of its own, and never a countdown that promises one. */
    int64_t until = w.in.now_ms + 7 * HOUR_MS;
    while (w.in.now_ms < until) {
        j = sim_until_job(&w, 600);
        TEST_ASSERT_NOT_EQUAL(STN_JOB_APPLY, j.kind);
        TEST_ASSERT_EQUAL_UINT32(0, stn_config_retry_in_s(&w.s, false, w.in.now_ms));
        if (j.kind != STN_JOB_NONE) {
            sim_done(&w, &(stn_result_t){ .err = STN_OK });
        }
    }
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);

    /* The user turns automatic configuration on: the backoff's retry is real again, and it runs. */
    w.in.autocfg = true;
    TEST_ASSERT_TRUE(stn_config_auto_retry(&w.s, true));
    uint32_t left = stn_config_retry_in_s(&w.s, true, w.in.now_ms);
    TEST_ASSERT_TRUE(left > 0 && left <= 6 * 3600);
    j = sim_until_job(&w, 6 * 3600 + 60);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_RETRY, j.why);
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

/* ---- whether the receiver answers ------------------------------------------------- */

/* stn_responding with the last RTCM frame at rtcm_at and the last line at line_at (STN_NEVER: none this boot). */
static bool answers_now(const sim_t *w, int64_t rtcm_at, int64_t line_at)
{
    uint32_t rtcm_age = rtcm_at == STN_NEVER ? UINT32_MAX : (uint32_t)(w->in.now_ms - rtcm_at);
    return stn_responding(&w->s, w->in.now_ms, rtcm_age, line_at);
}

TEST_CASE("station core: the receiver answers: it streams, or a line came after its last RTCM frame", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    int64_t now = w.in.now_ms;
    TEST_ASSERT_TRUE(answers_now(&w, now - 500, STN_NEVER));               /* it streams */
    TEST_ASSERT_TRUE(answers_now(&w, now - STN_FRESH_MS, STN_NEVER));
    TEST_ASSERT_FALSE(answers_now(&w, now - STN_FRESH_MS - 1, STN_NEVER));  /* RTCM stopped, nothing since */
    /* Lines from before the last frame, or with it (the same output), are no answer after it. */
    TEST_ASSERT_FALSE(answers_now(&w, now - 20000, now - 20500));
    TEST_ASSERT_FALSE(answers_now(&w, now - 20000, now - 20000 + STN_ANSWER_SLACK_MS));
    /* A line after it (a reply, its NMEA) is, while it is at most
     * STN_ANSWER_FRESH_MS old (the station asks every STN_ASK_MS meanwhile):
     * an older one says nothing of now (unplugged, hung). */
    TEST_ASSERT_TRUE(answers_now(&w, now - 20000, now - 20000 + STN_ANSWER_SLACK_MS + 1));
    TEST_ASSERT_TRUE(answers_now(&w, now - 3 * HOUR_MS, now - STN_ANSWER_FRESH_MS));
    TEST_ASSERT_FALSE(answers_now(&w, now - 3 * HOUR_MS, now - STN_ANSWER_FRESH_MS - 1));
    TEST_ASSERT_FALSE(answers_now(&w, now - 3 * HOUR_MS, now - 2 * HOUR_MS));
    /* No RTCM frame this boot: a line within STN_ANSWER_FRESH_MS (the station asks every STN_ASK_MS). */
    TEST_ASSERT_TRUE(answers_now(&w, STN_NEVER, now - STN_ANSWER_FRESH_MS));
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, now - STN_ANSWER_FRESH_MS - 1));
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, now - HOUR_MS));
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, STN_NEVER));
}

TEST_CASE("station core: a receiver that answers questions and sends nothing unasked answers steadily", "[station]")
{
    /* No antenna: no RTCM this boot, a reply only when asked. The station
     * asks once a minute while no line comes (STN_JOB_ASK, after its other
     * jobs): every second of two hours it answers, no flicker between two
     * questions. */
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;   /* nothing applied: only the questions */
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    w.in.text_lines++;   /* its reply */
    identity_done(&w, true, true);
    int64_t line = w.in.now_ms;
    unsigned asks = 0;
    for (int i = 0; i < 2 * 3600; i++) {
        stn_job_t j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            asks++;
            TEST_ASSERT_TRUE(w.in.now_ms - line >= STN_ASK_MS);
            ask_answered(&w);
            line = w.in.now_ms;
        } else if (j.kind != STN_JOB_NONE) {
            identity_done(&w, true, true);
        }
        TEST_ASSERT_TRUE_MESSAGE(answers_now(&w, STN_NEVER, line), "answers between two questions");
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(asks >= 115 && asks <= 120);

    /* It hangs: the questions go on, unanswered, and it stops answering
     * once its last answer is STN_ANSWER_FRESH_MS old. */
    int64_t last = line;
    unsigned unanswered = 0;
    for (int i = 0; i < 10 * 60; i++) {
        stn_job_t j = sim_next(&w);
        if (j.kind == STN_JOB_ASK) {
            unanswered++;
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        } else if (j.kind != STN_JOB_NONE) {
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
        }
        TEST_ASSERT_EQUAL(w.in.now_ms - last <= STN_ANSWER_FRESH_MS, answers_now(&w, STN_NEVER, last));
        sim_tick(&w);
    }
    TEST_ASSERT_TRUE(unanswered >= 9 && unanswered <= 10);
}

TEST_CASE("station core: the question only to a receiver that sends no RTCM and nothing else", "[station]")
{
    /* A receiver that sends a line every second (NMEA, no RTCM): never asked. */
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, true, true);
    for (int i = 0; i < 600; i++) {
        stn_job_t j = sim_next(&w);
        TEST_ASSERT_NOT_EQUAL(STN_JOB_ASK, j.kind);
        if (j.kind != STN_JOB_NONE) {
            identity_done(&w, true, true);
        }
        w.in.text_lines++;
        sim_tick(&w);
    }

    /* Streaming RTCM, no line for hours: never asked. */
    sim_init(&w, false);
    boot_applied(&w);
    for (int i = 0; i < 3 * 3600; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }

    /* In receiver maintenance: nothing at all, the question neither. */
    sim_init(&w, false);
    w.in.autocfg = false;
    boot_lines_only(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, true, true);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    for (int i = 0; i < 800; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        sim_tick(&w);
    }
}

TEST_CASE("station core: a receiver not found does not answer, and earlier lines do not count after it", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    int64_t line = w.in.now_ms;   /* something readable at boot, then nothing */
    TEST_ASSERT_TRUE(answers_now(&w, STN_NEVER, line));
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    fail_pass(&w, true);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, line));
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, w.in.now_ms));   /* never while none is found */

    /* Its lines again at the stored baud: streaming, and it answers; the old line still does not count. */
    w.in.now_ms += 5000;
    w.in.text_lines += 2;
    (void)sim_next(&w);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, line));
    TEST_ASSERT_TRUE(answers_now(&w, STN_NEVER, w.in.now_ms - 1000));

    /* A receiver update that left it at its loader is the same: no answer from before it. */
    sim_init(&w, false);
    boot_applied(&w);
    int64_t before = w.in.now_ms - 1000;
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    stn_result_t r = res(STN_OK);
    r.flash_end = (uint8_t)STN_FLASH_END_NOTHING;
    r.supported = true;
    r.in_loader = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    w.in.now_ms += 1000;
    w.rtcm_on = false;
    TEST_ASSERT_FALSE(answers_now(&w, STN_NEVER, before));
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

TEST_CASE("station core: a leased version query does not pause the casters", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.model_known = false;
    boot_lines_only(&w);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_FALSE(w.s.paused);   /* a query on the stream holds nothing back */
    sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });
    j = sim_next(&w);
    TEST_ASSERT_TRUE(j.leased);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);   /* nothing flows: the outage stays unplanned */
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
    j = sim_until_job_asked(&w, 70, NULL);   /* it answers the station's question meanwhile */
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_ANTENNA, j.mode_why);
    stn_result_t r = res(STN_OK);
    r.mode_fixed = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_ANTENNA_MOVED);
    /* Never re-surveyed or re-applied on its own. */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job_asked(&w, 600, NULL).kind);
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

/* ---- receiver maintenance -------------------------------------------------------- */

static bool has_note(sim_t *w, stn_note_kind_t kind, stn_note_t *out)
{
    stn_note_t n[STN_NOTES];
    unsigned c = stn_take_notes(&w->s, n, STN_NOTES);
    bool found = false;
    for (unsigned i = 0; i < c; i++) {
        if (n[i].kind == kind) {
            if (out != NULL) {
                *out = n[i];
            }
            found = true;
        }
    }
    return found;
}

TEST_CASE("station core: maintenance durations and entry refusals", "[station]")
{
    TEST_ASSERT_TRUE(stn_maint_duration_ok(15 * 60));
    TEST_ASSERT_TRUE(stn_maint_duration_ok(30 * 60));
    TEST_ASSERT_TRUE(stn_maint_duration_ok(3600));
    TEST_ASSERT_TRUE(stn_maint_duration_ok(2 * 3600));
    TEST_ASSERT_TRUE(stn_maint_duration_ok(4 * 3600));
    TEST_ASSERT_FALSE(stn_maint_duration_ok(0));
    TEST_ASSERT_FALSE(stn_maint_duration_ok(45 * 60));
    TEST_ASSERT_FALSE(stn_maint_duration_ok(8 * 3600));
    TEST_ASSERT_EQUAL_UINT32(30 * 60, STN_MAINT_DEFAULT_S);

    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_MAINT_DURATION, stn_maint_enter(&w.s, 1000, false, w.in.now_ms));
    /* A lease held, or a job of the station running: offer again when it ends. */
    TEST_ASSERT_EQUAL(STN_MAINT_SESSION, stn_maint_enter(&w.s, 900, true, w.in.now_ms));
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_MAINT_SESSION, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_FALSE(w.s.maint);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_MAINT_NOT_ON, stn_maint_end(&w.s, &w.in));

    /* A receiver-flash journal open: refused. */
    sim_t f;
    sim_init(&f, true);
    sim_start(&f);
    TEST_ASSERT_EQUAL(STN_MAINT_FLASH, stn_maint_enter(&f.s, 900, false, f.in.now_ms));
    TEST_ASSERT_FALSE(f.s.maint);
}

TEST_CASE("station core: in maintenance nothing is sent until it ends, then it starts as at boot", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_take_notes(&w.s, (stn_note_t[STN_NOTES]){ 0 }, STN_NOTES);
    /* A ladder step queued before is dropped; the user's own request stays. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    int64_t entered = w.in.now_ms;
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_ON, &n));
    TEST_ASSERT_EQUAL_INT32(900, n.a);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.rec_pending);
    TEST_ASSERT_EQUAL_UINT32(900, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);   /* listening only: the health ladder is suspended */
    TEST_ASSERT_FALSE(w.s.paused);

    /* Nothing at all: no ladder step, no query, no apply, not even after
     * the receiver goes silent and restarts. */
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_recovery(&w.s, STN_REC_RESYNC, true));
    stn_request_apply(&w.s);   /* the station's API refuses it; queued here it waits for the end */
    w.rtcm_on = false;
    stn_restart_seen(&w.s);
    uint32_t jobs = w.jobs;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 600).kind);
    TEST_ASSERT_EQUAL_UINT32(jobs, w.jobs);
    TEST_ASSERT_TRUE(w.s.maint);
    TEST_ASSERT_FALSE(w.s.restart_seen);   /* noted, nothing done */

    /* Extended while on: added to the time left, at most 4 h left. */
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_UINT32_WITHIN(2, 900 - 600 + 900, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 4 * 3600, false, w.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(STN_MAINT_MAX_S, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_MAINT_DURATION, stn_maint_enter(&w.s, 5, false, w.in.now_ms));

    /* Time up: the stream-first path, as at boot (listen, identity, check). */
    w.rtcm_on = true;
    stn_job_t j = sim_until_job(&w, STN_MAINT_MAX_S + 10);
    TEST_ASSERT_FALSE(w.s.maint);
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(1, n.b);   /* the time was up */
    TEST_ASSERT_INT32_WITHIN(5, (int32_t)((w.in.now_ms - entered) / 1000), n.a);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_FALSE(j.leased);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    identity_done(&w, true, true);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);   /* the user's request ran once it was over */
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
}

TEST_CASE("station core: maintenance ends at the user's word; a silent receiver is then looked for", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 3600, false, w.in.now_ms));
    w.rtcm_on = false;   /* reflashed over the other port, still silent */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 120).kind);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_end(&w.s, &w.in));
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(0, n.b);
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    TEST_ASSERT_FALSE(w.s.identity_ok);
    /* Nothing valid in 3 s: detection, as at boot. */
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_EQUAL(STN_DETECTING, w.s.state);

    /* Entered from NO_RECEIVER: no detection passes while it is on. */
    fail_pass(&w, false);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 890).kind);
}

TEST_CASE("station core: maintenance records: the RTC record and the stored copy, damaged ones refused", "[station]")
{
    stn_maint_rec_t rec = { .on = true, .stamp = 0xA5u, .left_ms = 600000, .chosen_s = 900,
                            .rtc_us = 0x123456789ull, .end_unix = 1791590400 };
    stn_maint_rtc_t r;
    stn_maint_rec_t got;
    stn_maint_rtc_write(&r, &rec);
    TEST_ASSERT_TRUE(stn_maint_rtc_read(&r, &got));
    TEST_ASSERT_TRUE(got.on);
    TEST_ASSERT_EQUAL_HEX32(0xA5u, got.stamp);
    TEST_ASSERT_EQUAL_UINT32(600000, got.left_ms);
    TEST_ASSERT_EQUAL_UINT32(900, got.chosen_s);
    TEST_ASSERT_TRUE(got.rtc_us == 0x123456789ull);
    TEST_ASSERT_TRUE(got.end_unix == 1791590400);
    stn_maint_rtc_t bad = r;
    bad.left_ms = 600001;   /* damaged */
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&bad, &got));
    bad = r;
    bad.magic ^= 1u;
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&bad, &got));
    memset(&r, 0xFF, sizeof(r));             /* random memory after a power cut */
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&r, &got));
    memset(&r, 0, sizeof(r));
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&r, &got));
    /* More left than chosen, or more than the longest choice: not ours. */
    stn_maint_rec_t odd = rec;
    odd.left_ms = 901000;
    stn_maint_rtc_write(&r, &odd);
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&r, &got));
    odd = rec;
    odd.left_ms = STN_MAINT_MAX_S * 1000u + 1u;
    odd.chosen_s = STN_MAINT_MAX_S * 2u;
    stn_maint_rtc_write(&r, &odd);
    TEST_ASSERT_FALSE(stn_maint_rtc_read(&r, &got));
    /* "Ended" is a record too (a stored copy left behind is then stale). */
    stn_maint_rec_t off = { .on = false, .stamp = 7 };
    stn_maint_rtc_write(&r, &off);
    TEST_ASSERT_TRUE(stn_maint_rtc_read(&r, &got));
    TEST_ASSERT_FALSE(got.on);
    TEST_ASSERT_EQUAL_HEX32(7, got.stamp);

    /* The stored copy: versioned, little-endian, newer writers may append;
     * nothing known: one byte says so. */
    uint8_t buf[STN_MAINT_STORE_MAX + 8];
    size_t len = stn_maint_store_encode(&rec, NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(22, len);
    TEST_ASSERT_EQUAL_UINT8(STN_MAINT_STORE_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[21]);
    stn_maint_known_t known;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &known, buf, len));
    TEST_ASSERT_FALSE(known.valid);
    TEST_ASSERT_TRUE(got.on);
    TEST_ASSERT_EQUAL_HEX32(0xA5u, got.stamp);
    TEST_ASSERT_EQUAL_UINT32(600000, got.left_ms);
    TEST_ASSERT_EQUAL_UINT32(900, got.chosen_s);
    TEST_ASSERT_TRUE(got.end_unix == 1791590400);
    TEST_ASSERT_TRUE(got.rtc_us == 0);   /* RTC record only */
    TEST_ASSERT_EQUAL_size_t(0, stn_maint_store_encode(&rec, NULL, buf, 21));
    TEST_ASSERT_FALSE(stn_maint_store_decode(&got, NULL, buf, 20));   /* the time left cut short */
    TEST_ASSERT_FALSE(stn_maint_store_decode(&got, NULL, buf, 0));
    TEST_ASSERT_FALSE(stn_maint_store_decode(&got, NULL, NULL, len));
    buf[len] = 0x55;   /* a newer writer's field */
    buf[0] = STN_MAINT_STORE_VERSION + 1;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, NULL, buf, len + 1));
    buf[0] = 0;
    TEST_ASSERT_FALSE(stn_maint_store_decode(&got, NULL, buf, len));
    /* A copy of version 1 (the firmware before): the time left, nothing known. */
    buf[0] = 1;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &known, buf, 21));
    TEST_ASSERT_EQUAL_UINT32(600000, got.left_ms);
    TEST_ASSERT_FALSE(known.valid);
    stn_maint_rec_t none = rec;
    none.left_ms = 0;   /* written in its last minute: read, with nothing left */
    len = stn_maint_store_encode(&none, NULL, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, NULL, buf, len));
    TEST_ASSERT_EQUAL_UINT32(0, got.left_ms);
}

/* What a station that identified its receiver, applied the profile and found the fixed position far knew. */
static stn_maint_known_t known_far(void)
{
    stn_maint_known_t k;
    memset(&k, 0, sizeof(k));
    k.valid = true;
    k.profile_hash = 0x1234ABCDu;
    k.receiver.valid = true;
    strcpy(k.receiver.model, "UM982");
    strcpy(k.receiver.firmware, "R4.10Build25164");
    k.receiver.build = 25164;
    strcpy(k.receiver.serial, "SYNTH00000001");
    strcpy(k.receiver.pn, "1234567890123");
    strcpy(k.receiver.efuse_id, "0123456789abcdef");
    strcpy(k.receiver.compile_date, "2025/04/18");
    k.identity_confirmed = true;
    k.sg_valid = true;
    k.sg_master = 3;
    k.sg_slave = 6;
    k.profile_applied = true;
    k.profile_acknowledged = true;
    k.alerts = STN_ALERT_FIXED_FAR | STN_ALERT_SIGNALS_MISMATCH;
    k.fix_state = STN_FIX_FAR;
    k.fix_horizontal_m = 1112.0f;
    k.fix_up_m = -3.0f;
    k.fix_distance_m = 1112.5f;
    k.fix_height_only = false;
    return k;
}

TEST_CASE("station core: the stored copy carries what the station knew of the receiver; a damaged part loses only that",
          "[station]")
{
    const stn_maint_rec_t rec = { .on = true, .stamp = 9, .left_ms = 600000, .chosen_s = 1800, .end_unix = 0 };
    stn_maint_known_t k = known_far();
    k.alerts |= STN_ALERT_NO_RECEIVER;   /* derived: never stored */
    k.survey_active = true;
    k.survey_complete = false;
    k.lock_done = true;
    k.survey_s = 600;
    k.survey_end_rel_ms = -1170000;
    uint8_t buf[STN_MAINT_STORE_MAX];
    size_t len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 22);
    stn_maint_rec_t got;
    stn_maint_known_t g;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_EQUAL_UINT32(600000, got.left_ms);
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_EQUAL_HEX32(0x1234ABCDu, g.profile_hash);
    TEST_ASSERT_TRUE(g.receiver.valid);
    TEST_ASSERT_EQUAL_STRING("UM982", g.receiver.model);
    TEST_ASSERT_EQUAL_STRING("R4.10Build25164", g.receiver.firmware);
    TEST_ASSERT_EQUAL_UINT32(25164, g.receiver.build);
    TEST_ASSERT_EQUAL_STRING("SYNTH00000001", g.receiver.serial);
    TEST_ASSERT_EQUAL_STRING("1234567890123", g.receiver.pn);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", g.receiver.efuse_id);
    TEST_ASSERT_EQUAL_STRING("2025/04/18", g.receiver.compile_date);
    TEST_ASSERT_TRUE(g.identity_confirmed);
    TEST_ASSERT_TRUE(g.sg_valid);
    TEST_ASSERT_EQUAL_UINT8(3, g.sg_master);
    TEST_ASSERT_EQUAL_UINT8(6, g.sg_slave);
    TEST_ASSERT_TRUE(g.profile_applied);
    TEST_ASSERT_TRUE(g.profile_acknowledged);
    TEST_ASSERT_EQUAL_HEX32(STN_ALERT_FIXED_FAR | STN_ALERT_SIGNALS_MISMATCH, g.alerts);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, g.fix_state);
    TEST_ASSERT_EQUAL_FLOAT(1112.0f, g.fix_horizontal_m);
    TEST_ASSERT_EQUAL_FLOAT(-3.0f, g.fix_up_m);
    TEST_ASSERT_EQUAL_FLOAT(1112.5f, g.fix_distance_m);
    TEST_ASSERT_FALSE(g.fix_height_only);
    TEST_ASSERT_TRUE(g.survey_active);
    TEST_ASSERT_FALSE(g.survey_complete);
    TEST_ASSERT_TRUE(g.lock_done);
    TEST_ASSERT_EQUAL_UINT32(600, g.survey_s);
    TEST_ASSERT_EQUAL_INT32(-1170000, g.survey_end_rel_ms);

    /* Nothing identified: no strings; acknowledged but not checked against the stream (no antenna). */
    stn_maint_known_t n = known_far();
    memset(&n.receiver, 0, sizeof(n.receiver));
    n.identity_confirmed = false;
    n.sg_valid = false;
    n.profile_applied = false;
    n.fix_state = STN_FIX_NEAR;
    len = stn_maint_store_encode(&rec, &n, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_FALSE(g.profile_applied);
    TEST_ASSERT_TRUE(g.profile_acknowledged);
    TEST_ASSERT_FALSE(g.fix_sea_level);
    n.profile_acknowledged = false;
    n.fix_state = STN_FIX_FAR;
    n.fix_sea_level = true;
    len = stn_maint_store_encode(&rec, &n, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_FALSE(g.profile_acknowledged);
    TEST_ASSERT_TRUE(g.fix_sea_level);   /* the other bit of its byte */
    n.profile_acknowledged = true;
    n.fix_sea_level = false;
    len = stn_maint_store_encode(&rec, &n, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_FALSE(g.receiver.valid);
    TEST_ASSERT_EQUAL_STRING("", g.receiver.model);
    TEST_ASSERT_FALSE(g.sg_valid);

    /* Every string at its longest still fits the buffer. */
    stn_maint_known_t big = known_far();
    memset(big.receiver.model, 'M', sizeof(big.receiver.model) - 1);
    memset(big.receiver.firmware, 'F', sizeof(big.receiver.firmware) - 1);
    memset(big.receiver.serial, 'S', sizeof(big.receiver.serial) - 1);
    memset(big.receiver.pn, 'P', sizeof(big.receiver.pn) - 1);
    memset(big.receiver.efuse_id, 'E', sizeof(big.receiver.efuse_id) - 1);
    memset(big.receiver.compile_date, 'D', sizeof(big.receiver.compile_date) - 1);
    len = stn_maint_store_encode(&rec, &big, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0 && len <= STN_MAINT_STORE_MAX);
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_EQUAL_size_t(sizeof(big.receiver.firmware) - 1, strlen(g.receiver.firmware));

    /* A damaged part, or one cut short: the time left still counts, nothing known. */
    len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len - 1));
    TEST_ASSERT_TRUE(got.on);
    TEST_ASSERT_EQUAL_UINT32(600000, got.left_ms);
    TEST_ASSERT_FALSE(g.valid);
    buf[22 + 4 + 1] = 200;   /* the model's length: longer than the copy */
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_FALSE(g.valid);
    /* "Identified" without a model, or a fixed-check state no writer has. */
    uint8_t nomodel[STN_MAINT_STORE_MAX];
    stn_maint_known_t anon = k;
    memset(&anon.receiver, 0, sizeof(anon.receiver));
    size_t alen = stn_maint_store_encode(&rec, &anon, nomodel, sizeof(nomodel));
    nomodel[22 + 4] |= 1u;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, nomodel, alen));
    TEST_ASSERT_FALSE(g.valid);
    len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    buf[len - 1 - 2 - 4 - 4 - 4 * 3 - 1] = 9;   /* the fixed-check state */
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_FALSE(g.valid);
}

TEST_CASE("station core: the stored copy keeps a fixed-position warning's sea-level reason, only with a far verdict",
          "[station]")
{
    const stn_maint_rec_t rec = { .on = true, .stamp = 9, .left_ms = 600000, .chosen_s = 1800, .end_unix = 0 };
    stn_maint_known_t k = known_far();
    k.fix_horizontal_m = 0.4f;
    k.fix_up_m = 34.0f;
    k.fix_distance_m = 34.0f;
    k.fix_height_only = true;
    k.fix_sea_level = true;
    uint8_t buf[STN_MAINT_STORE_MAX];
    size_t len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    stn_maint_rec_t got;
    stn_maint_known_t g;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, g.fix_state);
    TEST_ASSERT_TRUE(g.fix_height_only);
    TEST_ASSERT_TRUE(g.fix_sea_level);
    TEST_ASSERT_EQUAL_FLOAT(34.0f, g.fix_up_m);

    /* A far verdict for the distance: no sea-level reason. */
    k.fix_sea_level = false;
    len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_FALSE(g.fix_sea_level);

    /* The reason without a far verdict means nothing: dropped. */
    k.fix_sea_level = true;
    k.fix_state = STN_FIX_NEAR;
    len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NEAR, g.fix_state);
    TEST_ASSERT_FALSE(g.fix_sea_level);
}

TEST_CASE("station core: a resumed maintenance shows what the station knew only from a stored copy of its own",
          "[station]")
{
    const stn_maint_rec_t rtc = { .on = true, .stamp = 3, .left_ms = 600000, .chosen_s = 900, .rtc_us = 1000000000ull };
    const stn_maint_rec_t store = { .on = true, .stamp = 3, .left_ms = 535000, .chosen_s = 900 };
    stn_maint_boot_t b;
    stn_maint_boot_in_t in;
    memset(&in, 0, sizeof(in));
    in.uptime_ms = 1500;

    /* The RTC record gives the time, the stored copy what the station knew. */
    in.rtc_ok = true;
    in.rtc = rtc;
    in.rtc_timer_kept = true;
    in.rtc_now_us = rtc.rtc_us + 4200000ull;
    in.store_ok = true;
    in.store = store;
    in.store_known = true;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_RTC, b.from);
    TEST_ASSERT_TRUE(b.known);
    /* A copy without it (version 1, or an unreadable part). */
    in.store_known = false;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_RTC, b.from);
    TEST_ASSERT_FALSE(b.known);
    /* No stored copy of the run right before. */
    in.store_known = true;
    in.store_ok = false;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_RTC, b.from);
    TEST_ASSERT_FALSE(b.known);
    /* The EN pin: the stored copy gives both. */
    memset(&in, 0, sizeof(in));
    in.uptime_ms = 1500;
    in.store_ok = true;
    in.store = store;
    in.store_known = true;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_STORE, b.from);
    TEST_ASSERT_TRUE(b.known);
    /* Nothing resumes: nothing to show. */
    in.blocked = true;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_FALSE(b.known);
    in.blocked = false;
    in.store.left_ms = 1000;   /* expired across the reset */
    stn_maint_boot(&in, &b);
    TEST_ASSERT_TRUE(b.expired);
    TEST_ASSERT_FALSE(b.known);
}

TEST_CASE("station core: the stored copy is due at entry, at an extension and every minute, and holds less than is left",
          "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_maint_rec_t rec;
    TEST_ASSERT_FALSE(stn_maint_store_due(&w.s, 0, STN_NEVER, w.in.now_ms));   /* off: nothing to store */
    stn_maint_record(&w.s, w.in.now_ms, 0, false, &rec);
    TEST_ASSERT_FALSE(rec.on);

    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_TRUE(stn_maint_store_due(&w.s, 0, STN_NEVER, w.in.now_ms));
    const int64_t utc = 1791590400;
    stn_maint_record(&w.s, w.in.now_ms, utc, false, &rec);   /* the RTC record: exact */
    TEST_ASSERT_TRUE(rec.on);
    TEST_ASSERT_EQUAL_UINT32(900000, rec.left_ms);
    TEST_ASSERT_EQUAL_UINT32(900, rec.chosen_s);
    TEST_ASSERT_TRUE(rec.end_unix == utc + 900);
    stn_maint_record(&w.s, w.in.now_ms, 0, true, &rec);      /* the stored copy, no clock */
    TEST_ASSERT_EQUAL_UINT32(900000 - STN_MAINT_STORE_SHORT_S * 1000, rec.left_ms);
    TEST_ASSERT_TRUE(rec.end_unix == 0);

    /* Saved now: due again after STN_MAINT_SAVE_S, and a reset at any moment
     * until a save that comes a few seconds late never finds more than is
     * left. */
    int64_t saved = w.in.now_ms;
    stn_maint_rec_t stored;
    stn_maint_record(&w.s, w.in.now_ms, 0, true, &stored);
    for (int i = 0; i < (int)STN_MAINT_STORE_SHORT_S; i++) {
        TEST_ASSERT_EQUAL(i >= (int)STN_MAINT_SAVE_S, stn_maint_store_due(&w.s, 900, saved, w.in.now_ms));
        TEST_ASSERT_TRUE((int64_t)stored.left_ms <= w.s.maint_until_ms - w.in.now_ms);
        sim_tick(&w);
    }
    /* An extension is stored at once. */
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_TRUE(stn_maint_store_due(&w.s, 900, w.in.now_ms, w.in.now_ms));
    /* Near its end the copy holds nothing: a reset then ends it. */
    w.s.maint_until_ms = w.in.now_ms + (int64_t)STN_MAINT_STORE_SHORT_S * 1000 - 1000;
    stn_maint_record(&w.s, w.in.now_ms, 0, true, &rec);
    TEST_ASSERT_TRUE(rec.on);
    TEST_ASSERT_EQUAL_UINT32(0, rec.left_ms);
}

static stn_maint_boot_in_t boot_in(void)
{
    stn_maint_boot_in_t in;
    memset(&in, 0, sizeof(in));
    in.uptime_ms = 1500;
    return in;
}

TEST_CASE("station core: a boot resumes maintenance from the RTC record, else from the stored copy, less the reset's "
          "time",
          "[station]")
{
    const stn_maint_rec_t rtc = { .on = true, .stamp = 3, .left_ms = 600000, .chosen_s = 900,
                                  .rtc_us = 1000000000ull, .end_unix = 1791590400 };
    const stn_maint_rec_t store = { .on = true, .stamp = 3, .left_ms = 535000, .chosen_s = 900,
                                    .end_unix = 1791590400 };
    stn_maint_boot_t b;

    /* A software reset, a crash, a watchdog or a brownout (its interrupt
     * ends in a software system reset): the RTC timer tells the time across
     * it exactly; the stored copy is not needed. */
    stn_maint_boot_in_t in = boot_in();
    in.rtc_ok = true;
    in.rtc = rtc;
    in.rtc_timer_kept = lifecycle_reset_keeps_rtc_timer(ESP_RST_BROWNOUT);
    in.rtc_now_us = rtc.rtc_us + 4200000ull;
    in.store_ok = true;
    in.store = store;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_RTC, b.from);
    TEST_ASSERT_TRUE(b.elapsed_known);
    TEST_ASSERT_EQUAL_UINT32(600000 - 4200, b.left_ms);
    TEST_ASSERT_EQUAL_UINT32(900, b.chosen_s);
    TEST_ASSERT_TRUE(b.end_unix == 0);   /* no clock check needed */
    TEST_ASSERT_FALSE(b.expired);

    /* The RTC memory kept, the timer not (power-on class), or a timer that
     * went back: at least this boot's run time comes off, and the clock
     * checks the rest. */
    in.rtc_timer_kept = false;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_RTC, b.from);
    TEST_ASSERT_FALSE(b.elapsed_known);
    TEST_ASSERT_EQUAL_UINT32(600000 - 1500, b.left_ms);
    TEST_ASSERT_TRUE(b.end_unix == 1791590400);
    in.rtc_timer_kept = true;
    in.rtc_now_us = 2000000ull;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_FALSE(b.elapsed_known);
    TEST_ASSERT_EQUAL_UINT32(600000 - 1500, b.left_ms);

    /* Its time ran out across the reset. */
    in.rtc_now_us = rtc.rtc_us + 600000000ull;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_TRUE(b.expired);
    TEST_ASSERT_EQUAL_UINT32(900, b.chosen_s);

    /* The RTC record says it had ended: a stored copy left behind is stale. */
    in = boot_in();
    in.rtc_ok = true;
    in.rtc.on = false;
    in.store_ok = true;
    in.store = store;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_FALSE(b.expired);

    /* The EN pin (or a power cut) lost the RTC memory: the stored copy, less
     * this boot's run time; the clock checks the rest. */
    in = boot_in();
    in.store_ok = true;
    in.store = store;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_STORE, b.from);
    TEST_ASSERT_FALSE(b.elapsed_known);
    TEST_ASSERT_EQUAL_UINT32(535000 - 1500, b.left_ms);
    TEST_ASSERT_EQUAL_UINT32(900, b.chosen_s);
    TEST_ASSERT_TRUE(b.end_unix == 1791590400);
    in.store.left_ms = 1000;   /* stored in its last minute */
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_TRUE(b.expired);

    /* An open receiver flash journal, or the factory reset before this boot:
     * nothing resumes. */
    in = boot_in();
    in.rtc_ok = true;
    in.rtc = rtc;
    in.store_ok = true;
    in.store = store;
    in.blocked = true;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_FALSE(b.expired);

    /* Nothing at all. */
    in = boot_in();
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_NONE, b.from);
    TEST_ASSERT_FALSE(b.expired);
}

/* Resume as station_init does: from a decision with this much left. */
static void resume(sim_t *w, uint8_t from, uint32_t left_ms, uint32_t chosen_s, int64_t end_unix)
{
    stn_maint_boot_t b;
    memset(&b, 0, sizeof(b));
    b.from = from;
    b.left_ms = left_ms;
    b.chosen_s = chosen_s;
    b.end_unix = end_unix;
    stn_maint_restore(&w->s, &b, w->in.now_ms);
}

TEST_CASE("station core: resumed maintenance only listens until its time left is up", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    resume(&w, STN_MAINT_FROM_RTC, 600000, 900, 0);
    TEST_ASSERT_TRUE(w.s.maint);
    TEST_ASSERT_TRUE(w.s.maint_resumed);
    TEST_ASSERT_EQUAL_UINT32(900, w.s.maint_chosen_s);
    TEST_ASSERT_EQUAL_UINT32(600, stn_maint_left_s(&w.s, w.in.now_ms));
    w.rtcm_on = true;
    sim_start(&w);
    stn_restart_seen(&w.s);   /* the user's work restarted the receiver: noted, nothing done */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 590).kind);
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    stn_job_t j = sim_until_job(&w, 20);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(STN_MAINT_END_TIME, n.b);
    TEST_ASSERT_INT32_WITHIN(3, 900, n.a);   /* the time before the reset counts */
    TEST_ASSERT_FALSE(w.s.maint_resumed);

    /* Nothing left: nothing resumes. */
    sim_t z;
    sim_init(&z, false);
    resume(&z, STN_MAINT_FROM_STORE, 0, 900, 0);
    TEST_ASSERT_FALSE(z.s.maint);
    resume(&z, STN_MAINT_FROM_NONE, 60000, 900, 0);
    TEST_ASSERT_FALSE(z.s.maint);
}

TEST_CASE("station core: by the clock, resumed maintenance never ends later than chosen", "[station]")
{
    const int64_t end = 1791590400;
    sim_t w;
    sim_init(&w, false);
    w.rtcm_on = true;
    resume(&w, STN_MAINT_FROM_STORE, 300000, 900, end);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 10).kind);
    /* No clock yet: nothing changes. */
    stn_maint_clock(&w.s, 0, w.in.now_ms);
    TEST_ASSERT_UINT32_WITHIN(1, 290, stn_maint_left_s(&w.s, w.in.now_ms));
    /* The clock says 100 s are left (the ESP32 was held in reset): never
     * later than that (a second less: the clock counts whole seconds), and
     * the time spent grows by as much. */
    stn_maint_clock(&w.s, end - 100, w.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(99, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(900, w.s.maint_chosen_s);
    TEST_ASSERT_TRUE(w.s.maint_end_unix == 0);   /* once */
    stn_maint_clock(&w.s, end - 50, w.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(99, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 94).kind);
    stn_job_t j = sim_until_job(&w, 10);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(STN_MAINT_END_TIME, n.b);
    TEST_ASSERT_INT32_WITHIN(2, 900, n.a);

    /* The clock never adds time. */
    sim_t m;
    sim_init(&m, false);
    resume(&m, STN_MAINT_FROM_STORE, 300000, 900, end);
    stn_maint_clock(&m.s, end - 1000, m.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(300, stn_maint_left_s(&m.s, m.in.now_ms));

    /* Its end passed while the ESP32 was off: it ends at the next step. */
    sim_t x;
    sim_init(&x, false);
    x.rtcm_on = true;
    resume(&x, STN_MAINT_FROM_STORE, 300000, 900, end);
    sim_start(&x);
    stn_maint_clock(&x.s, end + 5, x.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(0, stn_maint_left_s(&x.s, x.in.now_ms));
    (void)sim_next(&x);
    TEST_ASSERT_FALSE(x.s.maint);
    TEST_ASSERT_TRUE(has_note(&x, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(STN_MAINT_END_TIME, n.b);

    /* An extension moves the end by the clock as much as the time left. */
    sim_t e;
    sim_init(&e, false);
    resume(&e, STN_MAINT_FROM_STORE, 300000, 900, end);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&e.s, 900, false, e.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(1200, stn_maint_left_s(&e.s, e.in.now_ms));
    TEST_ASSERT_TRUE(e.s.maint_end_unix == end + 900);
    stn_maint_clock(&e.s, end - 100, e.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(999, stn_maint_left_s(&e.s, e.in.now_ms));

    /* A record written before the clock checked the resumed end carries it on. */
    sim_t c;
    sim_init(&c, false);
    resume(&c, STN_MAINT_FROM_STORE, 300000, 900, end);
    stn_maint_rec_t rec;
    stn_maint_record(&c.s, c.in.now_ms, 0, true, &rec);
    TEST_ASSERT_TRUE(rec.end_unix == end);
    stn_maint_record(&c.s, c.in.now_ms, end - 100, true, &rec);   /* a clock, not checked yet: never later */
    TEST_ASSERT_TRUE(rec.end_unix == end);
    stn_maint_record(&c.s, c.in.now_ms, end - 1000, true, &rec);  /* the counted time ends first */
    TEST_ASSERT_TRUE(rec.end_unix == end - 700);
}

TEST_CASE("station core: a power cut ends resumed maintenance, not one the user started or extended", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.rtcm_on = true;
    resume(&w, STN_MAINT_FROM_STORE, 1200000, 1800, 0);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 20).kind);
    TEST_ASSERT_TRUE(stn_maint_power_cut(&w.s, &w.in));
    TEST_ASSERT_FALSE(w.s.maint);
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(STN_MAINT_END_POWER, n.b);
    TEST_ASSERT_INT32_WITHIN(2, 600 + 20, n.a);
    /* The stream-first path, as at boot. */
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    TEST_ASSERT_FALSE(stn_maint_power_cut(&w.s, &w.in));   /* nothing on */

    /* Started by the user this boot: a power-on verdict says nothing about it. */
    sim_t u;
    sim_init(&u, false);
    boot_applied(&u);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&u.s, 900, false, u.in.now_ms));
    TEST_ASSERT_FALSE(stn_maint_power_cut(&u.s, &u.in));
    TEST_ASSERT_TRUE(u.s.maint);

    /* Resumed, then extended by the user: theirs now. */
    sim_t x;
    sim_init(&x, false);
    resume(&x, STN_MAINT_FROM_RTC, 600000, 900, 0);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&x.s, 900, false, x.in.now_ms));
    TEST_ASSERT_FALSE(stn_maint_power_cut(&x.s, &x.in));
    TEST_ASSERT_TRUE(x.s.maint);
}

/* A station surveying (its boot read-back found a survey), 30 s after that read-back. */
static void boot_surveying(sim_t *w)
{
    w->in.profile_survey = true;
    w->skip_station = true;   /* 1005/1033 held back while surveying */
    boot_streaming(w, true);
    stn_job_t j = sim_next(w);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    stn_result_t r = res(STN_OK);
    r.mode_survey = true;
    sim_done(w, &r);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(w, 30).kind);
    TEST_ASSERT_TRUE(stn_surveying(&w->s, w->in.now_ms));
}

TEST_CASE("station core: a survey window goes across a reset in maintenance and runs on after it", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_surveying(&w);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 100).kind);
    uint32_t survey_left = stn_survey_left_s(&w.s, w.in.now_ms);
    TEST_ASSERT_UINT32_WITHIN(2, 720 - 130, survey_left);

    /* The stored copy: the time left 70 s short, the window's end relative to maintenance's own. */
    stn_maint_rec_t rec;
    stn_maint_record(&w.s, w.in.now_ms, 0, true, &rec);
    stn_maint_known_t k = known_far();
    stn_maint_known_survey(&w.s, &k);
    TEST_ASSERT_TRUE(k.survey_active);
    TEST_ASSERT_FALSE(k.survey_complete);
    TEST_ASSERT_EQUAL_UINT32(600, k.survey_s);
    TEST_ASSERT_INT32_WITHIN(1000, ((int32_t)survey_left - 1700) * 1000, k.survey_end_rel_ms);
    uint8_t buf[STN_MAINT_STORE_MAX];
    size_t len = stn_maint_store_encode(&rec, &k, buf, sizeof(buf));
    stn_maint_rec_t got;
    stn_maint_known_t g;
    TEST_ASSERT_TRUE(stn_maint_store_decode(&got, &g, buf, len));

    /* The EN pin: the next boot resumes from the copy, 5 s into its run. */
    stn_maint_boot_in_t in;
    memset(&in, 0, sizeof(in));
    in.uptime_ms = 5000;
    in.store_ok = true;
    in.store = got;
    in.store_known = g.valid;
    stn_maint_boot_t b;
    stn_maint_boot(&in, &b);
    TEST_ASSERT_EQUAL(STN_MAINT_FROM_STORE, b.from);
    TEST_ASSERT_TRUE(b.known);
    sim_t z;
    sim_init(&z, false);
    z.in.profile_survey = true;
    z.skip_station = true;
    z.rtcm_on = true;
    stn_maint_restore(&z.s, &b, z.in.now_ms);
    stn_maint_restore_survey(&z.s, &g);
    /* Surveying, with as much less left as the maintenance: never more than is left. */
    TEST_ASSERT_TRUE(stn_surveying(&z.s, z.in.now_ms));
    TEST_ASSERT_UINT32_WITHIN(1, survey_left - STN_MAINT_STORE_SHORT_S - 5, stn_survey_left_s(&z.s, z.in.now_ms));
    TEST_ASSERT_TRUE(z.s.lock_done == g.lock_done);
    sim_start(&z);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&z, 60).kind);   /* hands off */
    uint32_t left = stn_survey_left_s(&z.s, z.in.now_ms);

    /* The user ends it: identity, then the boot read-back finds the survey the
     * station already tracks: no new window, it runs on. */
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_end(&z.s, &z.in));
    stn_job_t j = sim_until_job(&z, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    identity_done(&z, true, true);
    j = sim_next(&z);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_BOOT, j.mode_why);
    stn_result_t r = res(STN_OK);
    r.mode_survey = true;
    sim_done(&z, &r);
    TEST_ASSERT_TRUE(stn_surveying(&z.s, z.in.now_ms));
    TEST_ASSERT_UINT32_WITHIN(6, left, stn_survey_left_s(&z.s, z.in.now_ms));
    /* ... and its read-back every minute goes on. */
    j = sim_next(&z);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    TEST_ASSERT_EQUAL(STN_MODE_POLL, j.mode_why);

    /* Nothing known, or no survey then: no window. */
    sim_t n;
    sim_init(&n, false);
    stn_maint_restore(&n.s, &b, n.in.now_ms);
    stn_maint_known_t none = g;
    none.survey_active = false;
    stn_maint_restore_survey(&n.s, &none);
    TEST_ASSERT_FALSE(n.s.survey.active);
    none = g;
    none.valid = false;
    stn_maint_restore_survey(&n.s, &none);
    TEST_ASSERT_FALSE(n.s.survey.active);
    stn_maint_restore_survey(&n.s, NULL);
    TEST_ASSERT_FALSE(n.s.survey.active);
    /* No maintenance resumed: nothing. */
    sim_t o;
    sim_init(&o, false);
    stn_maint_restore_survey(&o.s, &g);
    TEST_ASSERT_FALSE(o.s.survey.active);

    /* A survey that was complete but not locked yet: locked once maintenance ends. */
    sim_t c;
    sim_init(&c, false);
    c.in.profile_survey = true;
    c.rtcm_on = true;
    stn_maint_restore(&c.s, &b, c.in.now_ms);
    stn_maint_known_t done = g;
    done.survey_complete = true;
    done.lock_done = false;
    stn_maint_restore_survey(&c.s, &done);
    sim_start(&c);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_end(&c.s, &c.in));
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&c, 5).kind);
    identity_done(&c, true, true);
    j = sim_next(&c);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
}

TEST_CASE("station core: by the clock, a survey window restored with maintenance moves as much as it", "[station]")
{
    const int64_t end = 1791590400;
    sim_t w;
    sim_init(&w, false);
    stn_maint_boot_t b;
    memset(&b, 0, sizeof(b));
    b.from = STN_MAINT_FROM_STORE;
    b.left_ms = 1200000;
    b.chosen_s = 1800;
    b.end_unix = end;
    b.known = true;
    stn_maint_restore(&w.s, &b, w.in.now_ms);
    stn_maint_known_t k = known_far();
    k.survey_active = true;
    k.survey_s = 600;
    k.survey_end_rel_ms = -900000;   /* the window ends 300 s from now */
    stn_maint_restore_survey(&w.s, &k);
    TEST_ASSERT_EQUAL_UINT32(300, stn_survey_left_s(&w.s, w.in.now_ms));
    /* The ESP32 was held in reset 200 s longer than counted. */
    stn_maint_clock(&w.s, end - 1000, w.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(999, stn_maint_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(99, stn_survey_left_s(&w.s, w.in.now_ms));
    /* Once only. */
    stn_maint_clock(&w.s, end - 1500, w.in.now_ms);
    TEST_ASSERT_EQUAL_UINT32(99, stn_survey_left_s(&w.s, w.in.now_ms));
}

TEST_CASE("station core: a survey without data outlasts its window and is locked once it finishes", "[station]")
{
    /* The antenna goes right after the survey started: the receiver keeps
     * its survey-in, which makes no progress without satellites. */
    sim_t w;
    sim_init(&w, false);
    boot_surveying(&w);
    TEST_ASSERT_TRUE(stn_survey_unfinished(&w.s, w.in.now_ms));
    w.rtcm_on = false;
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 15 * 60, NULL));
    TEST_ASSERT_FALSE(stn_surveying(&w.s, w.in.now_ms));   /* the station's window ended */
    TEST_ASSERT_EQUAL_UINT32(0, stn_survey_left_s(&w.s, w.in.now_ms));
    TEST_ASSERT_TRUE(stn_survey_unfinished(&w.s, w.in.now_ms));   /* the survey did not */
    TEST_ASSERT_EQUAL_UINT(0, noant_run(&w, 60 * 60, NULL));
    TEST_ASSERT_TRUE(stn_survey_unfinished(&w.s, w.in.now_ms));

    /* Data again, with a 1005 that stays put: finished (its time is up and the position steady), then locked. */
    w.rtcm_on = true;
    w.skip_station = false;
    const int64_t arp[3] = { 9181291234LL, -43460712345LL, 45619773456LL };
    stn_job_t j;
    memset(&j, 0, sizeof(j));
    unsigned frames = 0;
    while (j.kind == STN_JOB_NONE && frames < UM98X_SURVEY_STABLE_FRAMES + 5) {
        TEST_ASSERT_TRUE(stn_survey_unfinished(&w.s, w.in.now_ms));
        stn_on_arp(&w.s, arp);
        frames++;
        j = sim_next(&w);
        if (j.kind == STN_JOB_NONE) {
            sim_tick(&w);
        }
    }
    TEST_ASSERT_EQUAL_UINT(UM98X_SURVEY_STABLE_FRAMES, frames);
    TEST_ASSERT_FALSE(stn_survey_unfinished(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_LOCK, j.why);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
}

/* Run up to max_s seconds until an apply is handed out, answering every other job (an identity: the stored profile is
 * not the acknowledged one); STN_JOB_NONE when none comes. */
static stn_job_t until_apply_answered(sim_t *w, int max_s)
{
    for (int i = 0; i <= max_s; i++) {
        stn_job_t j = sim_next(w);
        if (j.kind == STN_JOB_APPLY) {
            return j;
        }
        if (j.kind == STN_JOB_IDENTIFY) {
            w->in.text_lines++;
            identity_done(w, true, false);
        } else if (j.kind != STN_JOB_NONE) {
            w->in.text_lines++;
            sim_done(w, &(stn_result_t){ .err = STN_OK });
        }
        sim_tick(w);
    }
    stn_job_t none;
    memset(&none, 0, sizeof(none));
    return none;
}

TEST_CASE("station core: a profile the station stores itself is not acknowledged until an apply of it works", "[station]")
{
    /* The lock stores the fixed profile before its session takes the lease
     * (station.c then calls stn_profile_changed). What the receiver
     * acknowledged is still the survey profile, whatever the apply does. */
    sim_t w;
    sim_init(&w, false);
    boot_surveying(&w);
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));
    stn_request_lock(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
    stn_profile_changed(&w.s);   /* the job stored the locked profile */
    w.in.profile_survey = false;
    w.skip_station = false;
    stn_result_t r = res(STN_E_STATE);
    r.nothing_sent = true;   /* no lease: nothing reached the receiver */
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
    TEST_ASSERT_FALSE(w.s.profile_applied);

    /* The retry's session runs and is rolled back (the receiver restarted): still not acknowledged. */
    j = until_apply_answered(&w, 11 * 60);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_RETRY, j.why);
    sim_done(&w, &(stn_result_t){ .err = STN_E_FAIL, .restarted = true, .answered = true });
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));

    /* The next retry works: acknowledged. */
    j = until_apply_answered(&w, 31 * 60);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_FALSE(stn_profile_acknowledged(&w.s));
    sim_done(&w, &(stn_result_t){ .err = STN_OK, .answered = true });
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_TRUE(stn_profile_acknowledged(&w.s));
}

/* ---- live view ---------------------------------------------------------------- */

#define LIVE_MS (10 * MIN_MS)

static stn_live_status_t live_status(sim_t *w)
{
    stn_live_status_t st;
    stn_live_status(&w->s, &w->in, &st);
    return st;
}

static void sky_done(sim_t *w, stn_err_t err)
{
    stn_result_t r = res(err);
    r.lease_ms = 250;
    sim_done(w, &r);
}

/* Open a window on a station that streams and turn the output on. */
static void live_on(sim_t *w)
{
    uint8_t why = 99;
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w->s, LIVE_MS, w->in.now_ms, &why));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_NONE, why);
    stn_job_t j = sim_next(w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_TRUE(j.sky_on);
    TEST_ASSERT_TRUE(w->s.paused);   /* the lease holds RTCM back: planned for the casters */
    TEST_ASSERT_EQUAL(STN_STREAMING, w->s.state);
    sky_done(w, STN_OK);
    TEST_ASSERT_FALSE(w->s.paused);
    TEST_ASSERT_TRUE(w->s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_ON, live_status(w).sky);
}

TEST_CASE("station core: the live view turns the output on and off again by itself", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_take_notes(&w.s, (stn_note_t[STN_NOTES]){ 0 }, STN_NOTES);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_OFF, live_status(&w).sky);
    live_on(&w);
    stn_note_t n;
    stn_note_t notes[STN_NOTES];
    unsigned c = stn_take_notes(&w.s, notes, STN_NOTES);
    TEST_ASSERT_EQUAL_UINT(2, c);
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_LIVE_OPENED, notes[0].kind);
    TEST_ASSERT_EQUAL_INT32(600, notes[0].a);
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_SKY_ON, notes[1].kind);
    TEST_ASSERT_EQUAL_INT32(250, notes[1].a);   /* how long RTCM was held */

    /* An admin client keeps it open; it closes when its time is up. */
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 60).kind);
        stn_live_touch(&w.s, w.in.now_ms);
    }
    TEST_ASSERT_TRUE(live_status(&w).open);
    stn_job_t j = sim_until_job(&w, 61);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    TEST_ASSERT_FALSE(live_status(&w).open);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_STOPPING, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_EXPIRED, live_status(&w).last_end);
    sky_done(&w, STN_OK);
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_OFF, live_status(&w).sky);
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_LIVE_CLOSED, &n));
    TEST_ASSERT_EQUAL_INT32(STN_LIVE_END_EXPIRED, n.b);

    /* Nobody asks for it: closed after the idle time. */
    live_on(&w);
    j = sim_until_job(&w, STN_LIVE_IDLE_MS / 1000 + 1);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_IDLE, live_status(&w).last_end);
    sky_done(&w, STN_OK);

    /* The user's stop. */
    live_on(&w);
    stn_live_stop(&w.s, w.in.now_ms);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_USER, live_status(&w).last_end);
    sky_done(&w, STN_OK);

    /* Extended: from the current end, at most the longest window from now. */
    live_on(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_UINT32_WITHIN(1, 2 * LIVE_MS / 1000, live_status(&w).left_s);
    for (int i = 0; i < 10; i++) {
        stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(STN_LIVE_MAX_MS / 1000, live_status(&w).left_s);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);   /* already on: nothing sent */
}

TEST_CASE("station core: the live view never configures behind the user's back", "[station]")
{
    /* gnss/autocfg = 0: the user configures the receiver. */
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    w.in.autocfg = false;
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 30).kind);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_BLOCKED, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_AUTOCFG_OFF, live_status(&w).why);

    /* The profile sends it permanently: never toggled. */
    sim_init(&w, false);
    boot_applied(&w);
    w.in.profile_sky = true;
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 30).kind);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_ON_PROFILE, live_status(&w).sky);
    stn_live_stop(&w.s, w.in.now_ms);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 30).kind);

    /* Not a model we configure. */
    sim_init(&w, false);
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);
    identity_done(&w, false, false);
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 30).kind);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_UNSUPPORTED, live_status(&w).why);

    /* A receiver-flash journal: no live view at all. */
    sim_init(&w, true);
    sim_start(&w);
    uint8_t why = 0;
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, &why));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_FLASH, why);
    TEST_ASSERT_FALSE(w.s.live_open);
}

TEST_CASE("station core: after a reconfigure or restart the live view's output is simply off", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);

    /* An apply quiets the port and saves without it. */
    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_BLOCKED, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_RECONFIGURED, live_status(&w).why);
    TEST_ASSERT_TRUE(live_status(&w).open);   /* the window stays, without the output */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 40).kind);
    /* Starting the view again turns it on again. */
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_TRUE(j.sky_on);
    sky_done(&w, STN_OK);

    /* A receiver restart ends it too; nothing is sent to turn it off. */
    stn_restart_seen(&w.s);
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_RECONFIGURED, live_status(&w).why);
    stn_live_stop(&w.s, w.in.now_ms);
    j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);   /* the restart's own handling, no output toggle */
    TEST_ASSERT_TRUE(j.after_restart);

    /* An apply that sent nothing leaves the output alone. */
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    stn_result_t r = res(STN_E_UNSUPPORTED);
    r.nothing_sent = true;
    sim_done(&w, &r);
    TEST_ASSERT_TRUE(w.s.sky_on);

    /* The ladder's receiver reset. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RECEIVER_RESET, true));
    TEST_ASSERT_EQUAL(STN_JOB_RECEIVER_RESET, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.sky_on);
}

TEST_CASE("station core: the live view waits for the receiver to be free", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);

    /* Wanted while an apply runs: turned on after it. */
    stn_request_apply(&w.s);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_PENDING, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_BUSY, live_status(&w).why);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_TRUE(j.sky_on);
    sky_done(&w, STN_OK);

    /* Maintenance waits until the output is off (nothing would turn it off
     * in maintenance); then nothing is toggled while it is on. */
    TEST_ASSERT_EQUAL(STN_MAINT_SKY_OUTPUT, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    sky_done(&w, STN_OK);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    stn_live_stop(&w.s, w.in.now_ms);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 880).kind);
    j = sim_until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    identity_done(&w, true, true);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);   /* off already */

    /* Wanted during maintenance: pending, turned on after it. */
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, 60 * MIN_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_PENDING, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_MAINTENANCE, live_status(&w).why);
    for (int i = 0; i < 15; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 59).kind);
        stn_live_touch(&w.s, w.in.now_ms);
    }
    j = sim_until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    identity_done(&w, true, true);
    stn_live_touch(&w.s, w.in.now_ms);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_TRUE(j.sky_on);
}

TEST_CASE("station core: a refused or unanswered output command", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_SKY, sim_next(&w).kind);
    sky_done(&w, STN_E_FAIL);
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_BLOCKED, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_REJECTED, live_status(&w).why);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 60).kind);   /* not retried on its own */

    /* No answer: it may have taken, so it is turned off when the window
     * closes, not at once (that would hold RTCM back a second time). */
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_SKY, sim_next(&w).kind);
    sky_done(&w, STN_E_TIMEOUT);
    TEST_ASSERT_TRUE(w.s.sky_on);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_NO_ANSWER, live_status(&w).why);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_BLOCKED, live_status(&w).sky);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 59).kind);
        stn_live_touch(&w.s, w.in.now_ms);
    }
    TEST_ASSERT_TRUE(live_status(&w).open);
    /* Extending sends it again; still no answer, still left for the close. */
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_PENDING, live_status(&w).sky);
    stn_job_t on = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, on.kind);
    TEST_ASSERT_TRUE(on.sky_on);
    sky_done(&w, STN_E_TIMEOUT);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 30).kind);
    stn_live_stop(&w.s, w.in.now_ms);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_STOPPING, live_status(&w).sky);

    /* Turning it off fails: again after 60 s, three tries, then left alone. */
    for (int i = 0; i < STN_SKY_OFF_TRIES; i++) {
        stn_job_t j = sim_until_job(&w, 61);
        TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
        TEST_ASSERT_FALSE(j.sky_on);
        sky_done(&w, STN_E_TIMEOUT);
    }
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_SKY_LEFT_ON, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 300).kind);
}

TEST_CASE("station core: maintenance is refused while the live view's output may be on", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);
    TEST_ASSERT_EQUAL(STN_MAINT_SKY_OUTPUT, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_FALSE(w.s.maint);
    /* The window stays open without the output; it goes off first. */
    TEST_ASSERT_TRUE(live_status(&w).open);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_STOPPING, live_status(&w).sky);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    TEST_ASSERT_EQUAL(STN_MAINT_SESSION, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));   /* the off runs */
    sky_done(&w, STN_OK);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_BLOCKED, live_status(&w).sky);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_MAINTENANCE, live_status(&w).why);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_TRUE(w.s.maint);
    /* The window may close meanwhile: nothing is left on. */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, STN_LIVE_IDLE_MS / 1000 + 5).kind);
    TEST_ASSERT_FALSE(live_status(&w).open);
    TEST_ASSERT_FALSE(w.s.sky_on);

    /* "On" without an answer: the off goes out at once for the request. */
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, NULL));
    TEST_ASSERT_EQUAL(STN_JOB_SKY, sim_next(&w).kind);
    sky_done(&w, STN_E_TIMEOUT);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
    TEST_ASSERT_EQUAL(STN_MAINT_SKY_OUTPUT, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    sky_done(&w, STN_OK);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));

    /* No receiver to send it to: not held back. */
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);
    w.rtcm_on = false;
    stn_request_recovery(&w.s, STN_REC_RESCAN, true);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_next(&w).kind);
    fail_pass(&w, false);
    TEST_ASSERT_TRUE(w.s.state == STN_DETECTING || w.s.state == STN_NO_RECEIVER);
    TEST_ASSERT_TRUE(w.s.sky_on);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));

    /* A profile that sends it permanently owns it: nothing to turn off. */
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);
    stn_live_stop(&w.s, w.in.now_ms);
    w.in.profile_sky = true;
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
}

TEST_CASE("station core: the live view's output is turned off after an ESP32 reset", "[station]")
{
    stn_sky_rtc_t r;
    uint32_t stamp = 0;
    stn_sky_rtc_write(&r, 0x5Au, true);
    TEST_ASSERT_TRUE(stn_sky_rtc_read(&r, &stamp));
    TEST_ASSERT_EQUAL_HEX32(0x5Au, stamp);
    stn_sky_rtc_t bad = r;
    bad.stamp ^= 1u;   /* damaged */
    TEST_ASSERT_FALSE(stn_sky_rtc_read(&bad, &stamp));
    stn_sky_rtc_write(&r, 0x5Au, false);   /* cleared */
    TEST_ASSERT_FALSE(stn_sky_rtc_read(&r, &stamp));
    memset(&r, 0xFF, sizeof(r));            /* random memory after a power cut */
    TEST_ASSERT_FALSE(stn_sky_rtc_read(&r, &stamp));

    /* The receiver kept it: the off goes out once the station streams, with
     * no window open and although the applied check passes. */
    sim_t w;
    sim_init(&w, false);
    stn_sky_restore(&w.s);
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_STOPPING, live_status(&w).sky);
    boot_streaming(&w, true);
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_SKY, j.kind);
    TEST_ASSERT_FALSE(j.sky_on);
    sky_done(&w, STN_OK);
    TEST_ASSERT_FALSE(w.s.sky_on);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 40).kind);
    TEST_ASSERT_TRUE(w.s.profile_applied);
}

/* ---- receiver update sessions, bench operations, package checks ----------------------- */

static stn_result_t flash_end(stn_flash_end_t end, bool supported)
{
    stn_result_t r = res(STN_OK);
    r.flash_end = (uint8_t)end;
    r.supported = supported;
    return r;
}

TEST_CASE("station core: an update runs as FLASHING, then re-applies whatever autocfg says and closes after its check", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    live_on(&w);
    w.in.autocfg = false;
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_flash(&w.s, STN_FLASH_NEW));   /* one at a time */
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, j.kind);
    TEST_ASSERT_EQUAL_UINT8(STN_FLASH_NEW, j.flash_kind);
    TEST_ASSERT_EQUAL(STN_FLASHING, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    TEST_ASSERT_FALSE(w.s.live_open);   /* the live view closed for the update */
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_FLASH, w.s.live_last_end);
    /* Maintenance and the live view are refused while it runs. */
    TEST_ASSERT_EQUAL(STN_MAINT_FLASH, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    uint8_t why = 0;
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_live_start(&w.s, LIVE_MS, w.in.now_ms, &why));
    TEST_ASSERT_EQUAL_UINT8(STN_SKY_WHY_FLASH, why);

    stn_result_t r = flash_end(STN_FLASH_END_REAPPLY, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);   /* autocfg off: a flash session re-applies anyway */
    TEST_ASSERT_EQUAL(STN_WHY_FLASH, j.why);
    TEST_ASSERT_TRUE(j.force_mode);
    TEST_ASSERT_FALSE(has_note(&w, STN_NOTE_FLASH_CLOSE, NULL));
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    /* The journal closes once the re-apply's window is judged. */
    stn_note_t n;
    bool closed = false;
    for (int i = 0; i < 60 && !closed; i++) {
        sim_tick(&w);
        TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_next(&w).kind);
        closed = has_note(&w, STN_NOTE_FLASH_CLOSE, &n);
    }
    TEST_ASSERT_TRUE(closed);
    TEST_ASSERT_EQUAL_INT32(1, n.a);
    TEST_ASSERT_EQUAL_UINT8(0, w.s.flash_close);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_RECEIVER_ATTENTION);
}

TEST_CASE("station core: after the abort path RTCM must come back within 120 s, else attention", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    stn_result_t r = flash_end(STN_FLASH_END_REAPPLY_RTCM, true);
    w.rtcm_on = false;
    sim_done(&w, &r);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    stn_note_t n;
    bool closed = false;
    int secs = 0;
    while (!closed && secs < 200) {
        sim_tick(&w);
        secs++;
        if (sim_next(&w).kind == STN_JOB_ASK) {
            sim_done(&w, &(stn_result_t){ .err = STN_E_TIMEOUT });   /* the station's question, unanswered */
        }
        closed = has_note(&w, STN_NOTE_FLASH_CLOSE, &n);
    }
    TEST_ASSERT_TRUE(closed);
    TEST_ASSERT_EQUAL_INT32(0, n.a);
    TEST_ASSERT_TRUE(secs >= 119 && secs <= 122);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_RECEIVER_ATTENTION);

    /* With RTCM back it closes at once. */
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    closed = false;
    for (int i = 0; i < 5 && !closed; i++) {
        sim_tick(&w);
        (void)sim_next(&w);
        closed = has_note(&w, STN_NOTE_FLASH_CLOSE, &n);
    }
    TEST_ASSERT_TRUE(closed);
    TEST_ASSERT_EQUAL_INT32(1, n.a);
}

TEST_CASE("station core: a failed re-apply after an update closes the journal with attention", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    stn_result_t r = flash_end(STN_FLASH_END_REAPPLY, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);
    stn_result_t f = res(STN_E_FAIL);
    f.answered = true;
    sim_done(&w, &f);
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_FLASH_CLOSE, &n));
    TEST_ASSERT_EQUAL_INT32(0, n.a);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
}

TEST_CASE("station core: a session waiting for the user, then the user's retry and restart", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    stn_result_t r = flash_end(STN_FLASH_END_NEEDS_USER, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_FLASH_RECOVERY, w.s.state);
    TEST_ASSERT_TRUE(w.s.paused);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 600).kind);   /* nothing sent until the user acts */
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_bench(&w.s));

    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_RETRY));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, j.kind);
    TEST_ASSERT_EQUAL_UINT8(STN_FLASH_RETRY, j.flash_kind);
    r = flash_end(STN_FLASH_END_NOTHING, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_FLASH_RECOVERY, w.s.state);   /* changed nothing: still waiting */

    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_RESTART));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL_UINT8(STN_FLASH_RESTART, j.flash_kind);
    r = flash_end(STN_FLASH_END_REAPPLY, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, sim_next(&w).kind);

    /* Boot-time recovery with actions built in is the same job from FLASH_RECOVERY. */
    sim_init(&w, true);
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_FALSE(stn_flash_pending(&w.s));
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_RECOVER));
    TEST_ASSERT_TRUE(stn_flash_pending(&w.s));
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, j.kind);
    TEST_ASSERT_EQUAL_UINT8(STN_FLASH_RECOVER, j.flash_kind);
    TEST_ASSERT_TRUE(stn_flash_pending(&w.s));   /* running */
    r = flash_end(STN_FLASH_END_NEEDS_USER, true);
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(stn_flash_pending(&w.s));
}

TEST_CASE("station core: clearing the journal drops a retry still queued for it", "[station]")
{
    sim_t w;
    sim_init(&w, true);
    w.rtcm_on = true;
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_RETRY));
    TEST_ASSERT_TRUE(stn_flash_pending(&w.s));
    stn_journal_cleared(&w.s, &w.in);
    TEST_ASSERT_FALSE(stn_flash_pending(&w.s));
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    /* The normal start, and no flash job for a journal that no longer exists. */
    for (int i = 0; i < 120; i++) {
        stn_job_t j = sim_next(&w);
        TEST_ASSERT_NOT_EQUAL(STN_JOB_FLASH, j.kind);
        if (j.kind == STN_JOB_IDENTIFY) {
            identity_done(&w, true, true);
        } else if (j.kind != STN_JOB_NONE) {
            stn_result_t r = res(STN_OK);
            sim_done(&w, &r);
        }
        sim_tick(&w);
    }
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));   /* nothing stale holds a new update back */
}

TEST_CASE("station core: an update that changed nothing streams on; a receiver left at its loader is listened for", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    stn_result_t r = flash_end(STN_FLASH_END_NOTHING, true);
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_FALSE(w.s.paused);

    TEST_ASSERT_EQUAL(STN_OK, stn_request_flash(&w.s, STN_FLASH_NEW));
    TEST_ASSERT_EQUAL(STN_JOB_FLASH, sim_next(&w).kind);
    r = flash_end(STN_FLASH_END_NOTHING, true);
    r.in_loader = true;
    r.loader_key_sent = true;
    w.rtcm_on = false;   /* it stays at its loader */
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_RECEIVER_IN_LOADER);
    TEST_ASSERT_TRUE(w.s.loader_key_sent);
    stn_job_t j = sim_until_job(&w, 60);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_TRUE(j.listen_only);
}

TEST_CASE("station core: bench operations pause the casters and end with the stream-first path", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_bench(&w.s));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_bench(&w.s));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_BENCH, j.kind);
    TEST_ASSERT_TRUE(w.s.paused);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_bench(&w.s));   /* one runs */
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_EQUAL(STN_IDLE, w.s.state);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, sim_until_job(&w, 5).kind);

    /* Refused in maintenance and with a journal. */
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_bench(&w.s));
    sim_init(&w, true);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_bench(&w.s));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_pkg_check(&w.s));
}

TEST_CASE("station core: a package check runs without the receiver or a pause", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_pkg_check(&w.s));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_pkg_check(&w.s));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_PKG_CHECK, j.kind);
    TEST_ASSERT_FALSE(w.s.paused);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
}

/* ---- no receiver: alert, next search, "check again"; the live view at read time ---- */

TEST_CASE("station core: no receiver raises its alert with the next search time", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, stn_next_detect_in_s(&w.s, w.in.now_ms));   /* IDLE: the boot listen */
    stn_job_t j = sim_until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_EQUAL_UINT32(0, stn_next_detect_in_s(&w.s, w.in.now_ms));   /* the pass runs */
    TEST_ASSERT_EQUAL_HEX32(0, stn_alerts(&w.s) & STN_ALERT_NO_RECEIVER);   /* DETECTING: not yet */
    fail_pass(&w, false);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_EQUAL_HEX32(STN_ALERT_NO_RECEIVER, stn_alerts(&w.s) & STN_ALERT_NO_RECEIVER);
    TEST_ASSERT_EQUAL_HEX32(0, w.s.alerts & STN_ALERT_NO_RECEIVER);   /* derived, never stored */

    /* The schedule: 30 s apart in the first 5 min, counting down. */
    TEST_ASSERT_UINT32_WITHIN(1, STN_DETECT_FAST_MS / 1000, stn_next_detect_in_s(&w.s, w.in.now_ms));
    w.in.now_ms += 10000;
    TEST_ASSERT_UINT32_WITHIN(1, STN_DETECT_FAST_MS / 1000 - 10, stn_next_detect_in_s(&w.s, w.in.now_ms));

    /* Then every 5 min. */
    int64_t episode = w.s.episode_ms;
    while (w.in.now_ms - episode < STN_DETECT_FAST_SPAN_MS) {
        j = sim_until_job(&w, 40);
        TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
        fail_pass(&w, false);
    }
    TEST_ASSERT_UINT32_WITHIN(1, STN_DETECT_SLOW_MS / 1000, stn_next_detect_in_s(&w.s, w.in.now_ms));

    /* The receiver streams again: the alert clears, no search is scheduled. */
    w.in.now_ms += 5000;
    w.in.rtcm_frames += 1;
    (void)sim_next(&w);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL_HEX32(0, stn_alerts(&w.s) & STN_ALERT_NO_RECEIVER);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, stn_next_detect_in_s(&w.s, w.in.now_ms));
}

TEST_CASE("station core: the no-receiver alert gives way to the loader alert and to maintenance", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.prompt = true;   /* at its loader prompt, and the application did not come back */
    r.key_sent = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    uint32_t a = stn_alerts(&w.s);
    TEST_ASSERT_EQUAL_HEX32(STN_ALERT_RECEIVER_IN_LOADER, a & STN_ALERT_RECEIVER_IN_LOADER);
    TEST_ASSERT_EQUAL_HEX32(0, a & STN_ALERT_NO_RECEIVER);

    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    fail_pass(&w, false);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_EQUAL_HEX32(0, stn_alerts(&w.s) & STN_ALERT_NO_RECEIVER);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, stn_next_detect_in_s(&w.s, w.in.now_ms));
}

TEST_CASE("station core: check again starts the next search now and keeps the schedule", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_detect(&w.s, w.in.now_ms));   /* DETECTING: the first search runs */
    fail_pass(&w, false);
    int64_t episode = w.s.episode_ms;
    uint8_t short_fails = w.s.short_fails;

    w.in.now_ms += 5000;
    TEST_ASSERT_EQUAL(STN_OK, stn_request_detect(&w.s, w.in.now_ms));
    TEST_ASSERT_EQUAL_UINT32(0, stn_next_detect_in_s(&w.s, w.in.now_ms));
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);   /* at once, not 25 s later */
    TEST_ASSERT_EQUAL_INT64(episode, w.s.episode_ms);
    TEST_ASSERT_EQUAL_UINT8(short_fails, w.s.short_fails);
    TEST_ASSERT_FALSE(j.full);   /* the second short pass, as the schedule had it */
    fail_pass(&w, false);
    TEST_ASSERT_UINT32_WITHIN(1, STN_DETECT_FAST_MS / 1000, stn_next_detect_in_s(&w.s, w.in.now_ms));

    /* Refused while streaming, and in maintenance. */
    w.in.now_ms += 1000;
    w.in.rtcm_frames += 1;
    (void)sim_next(&w);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_detect(&w.s, w.in.now_ms));

    sim_init(&w, false);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    fail_pass(&w, false);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 1800, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(STN_E_STATE, stn_request_detect(&w.s, w.in.now_ms));
}

TEST_CASE("station core: the live view's window reads right after a start or stop, mid-job", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    sim_start(&w);
    /* A detection pass runs (the station task is busy for most of a minute). */
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, sim_until_job(&w, 5).kind);
    TEST_ASSERT_TRUE(w.s.job_busy);

    uint8_t why = 0xFF;
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, 10u * 60000u, w.in.now_ms, &why));
    stn_live_status_t lv;
    stn_live_status(&w.s, &w.in, &lv);   /* no stn_next() / stn_done() in between */
    TEST_ASSERT_TRUE(lv.open);
    TEST_ASSERT_UINT32_WITHIN(1, 600, lv.left_s);
    TEST_ASSERT_EQUAL_UINT32(0, lv.open_s);

    w.in.now_ms += 3000;
    stn_live_stop(&w.s, w.in.now_ms);
    stn_live_status(&w.s, &w.in, &lv);
    TEST_ASSERT_FALSE(lv.open);
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_USER, lv.last_end);
    TEST_ASSERT_EQUAL_UINT32(0, lv.left_s);

    /* Opened again: open, the previous end forgotten. */
    TEST_ASSERT_EQUAL(STN_OK, stn_live_start(&w.s, 0, w.in.now_ms, &why));
    stn_live_status(&w.s, &w.in, &lv);
    TEST_ASSERT_TRUE(lv.open);
    TEST_ASSERT_EQUAL_UINT8(STN_LIVE_END_NONE, lv.last_end);
    TEST_ASSERT_UINT32_WITHIN(1, STN_LIVE_DEFAULT_MS / 1000, lv.left_s);
}

/* ---- first-apply hold ------------------------------------------------------------------ */

TEST_CASE("station core: the first-apply hold at boot, decision table", "[station]")
{
    stn_hold_writes_t w;
    /* Build option off: never anything (the station build configures at first boot). */
    TEST_ASSERT_FALSE(stn_hold_at_boot(false, true, false, false, false, &w));
    TEST_ASSERT_FALSE(w.autocfg_off || w.recovery_alerts || w.hold);
    /* A fresh device in a hold build: all three. */
    TEST_ASSERT_TRUE(stn_hold_at_boot(true, true, false, false, false, &w));
    TEST_ASSERT_TRUE(w.autocfg_off);
    TEST_ASSERT_TRUE(w.recovery_alerts);
    TEST_ASSERT_TRUE(w.hold);
    /* A profile applied once (gnss/applied_hash): nothing. */
    TEST_ASSERT_FALSE(stn_hold_at_boot(true, true, true, false, false, &w));
    TEST_ASSERT_FALSE(w.autocfg_off || w.recovery_alerts || w.hold);
    /* gnss/autocfg set (the user's choice, or a held device's later boots): nothing. */
    TEST_ASSERT_FALSE(stn_hold_at_boot(true, true, false, true, false, &w));
    TEST_ASSERT_FALSE(w.autocfg_off || w.recovery_alerts || w.hold);
    /* A recovery mode the user chose stays: autocfg and the hold only. */
    TEST_ASSERT_TRUE(stn_hold_at_boot(true, true, false, false, true, &w));
    TEST_ASSERT_TRUE(w.autocfg_off);
    TEST_ASSERT_FALSE(w.recovery_alerts);
    TEST_ASSERT_TRUE(w.hold);
    TEST_ASSERT_FALSE(w.ram_only);
    /* Settings unavailable this boot: whether the device was ever configured cannot be read, so the hold
     * fails closed for this boot (autocfg off, held), and nothing is written. */
    TEST_ASSERT_TRUE(stn_hold_at_boot(true, false, false, false, false, &w));
    TEST_ASSERT_TRUE(w.ram_only && w.hold && w.autocfg_off);
    TEST_ASSERT_FALSE(w.recovery_alerts);
    TEST_ASSERT_TRUE(stn_hold_at_boot(true, false, true, true, true, &w));   /* presences unreadable: counted as stored */
    TEST_ASSERT_TRUE(w.ram_only && w.hold && w.autocfg_off);
    /* ... but never in a build without the option. */
    TEST_ASSERT_FALSE(stn_hold_at_boot(false, false, false, false, false, &w));
    TEST_ASSERT_FALSE(w.autocfg_off || w.recovery_alerts || w.hold || w.ram_only);
}

/* Boot with nothing on the link up to the first detection pass. */
static stn_job_t boot_silent_to_detect(sim_t *w)
{
    w->rtcm_on = false;
    sim_start(w);
    stn_job_t j = sim_until_job(w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    return j;
}

TEST_CASE("station core: while held, a loader prompt gets no restart key; the user's restart still works",
          "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;   /* as the hold writes it */
    stn_set_first_apply_held(&w.s, true);
    stn_job_t j = boot_silent_to_detect(&w);
    TEST_ASSERT_FALSE(j.loader_key);
    TEST_ASSERT_FALSE(j.listen_only);

    /* The pass met the loader prompt and sent nothing: at its loader, listen-only. */
    stn_result_t r = res(STN_E_NOT_FOUND);
    r.prompt = true;
    r.saw_bytes = true;
    sim_done(&w, &r);
    TEST_ASSERT_EQUAL(STN_NO_RECEIVER, w.s.state);
    TEST_ASSERT_TRUE(w.s.alerts & STN_ALERT_RECEIVER_IN_LOADER);
    TEST_ASSERT_FALSE(w.s.loader_key_sent);
    j = sim_until_job(&w, 40);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_TRUE(j.listen_only);
    TEST_ASSERT_FALSE(j.loader_key);
    sim_done(&w, &r);

    /* The user's receiver restart goes out as before. */
    TEST_ASSERT_EQUAL(STN_OK, stn_request_restart_receiver(&w.s));
    j = sim_until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_LEAVE_LOADER, j.kind);
}

TEST_CASE("station core: the ladder's rescan sends no loader key while held; the hold's end allows it again",
          "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_set_first_apply_held(&w.s, true);
    TEST_ASSERT_EQUAL(STN_OK, stn_request_recovery(&w.s, STN_REC_RESCAN, true));
    stn_job_t j = sim_until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_DETECT, j.kind);
    TEST_ASSERT_TRUE(j.break_first);
    TEST_ASSERT_FALSE(j.loader_key);
    sim_done(&w, &(stn_result_t){ .err = STN_OK });

    /* Released (applied, or autocfg turned on): the next detection may send the boot's one key. */
    sim_t v;
    sim_init(&v, false);
    stn_set_first_apply_held(&v.s, true);
    stn_set_first_apply_held(&v.s, false);
    j = boot_silent_to_detect(&v);
    TEST_ASSERT_TRUE(j.loader_key);
}

TEST_CASE("station core: a held device applies nothing by itself; the user's apply runs", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    w.in.autocfg = false;
    stn_set_first_apply_held(&w.s, true);
    boot_streaming(&w, false);   /* never applied: the static check fails */
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 120).kind);
    TEST_ASSERT_FALSE(w.s.profile_applied);
    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_USER, j.why);
}

TEST_CASE("station core: a plain signal-group refusal is applied back at once, no restart", "[station]")
{
    /* The receiver said no and changed nothing: the session was not rolled
     * back with RESET (nothing restarted), the group it runs went back into
     * the profile and is applied again right away. */
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    stn_request_apply(&w.s);
    stn_job_t j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    stn_result_t r = res(STN_E_FAIL);
    r.sg_back = true;
    r.restarted = false;
    r.answered = true;
    sim_done(&w, &r);
    TEST_ASSERT_FALSE(w.s.alerts & STN_ALERT_CONFIG_FAILED);
    TEST_ASSERT_FALSE(w.s.last_resort);
    j = sim_next(&w);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_WHY_SG_BACK, j.why);
}

TEST_CASE("station core: the signal report raises and clears its alert", "[station]")
{
    sim_t w;
    sim_init(&w, false);
    boot_applied(&w);
    TEST_ASSERT_FALSE(stn_alerts(&w.s) & STN_ALERT_SIGNALS_MISMATCH);
    stn_note_signals(&w.s, 0);   /* the receiver reports other signals than the profile's group gives */
    TEST_ASSERT_TRUE(stn_alerts(&w.s) & STN_ALERT_SIGNALS_MISMATCH);
    TEST_ASSERT_FALSE(stn_alerts(&w.s) & STN_ALERT_CONFIG_FAILED);
    stn_note_signals(&w.s, 1);   /* a later report matches */
    TEST_ASSERT_FALSE(stn_alerts(&w.s) & STN_ALERT_SIGNALS_MISMATCH);
    stn_note_signals(&w.s, 0);
    stn_note_signals(&w.s, -1);  /* a firmware that does not report: unknown, no alert to stand on */
    TEST_ASSERT_FALSE(stn_alerts(&w.s) & STN_ALERT_SIGNALS_MISMATCH);
}

/* ---- power-on reset: did the receiver keep running? ------------------------------ */

static stn_rxboot_in_t rxb(int64_t now, int64_t restart, int64_t data, int64_t uptime_at, uint32_t uptime)
{
    stn_rxboot_in_t in = { .now_ms = now, .restart_ms = restart, .data_ms = data, .uptime_at_ms = uptime_at,
                           .uptime_ms = uptime };
    return in;
}

TEST_CASE("station core: a power-on boot is a receiver restart only on the receiver's word", "[station]")
{
    stn_rxboot_by_t by;
    stn_rxboot_in_t in;

    /* Nothing yet: pending, then unknown once the window is over. */
    in = rxb(1500, STN_NEVER, STN_NEVER, STN_NEVER, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_PENDING, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_NONE, by);
    in.now_ms = STN_RXBOOT_WINDOW_MS - 1;
    TEST_ASSERT_EQUAL(STN_RXBOOT_PENDING, stn_rxboot_judge(&in, NULL));
    in.now_ms = STN_RXBOOT_WINDOW_MS;
    TEST_ASSERT_EQUAL(STN_RXBOOT_UNKNOWN, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_NONE, by);

    /* The bench's EN reset with sky: RTCM at once, no banner: kept running. */
    in = rxb(2000, STN_NEVER, 1800, STN_NEVER, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_STREAM, by);

    /* The bench's EN reset without sky (COM1 silent): the detection probe's
     * version log says the receiver has run for 1014 s. */
    in = rxb(6500, STN_NEVER, STN_NEVER, 6200, 1014000);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_UPTIME, by);

    /* A power cut: the receiver boots after the ESP32 and announces it. */
    in = rxb(5200, 5000, STN_NEVER, STN_NEVER, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_RESTART, by);
    in = rxb(7000, 5000, 6600, STN_NEVER, 0);   /* its GGA 1.6 s after the banner changes nothing */
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_RESTART, by);

    /* A power cut whose banner was missed: its clock started after the ESP32's. */
    in = rxb(9500, STN_NEVER, STN_NEVER, 9000, 2500);
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_UPTIME, by);
    /* ... and the zeroed header right after its banner. */
    in = rxb(9500, STN_NEVER, STN_NEVER, 9000, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, NULL));
    /* Its clock began before the ESP32's, but by less than the margin: not "kept". */
    in = rxb(9500, STN_NEVER, STN_NEVER, 9000, 9000 + STN_RXBOOT_MARGIN_MS);
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, NULL));
    in.uptime_ms = 9000 + STN_RXBOOT_MARGIN_MS + 1;
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, NULL));

    /* A restart after data had come is a later one of its own (the user's
     * RESET through its other port): the data at once decided. */
    in = rxb(12000, 10000, 1800, STN_NEVER, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_STREAM, by);
    /* ... and after its version log said it kept running. */
    in = rxb(12000, 10000, STN_NEVER, 6200, 1014000);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_UPTIME, by);

    /* Data that came late (a slow start, or a receiver that booted fast and
     * whose banner was missed) tells nothing: unknown. */
    in = rxb(STN_RXBOOT_WINDOW_MS, STN_NEVER, STN_RXBOOT_DATA_MS + 1, STN_NEVER, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_UNKNOWN, stn_rxboot_judge(&in, NULL));
    in.data_ms = STN_RXBOOT_DATA_MS;
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, NULL));

    /* Evidence after the window does not count. */
    in = rxb(STN_RXBOOT_WINDOW_MS + 5000, STN_RXBOOT_WINDOW_MS + 1, STN_NEVER, STN_RXBOOT_WINDOW_MS + 2, 0);
    TEST_ASSERT_EQUAL(STN_RXBOOT_UNKNOWN, stn_rxboot_judge(&in, NULL));

    /* The earliest evidence decides: a version log before a banner ... */
    in = rxb(9000, 8000, STN_NEVER, 6000, 600000);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_UPTIME, by);
    /* ... a banner before a version log; a tie goes to the receiver's clock. */
    in = rxb(9000, 5000, STN_NEVER, 8000, 2000);
    TEST_ASSERT_EQUAL(STN_RXBOOT_RESTARTED, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_RESTART, by);
    in = rxb(3000, STN_NEVER, 2000, 2000, 900000);
    TEST_ASSERT_EQUAL(STN_RXBOOT_KEPT, stn_rxboot_judge(&in, &by));
    TEST_ASSERT_EQUAL(STN_RXBOOT_BY_UPTIME, by);
}

TEST_CASE("station core: a brownout boot waits for the receiver's word too; one that restarted with it ends a resumed "
          "maintenance",
          "[station]")
{
    TEST_ASSERT_TRUE(stn_rxboot_judged(STN_RXBOOT_RESET_POWER_ON));
    TEST_ASSERT_TRUE(stn_rxboot_judged(STN_RXBOOT_RESET_BROWNOUT));
    TEST_ASSERT_FALSE(stn_rxboot_judged(STN_RXBOOT_RESET_OTHER));

    stn_rxboot_follow_t f;
    /* The receiver restarted with the ESP32: the power went for both. */
    stn_rxboot_follow(STN_RXBOOT_RESET_BROWNOUT, STN_RXBOOT_RESTARTED, &f);
    TEST_ASSERT_TRUE(f.maint_ends);
    TEST_ASSERT_FALSE(f.power_kept);
    stn_rxboot_follow(STN_RXBOOT_RESET_POWER_ON, STN_RXBOOT_RESTARTED, &f);
    TEST_ASSERT_TRUE(f.maint_ends);
    TEST_ASSERT_FALSE(f.power_kept);
    /* It kept running: maintenance goes on; only a power-on reset (the EN pin)
     * was no power loss, a brownout is the ESP32's own dip. */
    stn_rxboot_follow(STN_RXBOOT_RESET_BROWNOUT, STN_RXBOOT_KEPT, &f);
    TEST_ASSERT_FALSE(f.maint_ends);
    TEST_ASSERT_FALSE(f.power_kept);
    stn_rxboot_follow(STN_RXBOOT_RESET_POWER_ON, STN_RXBOOT_KEPT, &f);
    TEST_ASSERT_FALSE(f.maint_ends);
    TEST_ASSERT_TRUE(f.power_kept);
    /* Unknown: neither. Another reset: nothing to judge. */
    stn_rxboot_follow(STN_RXBOOT_RESET_BROWNOUT, STN_RXBOOT_UNKNOWN, &f);
    TEST_ASSERT_FALSE(f.maint_ends || f.power_kept);
    stn_rxboot_follow(STN_RXBOOT_RESET_OTHER, STN_RXBOOT_RESTARTED, &f);
    TEST_ASSERT_FALSE(f.maint_ends || f.power_kept);

    /* A brownout during maintenance (the RTC memory kept): it resumes; the
     * receiver's banner 4.4 s into the boot, before any data, ends it. */
    sim_t w;
    sim_init(&w, false);
    w.rtcm_on = true;
    resume(&w, STN_MAINT_FROM_RTC, 1200000, 1800, 0);
    sim_start(&w);
    TEST_ASSERT_EQUAL(STN_JOB_NONE, sim_until_job(&w, 10).kind);
    stn_rxboot_in_t in = rxb(5000, 4400, STN_NEVER, STN_NEVER, 0);
    stn_rxboot_follow(STN_RXBOOT_RESET_BROWNOUT, stn_rxboot_judge(&in, NULL), &f);
    TEST_ASSERT_TRUE(f.maint_ends);
    TEST_ASSERT_TRUE(stn_maint_power_cut(&w.s, &w.in));
    TEST_ASSERT_FALSE(w.s.maint);
    stn_note_t n;
    TEST_ASSERT_TRUE(has_note(&w, STN_NOTE_MAINT_OFF, &n));
    TEST_ASSERT_EQUAL_INT32(STN_MAINT_END_POWER, n.b);
    /* Its RTCM at once: only the ESP32's supply dipped, maintenance goes on. */
    sim_t k;
    sim_init(&k, false);
    resume(&k, STN_MAINT_FROM_RTC, 1200000, 1800, 0);
    in = rxb(2000, STN_NEVER, 300, STN_NEVER, 0);
    stn_rxboot_follow(STN_RXBOOT_RESET_BROWNOUT, stn_rxboot_judge(&in, NULL), &f);
    TEST_ASSERT_FALSE(f.maint_ends);
    TEST_ASSERT_TRUE(k.s.maint);
}

TEST_CASE("station core: NMEA sentences tell data from command-layer lines", "[station]")
{
    static const char *const yes[] = { "$GPGGA,123519,4807.038,N*47", "$GNRMC,,V*00", "$GPGSV,1,1,00,,,,,*55" };
    static const char *const no[] = { "$devicename,COM1*67", "$command,VERSIONA,response: OK*45",
                                      "$CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 6*01", "#VERSIONA,79,GPS;x*00",
                                      "The Board is resetting,do not response any command!*39", "$GPGG", "$gpgga,1*00",
                                      "" };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(stn_line_is_nmea(yes[i], strlen(yes[i])), yes[i]);
    }
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(stn_line_is_nmea(no[i], strlen(no[i])), no[i]);
    }
    TEST_ASSERT_FALSE(stn_line_is_nmea(NULL, 10));
}
