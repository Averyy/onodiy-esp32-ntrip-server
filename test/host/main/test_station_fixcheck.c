/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station core's fixed-position check (station_core.h): when the
 * receiver's own position is asked, what three answers decide, the warning
 * and its notes, and what drops or re-arms the check. A small world: a
 * receiver that streams and answers, job outcomes chosen by each test.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "station_core.h"
#include "unity.h"

#define T0      ((int64_t)UINT32_MAX - 5000)
#define MIN_MS  60000
#define HOUR_MS (60 * MIN_MS)

#define FIX_LAT 45.4
#define FIX_LON -75.7
#define FIX_H   100.0
#define M_PER_DEG_LAT 111132.0

typedef struct {
    stn_t s;
    stn_in_t in;
} world_t;

static void world_init(world_t *w, bool fixed)
{
    memset(w, 0, sizeof(*w));
    stn_init(&w->s, false);
    w->in.now_ms = T0;
    w->in.autocfg = true;
    w->in.model_known = true;
    w->in.profile_survey = !fixed;
    w->in.survey_s = 600;
    w->in.rtcm1230_ok = true;
    w->in.fixed_valid = fixed;
    w->in.fixed_llh[0] = FIX_LAT;
    w->in.fixed_llh[1] = FIX_LON;
    w->in.fixed_llh[2] = FIX_H;
}

static void tick(world_t *w, int64_t ms)
{
    w->in.now_ms += ms;
    w->in.rtcm_frames += 8;   /* the receiver streams */
}

static stn_result_t res(stn_err_t err)
{
    stn_result_t r;
    memset(&r, 0, sizeof(r));
    r.err = (uint8_t)err;
    return r;
}

/* Run in 1 s steps until a job comes (STN_JOB_NONE after max_s). */
static stn_job_t until_job(world_t *w, int max_s)
{
    for (int i = 0; i <= max_s; i++) {
        stn_job_t j = stn_next(&w->s, &w->in);
        if (j.kind != STN_JOB_NONE) {
            return j;
        }
        tick(w, 1000);
    }
    stn_job_t none;
    memset(&none, 0, sizeof(none));
    return none;
}

/* secs of world time without a position query (other jobs end in a timeout). */
static void no_query(world_t *w, int secs)
{
    for (int i = 0; i < secs; i++) {
        stn_job_t j = stn_next(&w->s, &w->in);
        TEST_ASSERT_NOT_EQUAL(STN_JOB_OWN_FIX, j.kind);
        if (j.kind != STN_JOB_NONE) {
            stn_result_t r = res(STN_E_TIMEOUT);
            stn_done(&w->s, &r, &w->in);
        }
        tick(w, 1000);
    }
}

/* Boot to STREAMING with the identity confirmed and the profile applied. */
static void boot(world_t *w, bool supported)
{
    stn_start(&w->s, &w->in);
    stn_job_t j = until_job(w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    stn_result_t r = res(STN_OK);
    r.supported = supported;
    r.static_ok = true;
    stn_done(&w->s, &r, &w->in);
    TEST_ASSERT_EQUAL(STN_STREAMING, w->s.state);
}

/* A usable single-point answer north_m north of the fixed position and up_m above it. */
static stn_result_t answer(double north_m, double up_m)
{
    stn_result_t r = res(STN_OK);
    r.own.computed = true;
    r.own.pos_type = UM98X_OWN_POS_SINGLE;
    r.own.lat_deg = FIX_LAT + north_m / M_PER_DEG_LAT;
    r.own.lon_deg = FIX_LON;
    r.own.separation_m = -34.0;
    r.own.msl_height_m = FIX_H + up_m + 34.0;
    r.own.sd_lat_m = 1.0;
    r.own.sd_lon_m = 1.0;
    r.own.sd_hgt_m = 2.0;
    r.own.sats_tracked = 40;
    r.own.sats_used = 30;
    return r;
}

/* The next query (fails the test if none comes within max_s), answered with r. */
static int64_t query(world_t *w, int max_s, const stn_result_t *r)
{
    int64_t from = w->in.now_ms;
    stn_job_t j = until_job(w, max_s);
    TEST_ASSERT_EQUAL_MESSAGE(STN_JOB_OWN_FIX, j.kind, "no position query");
    TEST_ASSERT_FALSE(w->s.paused);   /* the casters never wait for it */
    TEST_ASSERT_EQUAL(STN_STREAMING, w->s.state);
    stn_done(&w->s, r, &w->in);
    return w->in.now_ms - from;
}

static unsigned notes(world_t *w, stn_note_t *out)
{
    return stn_take_notes(&w->s, out, STN_NOTES);
}

/* Raise the warning: three answers 120 m off. */
static void raise_far(world_t *w)
{
    stn_result_t far = answer(120.0, 0.0);
    for (int i = 0; i < 3; i++) {
        query(w, 120, &far);
    }
    TEST_ASSERT_TRUE((stn_alerts(&w->s) & STN_ALERT_FIXED_FAR) != 0);
}

TEST_CASE("station fixcheck: a fixed profile asks the receiver's own position a minute after streaming began",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    int64_t t_boot = w.in.now_ms;
    stn_job_t j = until_job(&w, 120);
    TEST_ASSERT_EQUAL(STN_JOB_OWN_FIX, j.kind);
    TEST_ASSERT_FALSE(j.sppnav);   /* the base-position log first */
    TEST_ASSERT_FALSE(w.s.paused);
    TEST_ASSERT_INT64_WITHIN(5000, STN_FIX_FIRST_MS, w.in.now_ms - t_boot);
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_CHECKING, st.state);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, st.checked_age_s);

    /* A survey profile: never asked. */
    world_init(&w, false);
    boot(&w, true);
    no_query(&w, 600);
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_OFF, st.state);

    /* Not a model we configure: never asked either. */
    world_init(&w, true);
    boot(&w, false);
    no_query(&w, 600);
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_OFF, st.state);
}

TEST_CASE("station fixcheck: three answers 120 m off raise the warning once; the next check is an hour later",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t far = answer(120.0, 0.0);
    query(&w, 120, &far);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_PERIOD_MS, query(&w, 120, &far));
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);   /* two are not enough */
    query(&w, 120, &far);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);   /* streaming goes on */
    stn_note_t n[STN_NOTES];
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_FAR, n[0].kind);
    TEST_ASSERT_EQUAL_INT32(120, n[0].a);
    TEST_ASSERT_EQUAL_INT32(0, n[0].b);

    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms + 3000, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, st.state);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 120.0, st.offset.horizontal_m);
    TEST_ASSERT_FALSE(st.height_only);
    TEST_ASSERT_EQUAL_UINT32(3, st.checked_age_s);

    /* Decided: nothing more for an hour, then three answers again; the same verdict notes nothing. */
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_RECHECK_MS, query(&w, 2 * 3600, &far));
    query(&w, 120, &far);
    query(&w, 120, &far);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));
}

TEST_CASE("station fixcheck: the height alone, 80 m off, raises it too", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t high = answer(0.0, -80.0);   /* the receiver 80 m below the fixed height */
    for (int i = 0; i < 3; i++) {
        query(&w, 120, &high);
    }
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, st.state);
    TEST_ASSERT_TRUE(st.height_only);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 80.0, st.offset.up_m);
    stn_note_t n[STN_NOTES];
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_INT32(80, n[0].b);
}

TEST_CASE("station fixcheck: a position within the limits raises nothing; fixed later, the warning clears with a note",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t near = answer(3.0, -2.0);
    for (int i = 0; i < 3; i++) {
        query(&w, 120, &near);
    }
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NEAR, st.state);
    stn_note_t n[STN_NOTES];
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));

    /* Far, then 45 m (inside 50 m, not inside 40 m): undecided, the warning stays; then 2 m clears it. */
    world_init(&w, true);
    boot(&w, true);
    raise_far(&w);
    (void)notes(&w, n);
    stn_result_t mid = answer(45.0, 0.0);
    query(&w, 2 * 3600, &mid);
    for (int i = 1; i < STN_FIX_TRIES; i++) {
        query(&w, 120, &mid);
    }
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));
    stn_result_t ok = answer(2.0, 1.0);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_RECHECK_MS, query(&w, 2 * 3600, &ok));
    query(&w, 120, &ok);
    query(&w, 120, &ok);
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_NEAR, n[0].kind);
    TEST_ASSERT_EQUAL_INT32(2, n[0].a);
}

TEST_CASE("station fixcheck: missing data decides nothing; after 15 tries the check ends without a verdict",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t none = res(STN_E_TIMEOUT);            /* no log */
    stn_result_t nosky = res(STN_OK);                  /* a log without a solution: no antenna */
    nosky.own.pos_type = UM98X_OWN_POS_NONE;
    stn_result_t few = answer(120.0, 0.0);             /* 120 m off, but from 5 satellites */
    few.own.sats_used = 5;
    stn_result_t wide = answer(120.0, 0.0);            /* ... or with a wide deviation */
    wide.own.sd_hgt_m = 25.0;
    const stn_result_t *seq[] = { &none, &nosky, &few, &wide };
    for (int i = 0; i < STN_FIX_TRIES; i++) {
        query(&w, 120, seq[i % 4]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NO_SOLUTION, st.state);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, st.checked_age_s);
    /* Asked again an hour later. */
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_RECHECK_MS, query(&w, 2 * 3600, &none));

    /* A link busy (nothing sent) is no try: asked again 5 s later, the count unchanged. */
    uint8_t tries = w.s.fix_tries;
    stn_result_t busy = res(STN_E_STATE);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_PERIOD_MS, query(&w, 120, &busy));
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_BUSY_MS, query(&w, 120, &none));
    TEST_ASSERT_EQUAL_UINT8(tries + 1, w.s.fix_tries);
}

TEST_CASE("station fixcheck: the single-point log once the base-position log is refused or only echoes the position",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t refused = res(STN_E_FAIL);
    stn_job_t j = until_job(&w, 120);
    TEST_ASSERT_EQUAL(STN_JOB_OWN_FIX, j.kind);
    TEST_ASSERT_FALSE(j.sppnav);
    stn_done(&w.s, &refused, &w.in);
    int64_t t = w.in.now_ms;
    j = until_job(&w, 120);
    TEST_ASSERT_TRUE(j.sppnav);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_BUSY_MS, w.in.now_ms - t);
    stn_done(&w.s, &refused, &w.in);   /* that one too: refused, the check ends */
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_REFUSED, st.state);
    t = w.in.now_ms;
    j = until_job(&w, 2 * 3600);
    TEST_ASSERT_EQUAL(STN_JOB_OWN_FIX, j.kind);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_RECHECK_MS, w.in.now_ms - t);
    stn_done(&w.s, &refused, &w.in);

    /* Three base-position answers without a usable solution: the single-point log next. */
    world_init(&w, true);
    boot(&w, true);
    stn_result_t nothing = res(STN_OK);
    nothing.own.pos_type = UM98X_OWN_POS_NONE;
    for (int i = 0; i < STN_FIX_BASEPOS_TRIES; i++) {
        j = until_job(&w, 120);
        TEST_ASSERT_FALSE(j.sppnav);
        stn_done(&w.s, &nothing, &w.in);
    }
    j = until_job(&w, 120);
    TEST_ASSERT_TRUE(j.sppnav);
    stn_done(&w.s, &nothing, &w.in);

    /* A base-position answer that only echoes the set position: the single-point log next. */
    world_init(&w, true);
    boot(&w, true);
    stn_result_t echo = answer(0.0, 0.0);
    echo.own.pos_type = UM98X_OWN_POS_FIXED;
    query(&w, 120, &echo);
    j = until_job(&w, 120);
    TEST_ASSERT_TRUE(j.sppnav);
    stn_result_t far = answer(150.0, 0.0);
    stn_done(&w.s, &far, &w.in);
    query(&w, 120, &far);
    query(&w, 120, &far);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
}

TEST_CASE("station fixcheck: a new fixed position or a survey profile drops the warning at once, with a note",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    raise_far(&w);
    stn_note_t n[STN_NOTES];
    (void)notes(&w, n);

    /* The user corrects the position: dropped, checked again a minute later. */
    w.in.fixed_llh[0] = FIX_LAT + 120.0 / M_PER_DEG_LAT;
    int64_t t = w.in.now_ms;
    (void)stn_next(&w.s, &w.in);
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_CHANGED, n[0].kind);
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_CHECKING, st.state);
    stn_result_t at = answer(120.0, 0.0);   /* where the corrected position is */
    query(&w, 120, &at);
    TEST_ASSERT_INT64_WITHIN(2000, STN_FIX_FIRST_MS, w.in.now_ms - t);
    query(&w, 120, &at);
    query(&w, 120, &at);
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NEAR, st.state);
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));   /* near from no verdict: nothing to clear */

    /* Back to survey-in with the warning raised: dropped with a note, never asked again. */
    world_init(&w, true);
    boot(&w, true);
    raise_far(&w);
    (void)notes(&w, n);
    w.in.fixed_valid = false;
    w.in.profile_survey = true;
    no_query(&w, 3 * 3600);
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_CHANGED, n[0].kind);
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_OFF, st.state);
}

TEST_CASE("station fixcheck: a receiver restart re-arms the check; the warning stays until it decides",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    raise_far(&w);
    /* Halfway through the hour, the receiver restarts: identity, then the check a minute after the restart. */
    stn_result_t none = res(STN_E_TIMEOUT);
    no_query(&w, 1800);
    stn_restart_seen(&w.s);
    int64_t t = w.in.now_ms;
    stn_job_t j = until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    TEST_ASSERT_TRUE(j.after_restart);
    stn_result_t r = res(STN_OK);
    r.supported = true;
    r.static_ok = true;
    stn_done(&w.s, &r, &w.in);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    query(&w, 120, &none);
    TEST_ASSERT_INT64_WITHIN(2000, STN_FIX_FIRST_MS, w.in.now_ms - t);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
}

TEST_CASE("station fixcheck: nothing is asked in receiver maintenance", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 30u * 60u, false, w.in.now_ms));
    for (int i = 0; i < 29 * 60; i++) {
        TEST_ASSERT_EQUAL(STN_JOB_NONE, stn_next(&w.s, &w.in).kind);
        tick(&w, 1000);
    }
}
