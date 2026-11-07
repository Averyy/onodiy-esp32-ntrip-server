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
    w->in.autocfg = false;   /* warning-only behavior; automatic relocation is tested separately below */
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

/* A usable single-point answer east_m east and north_m north of the fixed position, up_m above it. */
static stn_result_t answer_en(double east_m, double north_m, double up_m)
{
    stn_result_t r = res(STN_OK);
    r.own.computed = true;
    r.own.pos_type = UM98X_OWN_POS_SINGLE;
    r.own.lat_deg = FIX_LAT + north_m / M_PER_DEG_LAT;
    r.own.lon_deg = FIX_LON + east_m / (M_PER_DEG_LAT * cos(FIX_LAT * 3.14159265358979323846 / 180.0));
    r.own.separation_m = -34.0;
    r.own.msl_height_m = FIX_H + up_m + 34.0;
    r.own.sd_lat_m = 1.0;
    r.own.sd_lon_m = 1.0;
    r.own.sd_hgt_m = 2.0;
    r.own.sats_tracked = 40;
    r.own.sats_used = 30;
    return r;
}

/* ... due north. */
static stn_result_t answer(double north_m, double up_m)
{
    return answer_en(0.0, north_m, up_m);
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

/* The fix that first decides at one fix per STN_FIX_PERIOD_MS: enough of them, over enough time. */
#define FIRST_DECISION (STN_MOVE_MIN_SPAN_MS / STN_FIX_PERIOD_MS + 1 > STN_MOVE_MIN_SAMPLES ? \
                        STN_MOVE_MIN_SPAN_MS / STN_FIX_PERIOD_MS + 1 : STN_MOVE_MIN_SAMPLES)

/* n position queries, each answered with r. */
static void queries(world_t *w, int n, const stn_result_t *r)
{
    for (int i = 0; i < n; i++) {
        query(w, 120, r);
    }
}

TEST_CASE("automatic position: wind, uncertain fixes and single outliers do not start a survey", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t wind = answer(0.8, 0.0);
    wind.own.sd_lat_m = 0.05;
    wind.own.sd_lon_m = 0.05;   /* even a very optimistic receiver cannot lower the 3 m floor */
    queries(&w, 70, &wind);
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);   /* the window was full: judged, not short */
    TEST_ASSERT_FALSE(w.s.relocating);
    stn_result_t far = answer(20.0, 0.0);
    for (int i = 0; i < 70; i++) {
        query(&w, 120, i % 4 == 0 ? &far : &wind);   /* a quarter of the fixes wildly off */
    }
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);
    stn_result_t uncertain = answer(11.0, 0.0);
    uncertain.own.sd_lat_m = 3.0;
    uncertain.own.sd_lon_m = 3.0;   /* 11 m is under three times their 4.2 m deviation */
    queries(&w, 70, &uncertain);   /* the window passes through every mix of the two groups */
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_FALSE(w.s.apply_wanted);
}

TEST_CASE("automatic position: a sustained significant move queues a survey and the RTCM keeps flowing", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    query(&w, 120, &far);
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_PERIOD_MS, query(&w, 120, &far));
    queries(&w, FIRST_DECISION - 3, &far);
    TEST_ASSERT_FALSE(w.s.relocating);   /* one fix short of a decision (of the minimum span here) */
    TEST_ASSERT_EQUAL(FIRST_DECISION - 1, w.s.move_n);
    query(&w, 120, &far);
    TEST_ASSERT_TRUE(w.s.relocating);
    TEST_ASSERT_EQUAL(0, w.s.move_n);
    TEST_ASSERT_FALSE(stn_rtcm_held(&w.s));   /* confirming the move holds nothing */
    stn_fix_status_t measured;
    stn_fix_status(&w.s, w.in.now_ms, &measured);
    TEST_ASSERT_TRUE(measured.own_valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, far.own.lat_deg, measured.own.lat_deg);
    stn_job_t j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    TEST_ASSERT_TRUE(stn_rtcm_held(&w.s));   /* the apply owns the port: a short planned gap, as for any apply */
    stn_result_t failed = res(STN_E_FAIL);
    failed.nothing_sent = true;
    stn_done(&w.s, &failed, &w.in);
    TEST_ASSERT_TRUE(w.s.relocating);   /* a failed configuration keeps the relocation pending */
    TEST_ASSERT_FALSE(stn_rtcm_held(&w.s));
    w.s.apply_retry_ms = w.in.now_ms;
    j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);  /* retry must not just reapply the old reference */
    w.in.fixed_valid = false;
    w.in.profile_survey = true;
    stn_result_t ok = res(STN_OK);
    ok.mode_sent = true;
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_TRUE(w.s.relocating);
    TEST_ASSERT_FALSE(w.s.relocation_ready);
    TEST_ASSERT_EQUAL(STN_NEVER, w.s.pos_check_ms);
    TEST_ASSERT_FALSE(stn_rtcm_held(&w.s));   /* the survey streams */
    w.s.survey.complete = true;
    j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
    w.in.fixed_valid = true;
    w.in.profile_survey = false;
    w.in.fixed_llh[0] = far.own.lat_deg;
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_TRUE(w.s.relocation_ready);
    TEST_ASSERT_TRUE(w.s.relocating);
    TEST_ASSERT_FALSE(stn_rtcm_held(&w.s));   /* the new reference streams while its 1005 is checked */
    stn_result_t here = answer(12.0, 0.0);
    query(&w, 30, &here);
    TEST_ASSERT_TRUE(w.s.relocating);
    j = until_job(&w, 20);
    TEST_ASSERT_EQUAL(STN_JOB_CHECK_POSITION, j.kind);
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_FALSE(stn_rtcm_held(&w.s));
}

TEST_CASE("automatic position: a new reference starts the window afresh, so its own survey error cannot trigger at once", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(&w, 30, &far);
    TEST_ASSERT_EQUAL(30, w.s.move_n);
    w.in.fixed_llh[0] = FIX_LAT + 0.5 / M_PER_DEG_LAT;   /* the user types in, or a survey locks, a new position */
    stn_result_t off = answer(4.0, 0.0);   /* 3.5 m from it: beyond the limit */
    off.own.sd_lat_m = off.own.sd_lon_m = 0.5;
    queries(&w, FIRST_DECISION - 1, &off);
    TEST_ASSERT_FALSE(w.s.relocating);   /* not before a full window of fixes against the new reference */
    query(&w, 120, &off);
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: small persistent offsets never relocate even with optimistic sigma", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t bias = answer(2.5, 0.0);
    bias.own.sd_lat_m = bias.own.sd_lon_m = 0.05;
    queries(&w, 120, &bias);
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_FALSE(w.s.apply_wanted);
}

TEST_CASE("automatic position: scatter on both sides never relocates; a wandering window takes the majority only when it agrees", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t north = answer(12.0, 0.0), south = answer(-12.0, 0.0);
    for (int i = 0; i < 121; i++) {
        query(&w, 120, i % 2 ? &north : &south);   /* an odd count puts the per-axis median on one side */
    }
    TEST_ASSERT_FALSE(w.s.relocating);
    for (int i = 0; i < 20; i++) query(&w, 120, &north);
    TEST_ASSERT_FALSE(w.s.relocating);   /* under 75 % of the window agree yet */
    int i = 0;
    while (!w.s.relocating && i < 60) {
        query(&w, 120, &north);
        i++;
    }
    TEST_ASSERT_TRUE(w.s.relocating);   /* once three quarters of the window agree on the new place */
}

TEST_CASE("automatic position: lost and unusable fixes only leave the window short; a long gap empties it", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    stn_result_t lost = res(STN_E_TIMEOUT);
    stn_result_t no_fix = answer(12.0, 0.0);
    no_fix.own.computed = false;
    queries(&w, 10, &far);
    query(&w, 120, &lost);
    query(&w, 120, &no_fix);
    TEST_ASSERT_EQUAL(10, w.s.move_n);   /* neither reset the window */
    int n = 0;
    while (!w.s.relocating && n < 70) {
        query(&w, 120, n % 6 == 5 ? &lost : &far);   /* one fix in six lost */
        n++;
    }
    TEST_ASSERT_TRUE(w.s.relocating);
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    queries(&w, 30, &far);
    tick(&w, STN_MOVE_WINDOW_MS + 1000);
    query(&w, 120, &far);
    TEST_ASSERT_EQUAL(1, w.s.move_n);   /* everything older than the window is gone */
    queries(&w, FIRST_DECISION - 2, &far);
    TEST_ASSERT_FALSE(w.s.relocating);
}

TEST_CASE("automatic position: a receiver restart, manual setup and maintenance empty the window", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(&w, 20, &far);
    TEST_ASSERT_EQUAL(20, w.s.move_n);
    stn_restart_seen(&w.s);   /* the receiver restarts: its own solution starts afresh */
    stn_job_t j = until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    stn_result_t id = res(STN_OK);
    id.supported = true;
    id.static_ok = true;
    stn_done(&w.s, &id, &w.in);
    TEST_ASSERT_EQUAL(0, w.s.move_n);
    query(&w, 120, &far);
    TEST_ASSERT_EQUAL(1, w.s.move_n);
    queries(&w, 19, &far);
    w.in.autocfg = false;
    no_query(&w, 1);
    TEST_ASSERT_EQUAL(0, w.s.move_n);
    w.in.autocfg = true;
    queries(&w, 20, &far);
    TEST_ASSERT_EQUAL(20, w.s.move_n);
    TEST_ASSERT_EQUAL(STN_MAINT_OK, stn_maint_enter(&w.s, 900, false, w.in.now_ms));
    TEST_ASSERT_EQUAL(0, w.s.move_n);
}

TEST_CASE("automatic position: a 5 m move is found with a typical 1.2 m deviation, its own wander is not", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t wander_a = answer_en(1.2, -0.6, 0.0), wander_b = answer_en(-0.4, 1.1, 0.0);
    wander_a.own.sd_lat_m = wander_a.own.sd_lon_m = 0.85;   /* hypot 1.2 m: the limit is 3.6 m */
    wander_b.own.sd_lat_m = wander_b.own.sd_lon_m = 0.85;
    for (int i = 0; i < 90; i++) query(&w, 120, i % 3 ? &wander_a : &wander_b);
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);
    stn_result_t moved_a = answer_en(3.0, 3.6, 0.0), moved_b = answer_en(4.6, 2.5, 0.0);   /* about 5 m, scattered */
    moved_a.own.sd_lat_m = moved_a.own.sd_lon_m = 0.85;
    moved_b.own.sd_lat_m = moved_b.own.sd_lon_m = 0.85;
    int n = 0;
    while (!w.s.relocating && n < 70) {
        query(&w, 120, n % 2 ? &moved_a : &moved_b);
        n++;
    }
    TEST_ASSERT_TRUE(w.s.relocating);
    TEST_ASSERT_LESS_OR_EQUAL(STN_MOVE_WINDOW_MS / STN_FIX_PERIOD_MS, n);   /* within one window of the move */
}

TEST_CASE("automatic position: far fixes keep their spread: scattered ones never agree, a far move does", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t near_far = answer_en(400.0, 0.0, 0.0), far_far = answer_en(1000.0, 0.0, 0.0);
    near_far.own.sd_lat_m = near_far.own.sd_lon_m = 0.85;
    far_far.own.sd_lat_m = far_far.own.sd_lon_m = 0.85;
    for (int i = 0; i < 90; i++) query(&w, 120, i % 2 ? &near_far : &far_far);
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_MIN_SAMPLES, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);   /* 600 m apart: they agree on no place */
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t moved = answer_en(-2000.0, 1500.0, 0.0);   /* another site, 2.5 km away */
    queries(&w, FIRST_DECISION - 1, &moved);
    TEST_ASSERT_FALSE(w.s.relocating);
    query(&w, 120, &moved);
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: a move east is found like one north", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t east = answer_en(-9.0, 0.5, 0.0);
    queries(&w, FIRST_DECISION - 1, &east);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_INT_WITHIN(5, -900, w.s.move[0].east_cm);   /* stored signed, on its own axis */
    TEST_ASSERT_INT_WITHIN(5, 50, w.s.move[0].north_cm);
    query(&w, 120, &east);
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: the window's 16-bit seconds wrap without losing or keeping fixes", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.now_ms = (int64_t)65536 * 1000 * 70 - 5 * MIN_MS;   /* the seconds wrap five minutes in */
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(&w, FIRST_DECISION - 1, &far);
    TEST_ASSERT_EQUAL(FIRST_DECISION - 1, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);
    query(&w, 120, &far);
    TEST_ASSERT_TRUE(w.s.relocating);
    world_init(&w, true);
    w.in.now_ms = (int64_t)65536 * 1000 * 70 - 5 * MIN_MS;
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t near = answer(0.5, 0.0);
    queries(&w, 90, &near);   /* 30 min across the wrap: old fixes still leave on time */
    TEST_ASSERT_LESS_OR_EQUAL(STN_MOVE_WINDOW_MS / STN_FIX_PERIOD_MS, w.s.move_n);
    TEST_ASSERT_GREATER_OR_EQUAL(STN_MOVE_WINDOW_MS / STN_FIX_PERIOD_MS - 1, w.s.move_n);
}

TEST_CASE("automatic position: three quarters of the window must agree on the new place", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    int full = STN_MOVE_WINDOW_MS / STN_FIX_PERIOD_MS;   /* 60 fixes */
    int others = full - (full * STN_MOVE_AGREE_PCT) / 100 + 1;   /* 16: one too many */
    stn_result_t home = answer(0.8, 0.0), moved = answer(12.0, 0.0);
    queries(&w, others, &home);
    queries(&w, full - others, &moved);
    TEST_ASSERT_EQUAL(full, w.s.move_n);
    TEST_ASSERT_FALSE(w.s.relocating);   /* 44 of 60 agree: the median is there, the agreement is not */
    query(&w, 120, &moved);   /* the oldest home fix leaves: 45 of 60 */
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: an answer about a reference replaced meanwhile is not judged", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(&w, FIRST_DECISION - 1, &far);
    stn_job_t j = until_job(&w, 120);
    TEST_ASSERT_EQUAL(STN_JOB_OWN_FIX, j.kind);
    w.in.fixed_llh[0] = far.own.lat_deg;   /* the user locks a new position while the query is out */
    stn_done(&w.s, &far, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(0, w.s.move_n);
}

/* A confirmed move and its survey, up to the lock's apply job (handed out, not done). */
static void relocate_to_lock_job(world_t *w)
{
    world_init(w, true);
    w->in.autocfg = true;
    boot(w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(w, FIRST_DECISION, &far);
    TEST_ASSERT_TRUE(w->s.relocating);
    stn_job_t j = until_job(w, 2);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    w->in.fixed_valid = false;
    w->in.profile_survey = true;
    stn_result_t ok = res(STN_OK);
    ok.mode_sent = true;
    stn_done(&w->s, &ok, &w->in);
    w->s.survey.complete = true;
    j = until_job(w, 2);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
}

/* The lock's apply succeeds: the station stored the surveyed position as the fixed profile. */
static void lock_done(world_t *w)
{
    w->in.fixed_valid = true;
    w->in.profile_survey = false;
    w->in.fixed_llh[0] = FIX_LAT + 12.0 / M_PER_DEG_LAT;
    stn_result_t ok = res(STN_OK);
    stn_done(&w->s, &ok, &w->in);
}

/* A confirmed move, its survey and its lock: the new reference waits for its 1005 check. */
static void relocate_to_check(world_t *w)
{
    relocate_to_lock_job(w);
    lock_done(w);
    TEST_ASSERT_TRUE(w->s.relocation_ready);
}

/* secs of world time: position queries answered near, a position check answered with check_err
 * (and counted), every other job with a timeout. @return the position checks handed out. */
static int run_for(world_t *w, int secs, stn_err_t check_err)
{
    stn_result_t here = answer(12.0, 0.0);
    int checks = 0;
    for (int i = 0; i < secs; i++) {
        stn_job_t j = stn_next(&w->s, &w->in);
        if (j.kind == STN_JOB_OWN_FIX) {
            stn_done(&w->s, &here, &w->in);
        } else if (j.kind != STN_JOB_NONE) {
            stn_result_t r = res(j.kind == STN_JOB_CHECK_POSITION ? check_err : STN_E_TIMEOUT);
            checks += j.kind == STN_JOB_CHECK_POSITION;
            stn_done(&w->s, &r, &w->in);
        }
        tick(w, 1000);
    }
    return checks;
}

/* The next CHECK_POSITION job (position queries meanwhile answered near the new reference). */
static void check_job(world_t *w)
{
    stn_result_t here = answer(12.0, 0.0);
    for (int i = 0; i < 60; i++) {
        stn_job_t j = until_job(w, 60);
        if (j.kind == STN_JOB_CHECK_POSITION) return;
        TEST_ASSERT_EQUAL(STN_JOB_OWN_FIX, j.kind);
        stn_done(&w->s, &here, &w->in);
    }
    TEST_FAIL_MESSAGE("no position check");
}

TEST_CASE("automatic position: the new reference's check ends the relocation, matched, mismatched or never compared", "[station_fixcheck]")
{
    world_t w;
    relocate_to_check(&w);
    check_job(&w);
    stn_result_t r = res(STN_OK);
    stn_done(&w.s, &r, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_CHECKED, w.s.relocation_end);

    relocate_to_check(&w);
    check_job(&w);
    r = res(STN_E_MISMATCH);
    stn_done(&w.s, &r, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);   /* movement is watched again; a restart does not survey once more */
    TEST_ASSERT_EQUAL(STN_RELOC_END_MISMATCH, w.s.relocation_end);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_POSITION_MISMATCH) != 0);   /* the user still sees it */

    relocate_to_check(&w);
    r = res(STN_E_TIMEOUT);
    for (int i = 0; i < STN_POS_CHECK_TRIES - 1; i++) {
        check_job(&w);
        stn_done(&w.s, &r, &w.in);
        TEST_ASSERT_TRUE(w.s.relocating);
    }
    check_job(&w);
    stn_done(&w.s, &r, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_UNCHECKED, w.s.relocation_end);
}

TEST_CASE("automatic position: a receiver restart after the lock leaves only the 1005 check, never another survey", "[station_fixcheck]")
{
    world_t w;
    relocate_to_check(&w);
    stn_restart_seen(&w.s);
    stn_job_t j = until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_IDENTIFY, j.kind);
    stn_result_t id = res(STN_OK);
    id.supported = true;
    id.static_ok = true;
    stn_done(&w.s, &id, &w.in);
    TEST_ASSERT_FALSE(w.s.apply_wanted);   /* the new reference is already the receiver's */
    TEST_ASSERT_TRUE(w.s.relocation_ready);
    check_job(&w);   /* fails on any apply in between */
    stn_result_t ok = res(STN_OK);
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_CHECKED, w.s.relocation_end);
}

TEST_CASE("automatic position: a survey the user starts after the lock keeps the relocation pending, even when its apply fails", "[station_fixcheck]")
{
    world_t w;
    relocate_to_check(&w);
    stn_request_resurvey(&w.s);
    stn_job_t j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    w.in.fixed_valid = false;   /* the survey profile is stored, then the lease is refused */
    w.in.profile_survey = true;
    stn_result_t failed = res(STN_E_FAIL);
    failed.nothing_sent = true;
    stn_done(&w.s, &failed, &w.in);
    run_for(&w, 600, STN_E_TIMEOUT);   /* ten minutes: the old reference's checks run out */
    TEST_ASSERT_TRUE(w.s.relocating);   /* no reference of the relocation's is the receiver's now */
    w.in.fixed_valid = true;   /* the user types the position back and applies it */
    w.in.profile_survey = false;
    stn_profile_changed(&w.s);
    stn_request_apply(&w.s);
    j = until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    stn_result_t ok = res(STN_OK);
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_TRUE(w.s.relocation_ready);
    run_for(&w, 60, STN_OK);   /* its 1005 is compared */
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_CHECKED, w.s.relocation_end);
}

TEST_CASE("automatic position: a survey requested while the lock applies keeps the relocation pending when it fails", "[station_fixcheck]")
{
    world_t w;
    relocate_to_lock_job(&w);
    stn_request_resurvey(&w.s);   /* the user, while the lock's apply runs */
    lock_done(&w);
    TEST_ASSERT_TRUE(w.s.relocation_ready);
    stn_job_t j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    w.in.fixed_valid = false;
    w.in.profile_survey = true;
    stn_result_t failed = res(STN_E_FAIL);
    failed.nothing_sent = true;
    stn_done(&w.s, &failed, &w.in);
    run_for(&w, 600, STN_E_TIMEOUT);
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: a failed survey request leaves an ordinary lock's 1005 check in place", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, false);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t mode = res(STN_OK);
    mode.mode_survey = true;
    stn_job_t j = until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    stn_done(&w.s, &mode, &w.in);
    w.s.survey.complete = true;
    j = until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_LOCK, j.change);
    w.in.fixed_valid = true;
    w.in.profile_survey = false;
    stn_result_t ok = res(STN_OK);
    stn_done(&w.s, &ok, &w.in);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_NOT_EQUAL(STN_NEVER, w.s.pos_check_ms);
    stn_request_resurvey(&w.s);
    j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    stn_result_t failed = res(STN_E_FAIL);   /* nothing stored: the fixed profile stays */
    failed.nothing_sent = true;
    stn_done(&w.s, &failed, &w.in);
    TEST_ASSERT_GREATER_THAN(0, run_for(&w, 60, STN_E_MISMATCH));   /* its check still runs */
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_POSITION_MISMATCH) != 0);
}

/* A confirmed move whose survey apply stores the survey profile and then fails. */
static void relocate_survey_failed(world_t *w)
{
    world_init(w, true);
    w->in.autocfg = true;
    boot(w, true);
    stn_result_t far = answer(12.0, 0.0);
    queries(w, FIRST_DECISION, &far);
    TEST_ASSERT_TRUE(w->s.relocating);
    stn_job_t j = until_job(w, 2);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    w->in.fixed_valid = false;
    w->in.profile_survey = true;
    stn_result_t failed = res(STN_E_FAIL);
    failed.nothing_sent = true;
    stn_done(&w->s, &failed, &w->in);
    TEST_ASSERT_TRUE(w->s.relocating);
    TEST_ASSERT_FALSE(w->s.relocation_ready);
}

/* The user saves a fixed position (lat_deg) and its apply succeeds. */
static void user_fixed_applied(world_t *w, double lat_deg)
{
    w->in.fixed_valid = true;
    w->in.profile_survey = false;
    w->in.fixed_llh[0] = lat_deg;
    stn_profile_changed(&w->s);   /* saved, then applied (station_request_reconfigure) */
    stn_request_apply(&w->s);
    stn_job_t j = until_job(w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    stn_result_t ok = res(STN_OK);
    stn_done(&w->s, &ok, &w->in);
}

TEST_CASE("automatic position: a position the user sets after a failed survey becomes the relocation's reference", "[station_fixcheck]")
{
    world_t w;
    relocate_survey_failed(&w);
    user_fixed_applied(&w, FIX_LAT + 12.0 / M_PER_DEG_LAT);
    TEST_ASSERT_TRUE(w.s.relocation_ready);
    run_for(&w, 60, STN_OK);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_CHECKED, w.s.relocation_end);
}

TEST_CASE("automatic position: the old position put back still owes a new survey", "[station_fixcheck]")
{
    world_t w;
    relocate_survey_failed(&w);
    user_fixed_applied(&w, FIX_LAT);   /* the reference the move was found against */
    TEST_ASSERT_FALSE(w.s.relocation_ready);
    TEST_ASSERT_TRUE(w.s.relocating);
    stn_job_t j = until_job(&w, 30);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
}

TEST_CASE("automatic position: turning automatic setup off drops a relocation", "[station_fixcheck]")
{
    world_t w;
    relocate_survey_failed(&w);
    w.in.autocfg = false;
    no_query(&w, 1);
    TEST_ASSERT_FALSE(w.s.relocating);
    TEST_ASSERT_EQUAL(STN_RELOC_END_MANUAL, w.s.relocation_end);
}

TEST_CASE("automatic position: a reset resumes an unfinished relocation", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    w.in.autocfg = true;
    w.s.relocating = true;   /* restored from the persisted relocation flag */
    boot(&w, true);
    stn_job_t j = until_job(&w, 2);
    TEST_ASSERT_EQUAL(STN_JOB_APPLY, j.kind);
    TEST_ASSERT_EQUAL(STN_CHANGE_RESURVEY, j.change);
    TEST_ASSERT_TRUE(w.s.relocating);
}

TEST_CASE("automatic position: GNSS monitoring continues while surveying and stale fixes are not live", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, false);
    w.in.autocfg = true;
    boot(&w, true);
    stn_result_t mode = res(STN_OK);
    mode.mode_survey = true;
    stn_job_t j = until_job(&w, 5);
    TEST_ASSERT_EQUAL(STN_JOB_MODE, j.kind);
    stn_done(&w.s, &mode, &w.in);
    stn_result_t point = answer(1.0, 0.0);
    query(&w, 120, &point);
    stn_fix_status_t status;
    stn_fix_status(&w.s, w.in.now_ms, &status);
    TEST_ASSERT_TRUE(status.own_valid);
    TEST_ASSERT_EQUAL(STN_FIX_OFF, status.state);
    stn_fix_status(&w.s, w.in.now_ms + 91000, &status);
    TEST_ASSERT_FALSE(status.own_valid);
    TEST_ASSERT_FALSE(w.s.relocating);
}

TEST_CASE("automatic position: enabling automatic setup does not wait for the manual hourly recheck", "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t near = answer(0.0, 0.0);
    for (int i = 0; i < 3; i++) query(&w, 120, &near);
    TEST_ASSERT_INT64_WITHIN(1000, w.in.now_ms + STN_FIX_RECHECK_MS, w.s.fix_due_ms);
    w.in.autocfg = true;
    TEST_ASSERT_INT64_WITHIN(1500, STN_FIX_PERIOD_MS, query(&w, 40, &near));
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

TEST_CASE("station fixcheck: a fixed height typed as height above sea level, 34 m off, raises the warning with its own note",
          "[station_fixcheck]")
{
    /* The receiver where the fixed position says; the fixed height is its height above sea level (the geoid 34 m below
     * the ellipsoid there), not its ellipsoidal height: inside the 50 m limit. */
    world_t w;
    world_init(&w, true);
    w.in.fixed_llh[2] = FIX_H + 34.0;
    boot(&w, true);
    stn_result_t here = answer(0.0, 0.0);   /* at FIX_H: 34 m below the fixed height, its sea-level height 134 m */
    for (int i = 0; i < 2; i++) {
        query(&w, 120, &here);
    }
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    query(&w, 120, &here);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL(STN_STREAMING, w.s.state);   /* streaming goes on */
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, st.state);
    TEST_ASSERT_TRUE(st.sea_level);
    TEST_ASSERT_TRUE(st.height_only);   /* the warning names the height */
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 34.0, st.offset.up_m);
    TEST_ASSERT_DOUBLE_WITHIN(0.1, 0.0, st.offset.up_msl_m);
    stn_note_t n[STN_NOTES];
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_SEA_LEVEL, n[0].kind);
    TEST_ASSERT_EQUAL_INT32(0, n[0].a);
    TEST_ASSERT_EQUAL_INT32(34, n[0].b);

    /* An hour later the same: nothing new to note. */
    query(&w, 2 * 3600, &here);
    query(&w, 120, &here);
    query(&w, 120, &here);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));

    /* The user enters the height above the ellipsoid: dropped at once, then checked near. */
    w.in.fixed_llh[2] = FIX_H;
    (void)stn_next(&w.s, &w.in);
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_CHANGED, n[0].kind);
    stn_result_t at = answer(0.0, 0.0);
    for (int i = 0; i < 3; i++) {
        query(&w, 120, &at);
    }
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NEAR, st.state);
    TEST_ASSERT_FALSE(st.sea_level);
    TEST_ASSERT_EQUAL_UINT32(0, stn_alerts(&w.s) & STN_ALERT_FIXED_FAR);
}

TEST_CASE("station fixcheck: the right height 34 m from the sea-level one raises nothing; a reason that changes is noted",
          "[station_fixcheck]")
{
    world_t w;
    world_init(&w, true);
    boot(&w, true);
    stn_result_t here = answer(0.0, 0.0);
    for (int i = 0; i < 3; i++) {
        query(&w, 120, &here);
    }
    stn_fix_status_t st;
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_NEAR, st.state);
    TEST_ASSERT_FALSE(st.sea_level);
    stn_note_t n[STN_NOTES];
    TEST_ASSERT_EQUAL_UINT(0, notes(&w, n));

    /* A sea-level height, then (an hour later) the receiver's own solution 120 m away: the reason changes, noted. */
    world_init(&w, true);
    w.in.fixed_llh[2] = FIX_H + 34.0;
    boot(&w, true);
    stn_result_t sea = answer(0.0, 0.0);
    for (int i = 0; i < 3; i++) {
        query(&w, 120, &sea);
    }
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_SEA_LEVEL, n[0].kind);
    stn_result_t away = answer(120.0, 0.0);
    query(&w, 2 * 3600, &away);
    query(&w, 120, &away);
    query(&w, 120, &away);
    stn_fix_status(&w.s, w.in.now_ms, &st);
    TEST_ASSERT_EQUAL_UINT8(STN_FIX_FAR, st.state);
    TEST_ASSERT_FALSE(st.sea_level);
    TEST_ASSERT_TRUE((stn_alerts(&w.s) & STN_ALERT_FIXED_FAR) != 0);
    TEST_ASSERT_EQUAL_UINT(1, notes(&w, n));
    TEST_ASSERT_EQUAL_UINT8(STN_NOTE_FIXED_FAR, n[0].kind);
    TEST_ASSERT_EQUAL_INT32(120, n[0].a);
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
