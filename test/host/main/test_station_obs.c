/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observed-set tracker: does the receiver send what the profile asks for?
 */
#include <stdint.h>
#include <string.h>

#include "gnss_rtcm3.h"
#include "station_obs.h"
#include "unity.h"

/* Clock near the 32-bit millisecond wrap, to show nothing depends on it. */
#define T0 ((int64_t)UINT32_MAX - 2000)

/* The Onocoy set at 1 s, QZSS included. */
static const um98x_rtcm_out_t k_set[] = {
    { 1005, 1 }, { 1033, 1 }, { 1077, 1 }, { 1087, 1 }, { 1097, 1 }, { 1117, 1 }, { 1127, 1 }, { 1230, 1 },
};
#define K_SET_N ((uint8_t)(sizeof(k_set) / sizeof(k_set[0])))

static const stn_obs_opts_t k_opts = { .rtcm1230_ok = true, .surveying = false };

/* One frame at t; an MSM carries the epoch time t (each output second its own epoch). */
static void feed(stn_obs_t *o, uint16_t msg, int64_t t)
{
    stn_obs_feed(o, msg, gnss_rtcm3_msm_type(msg, NULL, NULL), (uint32_t)(t - T0), t);
}

/* Feed one second of output starting at t: every listed message once, except `skip`. */
static void feed_second(stn_obs_t *o, int64_t t, const uint16_t *msgs, unsigned n, uint16_t skip)
{
    for (unsigned i = 0; i < n; i++) {
        if (msgs[i] != skip) {
            feed(o, msgs[i], t + 10 * (int64_t)i);
        }
    }
}

static const uint16_t k_all[] = { 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1230 };
static const uint16_t k_no_qzss[] = { 1005, 1033, 1077, 1087, 1097, 1127, 1230 };

/* Feed `seconds` of output from `start` (one per second), return the end time. */
static int64_t run(stn_obs_t *o, int64_t start, int seconds, const uint16_t *msgs, unsigned n, uint16_t skip)
{
    for (int s = 0; s < seconds; s++) {
        feed_second(o, start + (int64_t)s * 1000, msgs, n, skip);
    }
    return start + (int64_t)seconds * 1000;
}

TEST_CASE("observed set: unarmed is pending, silence ends as no RTCM after the window", "[station]")
{
    stn_obs_t o;
    stn_obs_clear(&o);
    TEST_ASSERT_EQUAL(STN_OBS_PENDING, stn_obs_eval(&o, &k_opts, T0, NULL));
    feed(&o, 1077, T0);   /* ignored while not armed */
    TEST_ASSERT_EQUAL_UINT32(0, o.frames);

    stn_obs_arm(&o, k_set, K_SET_N, T0);
    TEST_ASSERT_EQUAL_INT64(STN_OBS_WINDOW_MS, stn_obs_window_ms(&o));
    TEST_ASSERT_EQUAL(STN_OBS_PENDING, stn_obs_eval(&o, &k_opts, T0 + STN_OBS_WINDOW_MS - 1, NULL));
    TEST_ASSERT_EQUAL(STN_OBS_NO_RTCM, stn_obs_eval(&o, &k_opts, T0 + STN_OBS_WINDOW_MS, NULL));
}

TEST_CASE("observed set: the full set at its rates passes; the window starts at the first frame", "[station]")
{
    stn_obs_t o;
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    /* Output resumes 20 s after arming (a configuration session's restart). */
    int64_t start = T0 + 20000;
    int64_t end = run(&o, start, 29, k_all, 8, 0);
    TEST_ASSERT_EQUAL(STN_OBS_PENDING, stn_obs_eval(&o, &k_opts, end, NULL));
    end = run(&o, end, 1, k_all, 8, 0);
    uint16_t bad = 1;
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, start + STN_OBS_WINDOW_MS, &bad));
    TEST_ASSERT_EQUAL_UINT16(0, bad);
    /* A late evaluation (a job held the station task) does not read as "too fast". */
    end = run(&o, end, 20, k_all, 8, 0);
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, end, NULL));
}

TEST_CASE("observed set: a missing or slow global MSM is drift", "[station]")
{
    stn_obs_t o;
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    int64_t end = run(&o, T0, 30, k_all, 8, 1097);
    uint16_t bad = 0;
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, end, &bad));
    TEST_ASSERT_EQUAL_UINT16(1097, bad);

    /* GPS MSM7 every 5 s instead of every second. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        feed_second(&o, T0 + s * 1000, k_all, 8, (s % 5 == 0) ? 0 : 1077);
    }
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, T0 + 30000, &bad));
    TEST_ASSERT_EQUAL_UINT16(1077, bad);

    /* At half the rate it still passes (tolerance for lost epochs). */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        feed_second(&o, T0 + s * 1000, k_all, 8, (s % 2 == 0) ? 0 : 1077);
    }
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, T0 + 30000, NULL));
}

TEST_CASE("observed set: an MSM faster than its period is drift", "[station]")
{
    stn_obs_t o;
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        feed_second(&o, T0 + s * 1000, k_all, 8, 0);
        feed(&o, 1127, T0 + s * 1000 + 500);   /* BeiDou at 2 Hz: a second epoch every second */
    }
    uint16_t bad = 0;
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, T0 + 30000, &bad));
    TEST_ASSERT_EQUAL_UINT16(1127, bad);
}

TEST_CASE("observed set: a constellation split over several MSMs per epoch is one epoch", "[station]")
{
    /* About 25 BeiDou satellites on 3 signals (75 cells) do not fit one MSM
     * (64 cells): the receiver sends two 1127 frames with the same epoch
     * time every second. That is 1 Hz, not 2 Hz. */
    stn_obs_t o;
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        int64_t t = T0 + s * 1000;
        feed_second(&o, t, k_all, 8, 0);   /* 1127 at t + 60 with epoch time t + 60 */
        stn_obs_feed(&o, 1127, true, (uint32_t)(t + 60 - T0), t + 65);
    }
    uint16_t bad = 1;
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, T0 + 30000, &bad));
    TEST_ASSERT_EQUAL_UINT16(0, bad);
    TEST_ASSERT_EQUAL_UINT32(30, o.seen[6]);   /* 1127: 30 epochs */

    /* Frames without an epoch time (header not decoded) still count one by one. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        int64_t t = T0 + s * 1000;
        feed_second(&o, t, k_all, 8, 1127);
        stn_obs_feed(&o, 1127, false, 0, t + 60);
        stn_obs_feed(&o, 1127, false, 0, t + 65);
    }
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, T0 + 30000, &bad));
    TEST_ASSERT_EQUAL_UINT16(1127, bad);
}

TEST_CASE("observed set: an absent regional MSM is not drift, a too fast one is", "[station]")
{
    stn_obs_t o;
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    int64_t end = run(&o, T0, 30, k_no_qzss, 7, 0);   /* no QZSS satellite in view */
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, end, NULL));

    /* QZSS at the horizon for a few seconds only: still fine. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    end = run(&o, T0, 30, k_no_qzss, 7, 0);
    feed(&o, 1117, T0 + 3000);
    feed(&o, 1117, T0 + 4000);
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, end, NULL));

    /* QZSS at 2 Hz. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    for (int s = 0; s < 30; s++) {
        feed_second(&o, T0 + s * 1000, k_all, 8, 0);
        feed(&o, 1117, T0 + s * 1000 + 500);
    }
    uint16_t bad = 0;
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, T0 + 30000, &bad));
    TEST_ASSERT_EQUAL_UINT16(1117, bad);
}

TEST_CASE("observed set: station messages, the survey window and the 1230 result", "[station]")
{
    stn_obs_t o;
    uint16_t bad = 0;
    /* 1005 missing (the window starts 10 ms later, at the first 1033). */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    int64_t end = run(&o, T0, 30, k_all, 8, 1005) + 10;
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, end, &bad));
    TEST_ASSERT_EQUAL_UINT16(1005, bad);
    /* ... is fine while the receiver surveys. */
    stn_obs_opts_t surveying = { .rtcm1230_ok = true, .surveying = true };
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &surveying, end, NULL));

    /* 1033 seen only at the start of the window: within 30 s still counts. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    feed_second(&o, T0, k_all, 8, 0);
    end = run(&o, T0 + 1000, 29, k_all, 8, 1033);
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, end, NULL));
    /* ... but not once it is older than 30 s. */
    end = run(&o, end, 5, k_all, 8, 1033);
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, end, &bad));
    TEST_ASSERT_EQUAL_UINT16(1033, bad);

    /* 1230 is required only when the receiver accepted it. */
    stn_obs_arm(&o, k_set, K_SET_N, T0);
    end = run(&o, T0, 30, k_all, 8, 1230);
    TEST_ASSERT_EQUAL(STN_OBS_DRIFT, stn_obs_eval(&o, &k_opts, end, &bad));
    TEST_ASSERT_EQUAL_UINT16(1230, bad);
    stn_obs_opts_t no1230 = { .rtcm1230_ok = false, .surveying = false };
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &no1230, end, NULL));
}

TEST_CASE("observed set: slow station messages stretch the window; unchecked types are ignored", "[station]")
{
    static const um98x_rtcm_out_t set[] = { { 1077, 1 }, { 1033, 30 }, { 1019, 120 } };
    stn_obs_t o;
    stn_obs_arm(&o, set, 3, T0);
    /* 1033 every 30 s: the window is 60 s; 1019 (ephemeris) is not checked. */
    TEST_ASSERT_EQUAL_INT64(60000, stn_obs_window_ms(&o));
    for (int s = 0; s < 60; s++) {
        feed(&o, 1077, T0 + s * 1000);
        if (s % 30 == 0) {
            feed(&o, 1033, T0 + s * 1000 + 100);
        }
    }
    TEST_ASSERT_EQUAL(STN_OBS_PENDING, stn_obs_eval(&o, &k_opts, T0 + 59999, NULL));
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, T0 + 60000, NULL));
    /* Extra messages the profile does not list are not drift. */
    feed(&o, 1074, T0 + 60100);
    TEST_ASSERT_EQUAL(STN_OBS_PASS, stn_obs_eval(&o, &k_opts, T0 + 60200, NULL));
}
