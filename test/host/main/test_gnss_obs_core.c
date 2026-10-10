/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary core: bands per constellation over the 10 s window,
 * best constellation, Galileo E6 loss, satellites over a split epoch,
 * epoch counting, and the published 1074 frame end to end.
 */
#include <string.h>

#include "gnss_obs_core.h"
#include "gnss_rtcm3.h"
#include "rtcm_vectors.h"
#include "unity.h"

static uint32_t sigs(const uint8_t *ids, size_t n)
{
    uint32_t m = 0;
    for (size_t i = 0; i < n; i++) {
        m |= (uint32_t)1 << (ids[i] - 1);
    }
    return m;
}

static gnss_rtcm3_msm_t msm(gnss_sys_t sys, uint32_t epoch, bool more, uint64_t sat_mask, uint32_t sig_mask)
{
    gnss_rtcm3_msm_t h;
    memset(&h, 0, sizeof(h));
    h.msg_type = 1077;
    h.sys = sys;
    h.msm = 7;
    h.epoch_time = epoch;
    h.multiple_message = more;
    h.sat_mask = sat_mask;
    h.sig_mask = sig_mask;
    return h;
}

static obs_core_t s_core;

TEST_CASE("obs: Galileo E1/E5a/E5b/E6 is four bands and the best constellation", "[gnss_obs]")
{
    static const uint8_t gal[] = { 2, 8, 14, 22 };          /* E1, E6, E5b, E5a */
    static const uint8_t gps[] = { 2, 10, 22 };              /* L1, L2, L5 */
    static const uint8_t glo[] = { 2, 8 };                   /* G1, G2 */
    obs_core_reset(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, true, 0xFF, sigs(gps, 3));
    obs_core_feed_msm(&s_core, &h, 100);
    h = msm(GNSS_SYS_GLONASS, 2000, true, 0x0F, sigs(glo, 2));
    obs_core_feed_msm(&s_core, &h, 100);
    h = msm(GNSS_SYS_GALILEO, 1000, false, 0x3F, sigs(gal, 4));
    obs_core_feed_msm(&s_core, &h, 100);

    obs_core_view_t v;
    obs_core_view(&s_core, 200, &v);
    TEST_ASSERT_TRUE(v.sys[GNSS_SYS_GPS].present);
    TEST_ASSERT_EQUAL_UINT8(8, v.sys[GNSS_SYS_GPS].satellites);
    TEST_ASSERT_EQUAL_UINT8(3, v.sys[GNSS_SYS_GPS].bands);
    TEST_ASSERT_EQUAL_UINT8(2, v.sys[GNSS_SYS_GLONASS].bands);
    TEST_ASSERT_EQUAL_UINT8(4, v.sys[GNSS_SYS_GALILEO].bands);
    TEST_ASSERT_EQUAL_HEX8(0x0F, v.sys[GNSS_SYS_GALILEO].band_mask);
    TEST_ASSERT_EQUAL_HEX32(sigs(gal, 4), v.sys[GNSS_SYS_GALILEO].signal_mask);
    TEST_ASSERT_EQUAL_UINT8(6, v.sys[GNSS_SYS_GALILEO].satellites);
    TEST_ASSERT_FALSE(v.sys[GNSS_SYS_BEIDOU].present);
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(4, v.best_bands);
    TEST_ASSERT_FALSE(v.e6_lost);
}

TEST_CASE("obs: nothing fed means no bands and no epoch", "[gnss_obs]")
{
    obs_core_reset(&s_core);
    obs_core_view_t v;
    obs_core_view(&s_core, 0, &v);
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        TEST_ASSERT_FALSE(v.sys[s].present);
    }
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(0, v.best_bands);
    TEST_ASSERT_EQUAL_UINT64(0, v.epochs);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.epoch_age_ms);
}

TEST_CASE("obs: a band drops out only after the 10 s window", "[gnss_obs]")
{
    static const uint8_t three[] = { 2, 10, 22 };
    static const uint8_t two[] = { 2, 10 };
    obs_core_reset(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, false, 1, sigs(three, 3));
    obs_core_feed_msm(&s_core, &h, 0);                        /* t = 0 is a valid time */
    h = msm(GNSS_SYS_GPS, 6000, false, 1, sigs(two, 2));
    obs_core_feed_msm(&s_core, &h, 5000);

    obs_core_view_t v;
    obs_core_view(&s_core, OBS_SIGNAL_WINDOW_MS - 1, &v);
    TEST_ASSERT_EQUAL_UINT8(3, v.sys[GNSS_SYS_GPS].bands);    /* one missing epoch changes nothing */
    obs_core_view(&s_core, OBS_SIGNAL_WINDOW_MS, &v);
    TEST_ASSERT_EQUAL_UINT8(2, v.sys[GNSS_SYS_GPS].bands);
    TEST_ASSERT_EQUAL_HEX32(sigs(two, 2), v.sys[GNSS_SYS_GPS].signal_mask);
    obs_core_view(&s_core, 5000 + OBS_SIGNAL_WINDOW_MS, &v);
    TEST_ASSERT_FALSE(v.sys[GNSS_SYS_GPS].present);           /* constellation gone */
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GPS].bands);
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GPS].satellites);
    TEST_ASSERT_EQUAL_UINT8(0, v.best_bands);

    /* A clock older than the stamps (should not happen) counts as "now". */
    obs_core_view(&s_core, 0, &v);
    TEST_ASSERT_EQUAL_UINT8(3, v.sys[GNSS_SYS_GPS].bands);
}

TEST_CASE("obs: Galileo E6 is reported lost only after it was seen", "[gnss_obs]")
{
    static const uint8_t no_e6[] = { 2, 14, 22 };
    static const uint8_t with_e6[] = { 2, 8, 14, 22 };
    obs_core_view_t v;
    obs_core_reset(&s_core);

    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 1, false, 1, sigs(no_e6, 3));
    obs_core_feed_msm(&s_core, &h, 1000);
    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);                             /* never seen: not expected */

    h = msm(GNSS_SYS_GALILEO, 2, false, 1, sigs(with_e6, 4));
    obs_core_feed_msm(&s_core, &h, 2000);
    obs_core_view(&s_core, 2000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
    TEST_ASSERT_EQUAL_UINT8(4, v.best_bands);

    for (uint64_t t = 3000; t <= 13000; t += 1000) {          /* E6 stops */
        h = msm(GNSS_SYS_GALILEO, (uint32_t)t, false, 1, sigs(no_e6, 3));
        obs_core_feed_msm(&s_core, &h, t);
    }
    obs_core_view(&s_core, 11999, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
    obs_core_view(&s_core, 12000, &v);
    TEST_ASSERT_TRUE(v.e6_lost);
    TEST_ASSERT_EQUAL_UINT8(3, v.best_bands);

    obs_core_reset(&s_core);                                  /* new signal group: forget */
    h = msm(GNSS_SYS_GALILEO, 1, false, 1, sigs(no_e6, 3));
    obs_core_feed_msm(&s_core, &h, 20000);
    obs_core_view(&s_core, 20000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
}

TEST_CASE("obs: satellites are counted over a split epoch", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_reset(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_BEIDOU, 500, true, 0x000FFFFFull, 1u << 1);       /* 20 satellites */
    obs_core_feed_msm(&s_core, &h, 10);
    h = msm(GNSS_SYS_BEIDOU, 500, false, 0x7FFF00000ull, 1u << 1);                       /* 15 more */
    obs_core_feed_msm(&s_core, &h, 11);
    obs_core_view(&s_core, 20, &v);
    TEST_ASSERT_EQUAL_UINT8(35, v.sys[GNSS_SYS_BEIDOU].satellites);

    h = msm(GNSS_SYS_BEIDOU, 1500, false, 0x1Full, 1u << 1);                             /* next epoch */
    obs_core_feed_msm(&s_core, &h, 1010);
    obs_core_view(&s_core, 1020, &v);
    TEST_ASSERT_EQUAL_UINT8(5, v.sys[GNSS_SYS_BEIDOU].satellites);
}

TEST_CASE("obs: epochs close on the multiple-message bit", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_reset(&s_core);
    for (uint32_t e = 0; e < 3; e++) {
        uint64_t t = 1000u * e;
        gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, e, true, 1, 2);
        obs_core_feed_msm(&s_core, &h, t);
        h = msm(GNSS_SYS_GALILEO, e, true, 1, 2);
        obs_core_feed_msm(&s_core, &h, t + 5);
        h = msm(GNSS_SYS_BEIDOU, e, false, 1, 2);
        obs_core_feed_msm(&s_core, &h, t + 9);
    }
    obs_core_view(&s_core, 2009, &v);
    TEST_ASSERT_EQUAL_UINT64(3, v.epochs);
    TEST_ASSERT_EQUAL_UINT32(0, v.epoch_age_ms);
    obs_core_view(&s_core, 2509, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.epoch_age_ms);
    obs_core_view(&s_core, 2009 + 0x200000000ull, &v);       /* age far beyond 32 bits */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1u, v.epoch_age_ms);
}

TEST_CASE("obs: the published 1074 frame end to end", "[gnss_obs]")
{
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(k_v1074, sizeof(k_v1074), &h));
    obs_core_reset(&s_core);
    obs_core_feed_msm(&s_core, &h, 42);
    obs_core_view_t v;
    obs_core_view(&s_core, 50, &v);
    TEST_ASSERT_TRUE(v.sys[GNSS_SYS_GPS].present);
    TEST_ASSERT_EQUAL_UINT8(8, v.sys[GNSS_SYS_GPS].satellites);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 9), v.sys[GNSS_SYS_GPS].signal_mask);
    TEST_ASSERT_EQUAL_UINT8(2, v.sys[GNSS_SYS_GPS].bands);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, v.best_sys);
    TEST_ASSERT_EQUAL_UINT64(0, v.epochs);                    /* its multiple-message bit is set */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.epoch_age_ms);
}
