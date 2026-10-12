/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary core: bands per constellation over the 10 s window,
 * best constellation, Galileo E6 loss, satellites over a split epoch,
 * epoch counting, per-band CNR, epoch gaps against the configured period,
 * message statistics, the sky view, and frames end to end (the published
 * 1074 and MSM7 frames built per the RTCM layout).
 */
#include <string.h>

#include "gnss_obs_core.h"
#include "gnss_obs_parse.h"
#include "gnss_rtcm3.h"
#include "msm_test_builder.h"
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
    /* Every satellite carries every signal (as decoded from a frame). */
    for (unsigned i = 0; i < 64; i++) {
        h.num_sats = (uint8_t)(h.num_sats + ((sat_mask >> i) & 1u));
    }
    for (unsigned i = 0; i < 32; i++) {
        h.num_sigs = (uint8_t)(h.num_sigs + ((sig_mask >> i) & 1u));
    }
    unsigned grid = (unsigned)h.num_sats * h.num_sigs;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(GNSS_RTCM3_MSM_MAX_CELLS, grid);
    h.cell_mask = grid >= 64 ? UINT64_MAX : (((uint64_t)1 << grid) - 1u);
    h.num_cells = (uint8_t)grid;
    return h;
}

static obs_core_t s_core;

TEST_CASE("obs: Galileo E1/E5a/E5b/E6 is four bands and the best constellation", "[gnss_obs]")
{
    static const uint8_t gal[] = { 2, 8, 14, 22 };          /* E1, E6, E5b, E5a */
    static const uint8_t gps[] = { 2, 10, 22 };              /* L1, L2, L5 */
    static const uint8_t glo[] = { 2, 8 };                   /* G1, G2 */
    obs_core_init(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, true, 0xFF, sigs(gps, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    h = msm(GNSS_SYS_GLONASS, 2000, true, 0x0F, sigs(glo, 2));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    h = msm(GNSS_SYS_GALILEO, 1000, false, 0x3F, sigs(gal, 4));
    obs_core_feed_msm(&s_core, &h, NULL, 100);

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

TEST_CASE("obs: the best constellation counts only GPS, GLONASS, Galileo and BeiDou; ties keep message order",
          "[gnss_obs]")
{
    static const uint8_t qzs[] = { 2, 9, 15, 22 };          /* L1, L6, L2, L5: four bands */
    static const uint8_t gal[] = { 2, 14, 22 };             /* E1, E5b, E5a: three */
    static const uint8_t irn[] = { 22 };                    /* NavIC L5 */
    obs_core_init(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 1000, true, 0x3F, sigs(gal, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    h = msm(GNSS_SYS_QZSS, 1000, true, 0x03, sigs(qzs, 4));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    h = msm(GNSS_SYS_NAVIC, 1000, false, 0x07, sigs(irn, 1));
    obs_core_feed_msm(&s_core, &h, NULL, 100);

    obs_core_view_t v;
    obs_core_view(&s_core, 200, &v);
    TEST_ASSERT_EQUAL_UINT8(4, v.sys[GNSS_SYS_QZSS].bands);
    TEST_ASSERT_TRUE(v.sys[GNSS_SYS_NAVIC].present);
    TEST_ASSERT_EQUAL_HEX8(1u << GNSS_BAND_L5, v.sys[GNSS_SYS_NAVIC].band_mask);
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(3, v.best_bands);

    /* A tie (GPS and BeiDou with three bands each) keeps the first in
     * message order: GPS. */
    static const uint8_t gps[] = { 2, 10, 22 };
    static const uint8_t bds[] = { 2, 8, 14 };
    obs_core_init(&s_core);
    h = msm(GNSS_SYS_BEIDOU, 1000, true, 0x0F, sigs(bds, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    h = msm(GNSS_SYS_GPS, 1000, false, 0x0F, sigs(gps, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    obs_core_view(&s_core, 200, &v);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(3, v.best_bands);

    /* Only unrewarded constellations: no best one. */
    obs_core_init(&s_core);
    h = msm(GNSS_SYS_QZSS, 1000, false, 0x03, sigs(qzs, 4));
    obs_core_feed_msm(&s_core, &h, NULL, 100);
    obs_core_view(&s_core, 200, &v);
    TEST_ASSERT_TRUE(v.sys[GNSS_SYS_QZSS].present);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(0, v.best_bands);
}

TEST_CASE("obs: GPS time of the latest GPS epoch, for the ledger's clock", "[gnss_obs]")
{
    obs_core_init(&s_core);
    uint32_t tow = 0, age = 0;
    TEST_ASSERT_FALSE(obs_core_gps_time(&s_core, 1000, &tow, &age));
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 345000, false, 0x3, 0x2);
    obs_core_feed_msm(&s_core, &h, NULL, 1000);
    TEST_ASSERT_FALSE(obs_core_gps_time(&s_core, 1000, &tow, &age));   /* Galileo only */
    h = msm(GNSS_SYS_GPS, 345000, true, 0x3, 0x2);
    obs_core_feed_msm(&s_core, &h, NULL, 1000);
    h = msm(GNSS_SYS_GPS, 345000, false, 0xC, 0x2);
    obs_core_feed_msm(&s_core, &h, NULL, 1003);
    TEST_ASSERT_TRUE(obs_core_gps_time(&s_core, 1250, &tow, &age));
    TEST_ASSERT_EQUAL_UINT32(345000, tow);
    TEST_ASSERT_EQUAL_UINT32(247, age);
    h = msm(GNSS_SYS_GPS, 346000, false, 0xF, 0x2);
    obs_core_feed_msm(&s_core, &h, NULL, 2000);
    TEST_ASSERT_TRUE(obs_core_gps_time(&s_core, 2000, &tow, &age));
    TEST_ASSERT_EQUAL_UINT32(346000, tow);
    TEST_ASSERT_EQUAL_UINT32(0, age);
    /* stale after the window; an epoch time beyond a week is no time of week */
    TEST_ASSERT_FALSE(obs_core_gps_time(&s_core, 2000 + OBS_SIGNAL_WINDOW_MS, &tow, &age));
    h = msm(GNSS_SYS_GPS, 604800000u, false, 0xF, 0x2);
    obs_core_feed_msm(&s_core, &h, NULL, 3000);
    TEST_ASSERT_FALSE(obs_core_gps_time(&s_core, 3000, &tow, &age));
}

TEST_CASE("obs: nothing fed means no bands and no epoch", "[gnss_obs]")
{
    obs_core_init(&s_core);
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
    obs_core_init(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, false, 1, sigs(three, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 0);                  /* t = 0 is a valid time */
    h = msm(GNSS_SYS_GPS, 6000, false, 1, sigs(two, 2));
    obs_core_feed_msm(&s_core, &h, NULL, 5000);

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
    obs_core_init(&s_core);

    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 1, false, 1, sigs(no_e6, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 1000);
    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);                             /* never seen: not expected */

    h = msm(GNSS_SYS_GALILEO, 2, false, 1, sigs(with_e6, 4));
    obs_core_feed_msm(&s_core, &h, NULL, 2000);
    obs_core_view(&s_core, 2000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
    TEST_ASSERT_EQUAL_UINT8(4, v.best_bands);

    for (uint64_t t = 3000; t <= 13000; t += 1000) {          /* E6 stops */
        h = msm(GNSS_SYS_GALILEO, (uint32_t)t, false, 1, sigs(no_e6, 3));
        obs_core_feed_msm(&s_core, &h, NULL, t);
    }
    obs_core_view(&s_core, 11999, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
    obs_core_view(&s_core, 12000, &v);
    TEST_ASSERT_TRUE(v.e6_lost);
    TEST_ASSERT_EQUAL_UINT8(3, v.best_bands);

    obs_core_reset(&s_core);                                  /* new signal group: forget */
    h = msm(GNSS_SYS_GALILEO, 1, false, 1, sigs(no_e6, 3));
    obs_core_feed_msm(&s_core, &h, NULL, 20000);
    obs_core_view(&s_core, 20000, &v);
    TEST_ASSERT_FALSE(v.e6_lost);
}

TEST_CASE("obs: satellites are counted over a split epoch", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_init(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_BEIDOU, 500, true, 0x000FFFFFull, 1u << 1);       /* 20 satellites */
    obs_core_feed_msm(&s_core, &h, NULL, 10);
    h = msm(GNSS_SYS_BEIDOU, 500, false, 0x7FFF00000ull, 1u << 1);                       /* 15 more */
    obs_core_feed_msm(&s_core, &h, NULL, 11);
    obs_core_view(&s_core, 20, &v);
    TEST_ASSERT_EQUAL_UINT8(35, v.sys[GNSS_SYS_BEIDOU].satellites);

    h = msm(GNSS_SYS_BEIDOU, 1500, false, 0x1Full, 1u << 1);                             /* next epoch */
    obs_core_feed_msm(&s_core, &h, NULL, 1010);
    obs_core_view(&s_core, 1020, &v);
    TEST_ASSERT_EQUAL_UINT8(5, v.sys[GNSS_SYS_BEIDOU].satellites);
}

TEST_CASE("obs: epochs close on the multiple-message bit", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_init(&s_core);
    for (uint32_t e = 0; e < 3; e++) {
        uint64_t t = 1000u * e;
        gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, e, true, 1, 2);
        obs_core_feed_msm(&s_core, &h, NULL, t);
        h = msm(GNSS_SYS_GALILEO, e, true, 1, 2);
        obs_core_feed_msm(&s_core, &h, NULL, t + 5);
        h = msm(GNSS_SYS_BEIDOU, e, false, 1, 2);
        obs_core_feed_msm(&s_core, &h, NULL, t + 9);
    }
    obs_core_view(&s_core, 2009, &v);
    TEST_ASSERT_EQUAL_UINT64(3, v.epochs);
    TEST_ASSERT_EQUAL_UINT32(0, v.epoch_age_ms);
    obs_core_view(&s_core, 2509, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.epoch_age_ms);
    obs_core_view(&s_core, 2009 + 0x200000000ull, &v);       /* age far beyond 32 bits */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1u, v.epoch_age_ms);
}

TEST_CASE("obs: an epoch whose closing MSM never came ends at the next epoch time", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_init(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, true, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, 0);
    h = msm(GNSS_SYS_GLONASS, 7000, true, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, 5);       /* the closing BeiDou MSM is lost */
    obs_core_view(&s_core, 900, &v);
    TEST_ASSERT_EQUAL_UINT64(0, v.epochs);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, v.epoch_age_ms);
    h = msm(GNSS_SYS_GPS, 2000, true, 1, 2);       /* next epoch: ends the open one */
    obs_core_feed_msm(&s_core, &h, NULL, 1000);
    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_EQUAL_UINT64(1, v.epochs);
    TEST_ASSERT_EQUAL_UINT32(0, v.epoch_age_ms);
    h = msm(GNSS_SYS_GPS, 2000, true, 2, 2);       /* same epoch, second GPS MSM: no end */
    obs_core_feed_msm(&s_core, &h, NULL, 1002);
    h = msm(GNSS_SYS_BEIDOU, 2000 - 14000 + 604800000u, false, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, 1009);
    obs_core_view(&s_core, 1009, &v);
    TEST_ASSERT_EQUAL_UINT64(2, v.epochs);
    h = msm(GNSS_SYS_GALILEO, 3000, true, 1, 2);   /* after a closed epoch nothing is open */
    obs_core_feed_msm(&s_core, &h, NULL, 2000);
    h = msm(GNSS_SYS_GPS, 3000, false, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, 2005);
    obs_core_view(&s_core, 2005, &v);
    TEST_ASSERT_EQUAL_UINT64(3, v.epochs);
}

TEST_CASE("obs: a signal or satellite in the masks without a cell is not observed", "[gnss_obs]")
{
    /* Two BeiDou satellites, signals 2I (2), 7I (14) and 7D (25); no cell
     * uses 7I, and the third satellite has no cell at all. */
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_BEIDOU, 500, false, 0x7u, (1u << 1) | (1u << 13) | (1u << 24));
    TEST_ASSERT_EQUAL_UINT8(9, h.num_cells);
    h.cell_mask = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5);   /* sat 1: 2I 7D, sat 2: 2I 7D */
    h.num_cells = 4;
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 24), gnss_rtcm3_msm_sigs_with_cells(&h));
    TEST_ASSERT_EQUAL_HEX64(0x3u, gnss_rtcm3_msm_sats_with_cells(&h));
    obs_core_init(&s_core);
    obs_core_feed_msm(&s_core, &h, NULL, 10);
    obs_core_feed_sats(&s_core, &h, NULL, 10);
    obs_core_view_t v;
    obs_core_view(&s_core, 20, &v);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 24), v.sys[GNSS_SYS_BEIDOU].signal_mask);
    TEST_ASSERT_EQUAL_UINT8(2, v.sys[GNSS_SYS_BEIDOU].satellites);
    TEST_ASSERT_EQUAL_HEX8((1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L2), v.sys[GNSS_SYS_BEIDOU].band_mask);
    obs_live_totals_t tot;
    obs_live_sat_t live[4];
    TEST_ASSERT_EQUAL_size_t(2, obs_core_live(&s_core, 20, live, 4, &tot));
    TEST_ASSERT_EQUAL_UINT16(4, tot.signals);

    /* Masks that do not fit together give nothing. */
    h.num_sats = 2;
    TEST_ASSERT_EQUAL_HEX32(0, gnss_rtcm3_msm_sigs_with_cells(&h));
    TEST_ASSERT_EQUAL_HEX64(0, gnss_rtcm3_msm_sats_with_cells(&h));
}

TEST_CASE("obs: the published 1074 frame end to end", "[gnss_obs]")
{
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(k_v1074, sizeof(k_v1074), &h));
    obs_core_init(&s_core);
    obs_core_feed_msm(&s_core, &h, NULL, 42);
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

/* ---- CNR ------------------------------------------------------------------ */

static obs_cnr_t cnr1(gnss_band_t band, uint16_t cells, uint32_t sum_q4)
{
    obs_cnr_t c;
    memset(&c, 0, sizeof(c));
    c.cells[band] = cells;
    c.sum_q4[band] = sum_q4;
    return c;
}

TEST_CASE("obs: CNR per band is the mean over the epoch's MSMs", "[gnss_obs]")
{
    static const uint8_t gal[] = { 2, 8, 14, 22 };          /* E1, E6, E5b, E5a */
    obs_core_view_t v;
    obs_core_init(&s_core);

    obs_cnr_t a = cnr1(GNSS_BAND_L1, 3, 3 * 720);             /* 45 dB-Hz */
    a.cells[GNSS_BAND_L5] = 2;
    a.sum_q4[GNSS_BAND_L5] = 2 * 760;                         /* 47.5 dB-Hz */
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 1000, true, 0x0F, sigs(gal, 4));
    obs_core_feed_msm(&s_core, &h, &a, 100);
    obs_core_view(&s_core, 105, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);   /* epoch still open */

    obs_cnr_t b = cnr1(GNSS_BAND_L1, 1, 640);                 /* 40 dB-Hz */
    b.cells[GNSS_BAND_L6] = 2;
    b.sum_q4[GNSS_BAND_L6] = 2 * 600;                         /* 37.5 dB-Hz */
    h = msm(GNSS_SYS_GALILEO, 1000, true, 0xF0, sigs(gal, 4));
    obs_core_feed_msm(&s_core, &h, &b, 110);
    h = msm(GNSS_SYS_BEIDOU, 986000, false, 1, 2);            /* last MSM of the epoch */
    obs_core_feed_msm(&s_core, &h, NULL, 120);

    obs_core_view(&s_core, 130, &v);
    const obs_core_sys_t *g = &v.sys[GNSS_SYS_GALILEO];
    TEST_ASSERT_EQUAL_UINT8(8, g->satellites);
    TEST_ASSERT_EQUAL_UINT8(44, g->cnr_dbhz[GNSS_BAND_L1]);  /* (3 x 45 + 40) / 4 = 43.75 */
    TEST_ASSERT_EQUAL_UINT8(48, g->cnr_dbhz[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT8(38, g->cnr_dbhz[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT8(0, g->cnr_dbhz[GNSS_BAND_L2]);   /* E5b present, no CNR sent */
    TEST_ASSERT_EQUAL_UINT8(0, g->cnr_dbhz[GNSS_BAND_E5AB]); /* band absent */
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_BEIDOU].cnr_dbhz[GNSS_BAND_L1]);
}

TEST_CASE("obs: CNR follows the latest epoch and leaves with its band", "[gnss_obs]")
{
    static const uint8_t l1_l5[] = { 2, 22 };
    static const uint8_t l1[] = { 2 };
    obs_core_view_t v;
    obs_core_init(&s_core);

    obs_cnr_t c = cnr1(GNSS_BAND_L1, 2, 2 * 45 * 16);
    c.cells[GNSS_BAND_L5] = 1;
    c.sum_q4[GNSS_BAND_L5] = 47 * 16;
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GALILEO, 1000, false, 3, sigs(l1_l5, 2));
    obs_core_feed_msm(&s_core, &h, &c, 0);
    c = cnr1(GNSS_BAND_L1, 2, 2 * 50 * 16);
    h = msm(GNSS_SYS_GALILEO, 2000, false, 3, sigs(l1, 1));
    obs_core_feed_msm(&s_core, &h, &c, 1000);

    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_EQUAL_UINT8(50, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(47, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L5]);   /* band still in the window */
    obs_core_view(&s_core, OBS_SIGNAL_WINDOW_MS, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GALILEO].band_mask & (1u << GNSS_BAND_L5));
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT8(50, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);
    obs_core_view(&s_core, 1000 + OBS_SIGNAL_WINDOW_MS, &v);
    TEST_ASSERT_FALSE(v.sys[GNSS_SYS_GALILEO].present);
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);

    /* A tiny mean still reads as measured, not as "not available". */
    obs_core_init(&s_core);
    c = cnr1(GNSS_BAND_L1, 1, 3);
    h = msm(GNSS_SYS_GALILEO, 1000, false, 1, sigs(l1, 1));
    obs_core_feed_msm(&s_core, &h, &c, 0);
    obs_core_view(&s_core, 0, &v);
    TEST_ASSERT_EQUAL_UINT8(1, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);
}

TEST_CASE("obs: without a closing MSM the next epoch time finishes the CNR", "[gnss_obs]")
{
    static const uint8_t l1[] = { 2 };
    obs_core_view_t v;
    obs_core_init(&s_core);
    obs_cnr_t c = cnr1(GNSS_BAND_L1, 1, 40 * 16);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, 1000, true, 1, sigs(l1, 1));
    obs_core_feed_msm(&s_core, &h, &c, 0);
    obs_core_view(&s_core, 0, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L1]);
    c = cnr1(GNSS_BAND_L1, 1, 44 * 16);
    h = msm(GNSS_SYS_GPS, 2000, true, 1, sigs(l1, 1));
    obs_core_feed_msm(&s_core, &h, &c, 1000);
    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_EQUAL_UINT8(40, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L1]);
    h = msm(GNSS_SYS_GPS, 3000, true, 1, sigs(l1, 1));
    obs_core_feed_msm(&s_core, &h, NULL, 2000);
    obs_core_view(&s_core, 2000, &v);
    TEST_ASSERT_EQUAL_UINT8(44, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT64(2, v.epochs);   /* epochs 1000 and 2000 ended by the next epoch time */
    TEST_ASSERT_EQUAL_UINT32(0, v.epoch_age_ms);
}

/* ---- epoch gaps ------------------------------------------------------------ */

#define TOW0 100000u
#define SYS_BIT(s) (1u << (s))

/* BeiDou time runs 14 s behind GPS time. */
static uint32_t epoch_of(gnss_sys_t sys, uint32_t gps_tow)
{
    return (sys == GNSS_SYS_BEIDOU) ? gps_tow - 14000u : gps_tow;
}

/* Output second i: one MSM per constellation in `mask`, the last one closing the epoch. */
static void feed_second(unsigned i, unsigned mask)
{
    unsigned last = 0;
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        if (mask & SYS_BIT(s)) {
            last = s;
        }
    }
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        if (mask & SYS_BIT(s)) {
            gnss_rtcm3_msm_t h = msm((gnss_sys_t)s, epoch_of((gnss_sys_t)s, TOW0 + 1000u * i), s != last, 1, 2);
            obs_core_feed_msm(&s_core, &h, NULL, 1000u * (uint64_t)i + s);
        }
    }
}

static const um98x_rtcm_out_t k_profile[] = {
    { 1005, 1 }, { 1033, 1 }, { 1077, 1 }, { 1097, 1 }, { 1127, 1 }, { 1230, 1 },
};
#define K_PROFILE_N (sizeof(k_profile) / sizeof(k_profile[0]))

TEST_CASE("obs: epoch gaps against the configured period count each lost epoch once", "[gnss_obs]")
{
    const unsigned all = SYS_BIT(GNSS_SYS_GPS) | SYS_BIT(GNSS_SYS_GALILEO) | SYS_BIT(GNSS_SYS_BEIDOU);
    const unsigned no_gps = SYS_BIT(GNSS_SYS_GALILEO) | SYS_BIT(GNSS_SYS_BEIDOU);
    obs_core_view_t v;
    obs_core_init(&s_core);
    obs_core_set_expected(&s_core, k_profile, K_PROFILE_N);

    for (unsigned i = 0; i <= 9; i++) {
        if (i != 5 && i != 6) {                    /* two epochs lost on the link */
            feed_second(i, all);
        }
    }
    obs_core_view(&s_core, 9010, &v);
    TEST_ASSERT_EQUAL_UINT32(1000, v.epoch_period_ms);
    TEST_ASSERT_EQUAL_UINT64(2, v.epoch_gaps);    /* not 2 per constellation */
    TEST_ASSERT_EQUAL_UINT64(2, v.sys[GNSS_SYS_GPS].epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(2, v.sys[GNSS_SYS_GALILEO].epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(2, v.sys[GNSS_SYS_BEIDOU].epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(0, v.epoch_jumps);
    TEST_ASSERT_EQUAL_UINT64(8, v.epochs);

    /* Galileo alone misses one epoch: its own count only. */
    feed_second(10, SYS_BIT(GNSS_SYS_GPS) | SYS_BIT(GNSS_SYS_BEIDOU));
    feed_second(11, all);
    obs_core_view(&s_core, 11010, &v);
    TEST_ASSERT_EQUAL_UINT64(2, v.epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(3, v.sys[GNSS_SYS_GALILEO].epoch_gaps);

    /* GPS stops for good: once it has left the window, Galileo counts for the station. */
    for (unsigned i = 12; i <= 25; i++) {
        feed_second(i, no_gps);
    }
    feed_second(27, no_gps);                       /* 26 lost */
    obs_core_view(&s_core, 27010, &v);
    TEST_ASSERT_FALSE(v.sys[GNSS_SYS_GPS].present);
    TEST_ASSERT_EQUAL_UINT64(3, v.epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(2, v.sys[GNSS_SYS_GPS].epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(4, v.sys[GNSS_SYS_GALILEO].epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(3, v.sys[GNSS_SYS_BEIDOU].epoch_gaps);

    /* A reset forgets the counts, not the configured period. */
    obs_core_reset(&s_core);
    obs_core_view(&s_core, 27010, &v);
    TEST_ASSERT_EQUAL_UINT64(0, v.epoch_gaps);
    TEST_ASSERT_EQUAL_UINT32(1000, v.epoch_period_ms);
}

TEST_CASE("obs: an outage counts once whichever constellation comes first after it", "[gnss_obs]")
{
    /* GPS and Galileo at 1 s; a 20 s link outage (19 epochs lost); in the
     * first epoch after it Galileo's MSM arrives before GPS's. */
    static const um98x_rtcm_out_t prof[] = { { 1077, 1 }, { 1097, 1 } };
    for (int order = 0; order < 2; order++) {
        obs_core_view_t v;
        obs_core_init(&s_core);
        obs_core_set_expected(&s_core, prof, 2);
        for (unsigned i = 0; i < 5; i++) {
            feed_second(i, SYS_BIT(GNSS_SYS_GPS) | SYS_BIT(GNSS_SYS_GALILEO));
        }
        const unsigned i = 24;
        gnss_rtcm3_msm_t g = msm(GNSS_SYS_GPS, TOW0 + 1000u * i, order == 0, 1, 2);
        gnss_rtcm3_msm_t e = msm(GNSS_SYS_GALILEO, TOW0 + 1000u * i, order == 1, 1, 2);
        if (order == 0) {
            obs_core_feed_msm(&s_core, &g, NULL, 1000u * i);
            obs_core_feed_msm(&s_core, &e, NULL, 1000u * i + 2);
        } else {
            obs_core_feed_msm(&s_core, &e, NULL, 1000u * i);
            obs_core_feed_msm(&s_core, &g, NULL, 1000u * i + 2);
        }
        obs_core_view(&s_core, 1000u * i + 10, &v);
        TEST_ASSERT_EQUAL_UINT64(19, v.sys[GNSS_SYS_GPS].epoch_gaps);
        TEST_ASSERT_EQUAL_UINT64(19, v.sys[GNSS_SYS_GALILEO].epoch_gaps);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(19, v.epoch_gaps, order == 0 ? "GPS first" : "Galileo first");
    }
}

static void feed_gps(uint32_t tow, uint64_t t)
{
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, tow, false, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, t);
}

TEST_CASE("obs: epoch gaps need a period; jumps, steps back and the week wrap", "[gnss_obs]")
{
    obs_core_view_t v;
    obs_core_init(&s_core);
    feed_gps(10000, 0);
    feed_gps(14000, 4000);
    obs_core_view(&s_core, 4000, &v);
    TEST_ASSERT_EQUAL_UINT32(0, v.epoch_period_ms);
    TEST_ASSERT_EQUAL_UINT64(0, v.epoch_gaps);    /* nothing configured: nothing counted */

    static const um98x_rtcm_out_t two_s[] = { { 1077, 2 } };
    obs_core_set_expected(&s_core, two_s, 1);
    feed_gps(16000, 6000);
    feed_gps(18000, 8000);
    feed_gps(18000 + 2900, 10900);                 /* under 1.5 periods: on time */
    obs_core_view(&s_core, 11000, &v);
    TEST_ASSERT_EQUAL_UINT64(0, v.epoch_gaps);
    feed_gps(24900, 14900);                        /* two periods: one missing */
    obs_core_view(&s_core, 15000, &v);
    TEST_ASSERT_EQUAL_UINT64(1, v.epoch_gaps);

    feed_gps(24900 + 70000, 85000);                /* outage over 60 s */
    feed_gps(80000, 86000);                        /* step back */
    feed_gps(604798000u, 87000);                   /* far ahead */
    feed_gps(2000, 91000);                         /* across the week: 4 s */
    obs_core_view(&s_core, 91000, &v);
    TEST_ASSERT_EQUAL_UINT64(3, v.epoch_jumps);
    TEST_ASSERT_EQUAL_UINT64(2, v.epoch_gaps);

    /* A slow period stretches the jump limit to four periods. */
    static const um98x_rtcm_out_t slow[] = { { 1077, 30 } };
    obs_core_set_expected(&s_core, slow, 1);
    feed_gps(92000, 200000);                       /* 90 s: two missing, not a jump */
    obs_core_view(&s_core, 200000, &v);
    TEST_ASSERT_EQUAL_UINT64(4, v.epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(3, v.epoch_jumps);
}

TEST_CASE("obs: GLONASS epoch gaps across the day and with an unknown day", "[gnss_obs]")
{
    static const um98x_rtcm_out_t glo[] = { { 1087, 1 } };
    obs_core_view_t v;
    obs_core_init(&s_core);
    obs_core_set_expected(&s_core, glo, 1);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GLONASS, (6u << 27) | 86399000u, false, 1, 2);
    obs_core_feed_msm(&s_core, &h, NULL, 0);
    h = msm(GNSS_SYS_GLONASS, (0u << 27) | 1000u, false, 1, 2);          /* Saturday -> Sunday, 2 s */
    obs_core_feed_msm(&s_core, &h, NULL, 2000);
    h = msm(GNSS_SYS_GLONASS, (7u << 27) | 5000u, false, 1, 2);          /* day unknown: time of day only */
    obs_core_feed_msm(&s_core, &h, NULL, 6000);
    h = msm(GNSS_SYS_GLONASS, (7u << 27) | 86399500u, false, 1, 2);      /* far: a jump */
    obs_core_feed_msm(&s_core, &h, NULL, 7000);
    h = msm(GNSS_SYS_GLONASS, (7u << 27) | 500u, false, 1, 2);           /* across midnight: 1 s */
    obs_core_feed_msm(&s_core, &h, NULL, 8000);
    obs_core_view(&s_core, 8000, &v);
    TEST_ASSERT_EQUAL_UINT64(1 + 3, v.epoch_gaps);
    TEST_ASSERT_EQUAL_UINT64(1, v.epoch_jumps);
    TEST_ASSERT_EQUAL_UINT64(4, v.sys[GNSS_SYS_GLONASS].epoch_gaps);
}

/* ---- message statistics ------------------------------------------------- */

static const obs_msg_stat_t *find_stat(const obs_msg_stat_t *st, size_t n, uint16_t msg)
{
    for (size_t i = 0; i < n; i++) {
        if (st[i].msg == msg) {
            return &st[i];
        }
    }
    return NULL;
}

TEST_CASE("obs: message statistics count epochs, measure periods, list the profile", "[gnss_obs]")
{
    static const um98x_rtcm_out_t prof[] = { { 1230, 10 }, { 1077, 5 }, { 1005, 10 }, { 1077, 1 } };
    obs_msg_stat_t st[OBS_MSG_MAX];
    obs_core_init(&s_core);
    obs_core_set_expected(&s_core, prof, 4);

    size_t n = obs_core_msgs(&s_core, 0, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT16(1005, st[0].msg);
    TEST_ASSERT_EQUAL_UINT16(1077, st[1].msg);
    TEST_ASSERT_EQUAL_UINT16(1, st[1].expected_period_s);     /* listed twice: the shorter period */
    TEST_ASSERT_EQUAL_UINT16(1230, st[2].msg);
    TEST_ASSERT_EQUAL_UINT64(0, st[2].count);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, st[2].period_ms);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, st[2].age_ms);

    for (unsigned i = 0; i <= 20; i++) {
        uint64_t t = 1000u * (uint64_t)i;
        obs_core_feed_msg(&s_core, 1077, true, TOW0 + 1000u * i, t);       /* two frames per epoch */
        obs_core_feed_msg(&s_core, 1077, true, TOW0 + 1000u * i, t + 3);
        obs_core_feed_msg(&s_core, 1033, false, 0, t + 5);
        if (i % 10 == 0) {
            obs_core_feed_msg(&s_core, 1005, false, 0, t + 7);
        }
    }
    obs_core_feed_msg(&s_core, 0, false, 0, 20008);                       /* no number: ignored */
    n = obs_core_msgs(&s_core, 20010, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL(4, n);
    static const uint16_t order[] = { 1005, 1033, 1077, 1230 };
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(order[i], st[i].msg);
    }
    const obs_msg_stat_t *m = find_stat(st, n, 1077);
    TEST_ASSERT_EQUAL_UINT64(21, m->count);
    TEST_ASSERT_EQUAL_UINT64(42, m->frames);
    TEST_ASSERT_EQUAL_UINT32(1000, m->period_ms);
    TEST_ASSERT_EQUAL_UINT32(7, m->age_ms);
    m = find_stat(st, n, 1005);
    TEST_ASSERT_EQUAL_UINT64(3, m->count);
    TEST_ASSERT_EQUAL_UINT32(10000, m->period_ms);
    TEST_ASSERT_EQUAL_UINT32(3, m->age_ms);
    TEST_ASSERT_EQUAL_UINT16(10, m->expected_period_s);
    m = find_stat(st, n, 1033);
    TEST_ASSERT_EQUAL_UINT16(0, m->expected_period_s);       /* not in the profile */
    TEST_ASSERT_EQUAL_UINT64(21, m->count);
    m = find_stat(st, n, 1230);
    TEST_ASSERT_EQUAL_UINT64(0, m->count);

    /* Two epochs lost: the mean over the last 8 intervals shows it. */
    obs_core_feed_msg(&s_core, 1077, true, TOW0 + 23000u, 23000);
    n = obs_core_msgs(&s_core, 23000, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL_UINT32((7 * 1000 + 3000) / 8, find_stat(st, n, 1077)->period_ms);

    /* A small capacity keeps the lowest numbers. */
    n = obs_core_msgs(&s_core, 23000, st, 2);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT16(1005, st[0].msg);
    TEST_ASSERT_EQUAL_UINT16(1033, st[1].msg);
    TEST_ASSERT_EQUAL(0, obs_core_msgs(&s_core, 23000, NULL, 0));

    /* A reset keeps the profile's numbers, counts start over. */
    obs_core_reset(&s_core);
    n = obs_core_msgs(&s_core, 23000, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT64(0, find_stat(st, n, 1077)->count);
    TEST_ASSERT_NULL(find_stat(st, n, 1033));
}

TEST_CASE("obs: a full message table keeps the profile and gives up stale numbers", "[gnss_obs]")
{
    static const um98x_rtcm_out_t a[] = { { 1077, 1 } };
    static const um98x_rtcm_out_t b[] = { { 1087, 1 } };
    obs_msg_stat_t st[OBS_MSG_MAX];
    obs_core_view_t v;
    obs_core_init(&s_core);
    obs_core_set_expected(&s_core, a, 1);
    for (uint16_t msg = 2000; msg < 2000 + OBS_MSG_MAX - 1; msg++) {
        obs_core_feed_msg(&s_core, msg, false, 0, 10);
    }
    obs_core_feed_msg(&s_core, 3000, false, 0, 1000);                    /* all fresh: not taken */
    obs_core_view(&s_core, 1000, &v);
    TEST_ASSERT_EQUAL_UINT32(1, v.msg_dropped);
    obs_core_feed_msg(&s_core, 3000, false, 0, 10 + OBS_MSG_STALE_MS);   /* 2000 went stale */
    size_t n = obs_core_msgs(&s_core, 10 + OBS_MSG_STALE_MS, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL(OBS_MSG_MAX, n);
    TEST_ASSERT_NOT_NULL(find_stat(st, n, 3000));
    TEST_ASSERT_NULL(find_stat(st, n, 2000));
    TEST_ASSERT_NOT_NULL(find_stat(st, n, 1077));                        /* never seen, still listed */

    /* A new profile takes the place of a number nobody sends. */
    obs_core_set_expected(&s_core, b, 1);
    n = obs_core_msgs(&s_core, 10 + OBS_MSG_STALE_MS, st, OBS_MSG_MAX);
    TEST_ASSERT_NOT_NULL(find_stat(st, n, 1087));
    TEST_ASSERT_NULL(find_stat(st, n, 1077));
    TEST_ASSERT_NOT_NULL(find_stat(st, n, 2001));
}

/* ---- sky view ------------------------------------------------------------- */

static obs_gsv_t gsv(gnss_sys_t sys, const uint8_t *sat, const int8_t *el, const uint16_t *az, const uint8_t *cnr,
                     unsigned n)
{
    obs_gsv_t g;
    memset(&g, 0, sizeof(g));
    g.total = 1;
    g.number = 1;
    g.in_view = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        g.sat[i].sys = sys;
        g.sat[i].sat = sat[i];
        g.sat[i].has_pos = (el != NULL);
        g.sat[i].elevation_deg = el ? el[i] : 0;
        g.sat[i].azimuth_deg = az ? az[i] : 0;
        g.sat[i].cnr_dbhz = cnr[i];
    }
    g.count = (uint8_t)n;
    return g;
}

TEST_CASE("obs: sky view from satellite-in-view sentences", "[gnss_obs]")
{
    static const uint8_t gps[] = { 10, 7, 5 };
    static const int8_t el[] = { 63, 61, 59 };
    static const uint16_t az[] = { 137, 98, 290 };
    static const uint8_t l1[] = { 17, 15, 0 };
    static const uint8_t l5[] = { 30, 0, 0 };
    static const uint8_t gal[] = { 4 };
    static const int8_t gal_el[] = { 51 };
    static const uint16_t gal_az[] = { 180 };
    static const uint8_t gal_cnr[] = { 44 };
    obs_sat_t sky[OBS_SKY_MAX];
    obs_core_init(&s_core);
    TEST_ASSERT_FALSE(obs_core_sky_available(&s_core, 0));
    TEST_ASSERT_EQUAL(0, obs_core_sky(&s_core, 0, sky, OBS_SKY_MAX));

    obs_gsv_t g = gsv(GNSS_SYS_GALILEO, gal, gal_el, gal_az, gal_cnr, 1);
    obs_core_feed_gsv(&s_core, &g, 1000);
    g = gsv(GNSS_SYS_GPS, gps, el, az, l1, 3);
    obs_core_feed_gsv(&s_core, &g, 1002);
    g = gsv(GNSS_SYS_GPS, gps, el, az, l5, 3);                  /* the same satellites on L5 */
    obs_core_feed_gsv(&s_core, &g, 1004);

    TEST_ASSERT_TRUE(obs_core_sky_available(&s_core, 1100));
    size_t n = obs_core_sky(&s_core, 1100, sky, OBS_SKY_MAX);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, sky[0].sys);
    TEST_ASSERT_EQUAL_UINT8(5, sky[0].sat);
    TEST_ASSERT_EQUAL_UINT8(0, sky[0].cnr_dbhz);               /* in view, not tracked */
    TEST_ASSERT_EQUAL_UINT8(7, sky[1].sat);
    TEST_ASSERT_EQUAL_UINT8(15, sky[1].cnr_dbhz);
    TEST_ASSERT_EQUAL_UINT8(10, sky[2].sat);
    TEST_ASSERT_EQUAL_UINT8(30, sky[2].cnr_dbhz);              /* best signal of the burst */
    TEST_ASSERT_EQUAL_INT8(63, sky[2].elevation_deg);
    TEST_ASSERT_EQUAL_UINT16(137, sky[2].azimuth_deg);
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, sky[3].sys);
    TEST_ASSERT_EQUAL_UINT8(4, sky[3].sat);

    /* Next report 10 s later: values of the new burst replace the old ones. */
    static const uint8_t ten[] = { 10 };
    static const int8_t ten_el[] = { 60 };
    static const uint16_t ten_az[] = { 140 };
    static const uint8_t ten_l1[] = { 20 };
    static const uint8_t ten_l5[] = { 0 };
    g = gsv(GNSS_SYS_GPS, ten, ten_el, ten_az, ten_l1, 1);
    obs_core_feed_gsv(&s_core, &g, 11000);
    g = gsv(GNSS_SYS_GPS, ten, ten_el, ten_az, ten_l5, 1);
    obs_core_feed_gsv(&s_core, &g, 11001);
    static const uint8_t newsat[] = { 12 };
    static const uint8_t newcnr[] = { 33 };
    g = gsv(GNSS_SYS_GPS, newsat, NULL, NULL, newcnr, 1);      /* no position: nothing to draw */
    obs_core_feed_gsv(&s_core, &g, 11002);
    n = obs_core_sky(&s_core, 11100, sky, OBS_SKY_MAX);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_UINT8(20, sky[2].cnr_dbhz);
    TEST_ASSERT_EQUAL_INT8(60, sky[2].elevation_deg);

    /* Satellites not reported for 30 s leave; the view ends 30 s after the last sentence. */
    n = obs_core_sky(&s_core, 1004 + OBS_SKY_WINDOW_MS, sky, OBS_SKY_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_UINT8(10, sky[0].sat);
    TEST_ASSERT_TRUE(obs_core_sky_available(&s_core, 11002 + OBS_SKY_WINDOW_MS - 1));
    TEST_ASSERT_FALSE(obs_core_sky_available(&s_core, 11002 + OBS_SKY_WINDOW_MS));

    /* A small capacity keeps the lowest constellation and satellite numbers. */
    n = obs_core_sky(&s_core, 11100, sky, 2);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT8(5, sky[0].sat);
    TEST_ASSERT_EQUAL_UINT8(7, sky[1].sat);
}

TEST_CASE("obs: a full sky table takes new satellites only as old ones expire", "[gnss_obs]")
{
    obs_sat_t sky[OBS_SKY_MAX];
    obs_core_init(&s_core);
    static const int8_t el[] = { 10 };
    static const uint16_t az[] = { 20 };
    static const uint8_t cnr[] = { 30 };
    for (unsigned i = 0; i < OBS_SKY_MAX; i++) {
        uint8_t sat = (uint8_t)(1 + i % 60);
        obs_gsv_t g = gsv(i < 60 ? GNSS_SYS_BEIDOU : GNSS_SYS_GPS, &sat, el, az, cnr, 1);
        obs_core_feed_gsv(&s_core, &g, 100 + i);
    }
    uint8_t one = 1;
    obs_gsv_t g = gsv(GNSS_SYS_GALILEO, &one, el, az, cnr, 1);
    obs_core_feed_gsv(&s_core, &g, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, s_core.sky_dropped);
    TEST_ASSERT_EQUAL(OBS_SKY_MAX, obs_core_sky(&s_core, 1000, sky, OBS_SKY_MAX));

    obs_core_feed_gsv(&s_core, &g, 100 + OBS_SKY_WINDOW_MS);    /* the first BeiDou entry expired */
    size_t n = obs_core_sky(&s_core, 100 + OBS_SKY_WINDOW_MS, sky, OBS_SKY_MAX);
    TEST_ASSERT_EQUAL(OBS_SKY_MAX, n);                          /* BeiDou 1 gone, Galileo 1 in */
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, sky[20].sys);           /* after the 20 GPS satellites */
    TEST_ASSERT_EQUAL(GNSS_SYS_BEIDOU, sky[21].sys);
    TEST_ASSERT_EQUAL_UINT8(2, sky[21].sat);
}

/* ---- end to end ------------------------------------------------------------ */

static uint8_t s_frame[GNSS_RTCM3_MAX_FRAME];

/* What the RTOS wrapper does with one frame. */
static void feed_frame(const uint8_t *frame, size_t len, uint64_t t)
{
    gnss_rtcm3_msm_t h;
    obs_cnr_t cnr;
    uint16_t type = gnss_rtcm3_msg_type(frame, len);
    bool is_msm = gnss_rtcm3_msm_decode(frame, len, &h);
    bool has_cnr = is_msm && obs_msm_cnr(frame, len, &h, &cnr, NULL);
    obs_core_feed_msg(&s_core, type, is_msm, is_msm ? h.epoch_time : 0, t);
    if (is_msm) {
        obs_core_feed_msm(&s_core, &h, has_cnr ? &cnr : NULL, t);
    }
}

TEST_CASE("obs: MSM7 frames built per the RTCM layout, end to end", "[gnss_obs]")
{
    static const uint8_t gps_sats[] = { 2, 13, 24 };
    static const uint8_t gps_sigs[] = { 2, 16, 23 };           /* 1C, 2L, 5Q */
    static const uint16_t gps_cnr[9] = {
        46 * 16, 40 * 16, 44 * 16,
        41 * 16, 35 * 16, 39 * 16,
        48 * 16, 43 * 16, 49 * 16,
    };
    static const uint8_t gal_sats[] = { 5, 9 };
    static const uint8_t gal_sigs[] = { 2, 9, 15, 23 };        /* E1, E6, E5b, E5a */
    static const uint16_t gal_cnr[8] = {
        45 * 16, 42 * 16, 46 * 16, 47 * 16,
        43 * 16, 40 * 16, 44 * 16, 45 * 16,
    };
    static const um98x_rtcm_out_t prof[] = { { 1077, 1 }, { 1097, 1 } };
    obs_core_init(&s_core);
    obs_core_set_expected(&s_core, prof, 2);

    for (unsigned i = 0; i < 3; i++) {
        uint32_t tow = TOW0 + 1000u * i;
        uint64_t t = 1000u * (uint64_t)i;
        msm_desc_t g = { .type = 1077, .epoch = tow, .more = true, .sats = gps_sats, .nsats = 3,
                         .sigs = gps_sigs, .nsigs = 3, .cnr = gps_cnr };
        size_t len = msm_build(s_frame, &g);
        TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(s_frame, len));
        feed_frame(s_frame, len, t);
        msm_desc_t e = { .type = 1097, .epoch = tow, .more = false, .sats = gal_sats, .nsats = 2,
                         .sigs = gal_sigs, .nsigs = 4, .cnr = gal_cnr };
        len = msm_build(s_frame, &e);
        feed_frame(s_frame, len, t + 20);
    }
    feed_frame(k_v1005, sizeof(k_v1005), 2050);

    obs_core_view_t v;
    obs_core_view(&s_core, 2100, &v);
    TEST_ASSERT_EQUAL_UINT8(3, v.sys[GNSS_SYS_GPS].satellites);
    TEST_ASSERT_EQUAL_UINT8(3, v.sys[GNSS_SYS_GPS].bands);
    TEST_ASSERT_EQUAL_UINT8(45, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L1]);   /* (46 + 41 + 48) / 3 */
    TEST_ASSERT_EQUAL_UINT8(39, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L2]);   /* (40 + 35 + 43) / 3 = 39.3 */
    TEST_ASSERT_EQUAL_UINT8(44, v.sys[GNSS_SYS_GPS].cnr_dbhz[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, v.best_sys);
    TEST_ASSERT_EQUAL_UINT8(4, v.best_bands);
    TEST_ASSERT_EQUAL_UINT8(44, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(41, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT8(45, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT8(46, v.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT64(3, v.epochs);
    TEST_ASSERT_EQUAL_UINT64(0, v.epoch_gaps);

    obs_msg_stat_t st[OBS_MSG_MAX];
    size_t n = obs_core_msgs(&s_core, 2100, st, OBS_MSG_MAX);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT16(1005, st[0].msg);
    TEST_ASSERT_EQUAL_UINT16(0, st[0].expected_period_s);
    TEST_ASSERT_EQUAL_UINT64(3, find_stat(st, n, 1077)->count);
    TEST_ASSERT_EQUAL_UINT32(1000, find_stat(st, n, 1097)->period_ms);
}

/* ---- live view: satellites of the latest MSM epochs ------------------------------ */

/* A whole frame as gnss_obs feeds it, the per-satellite table included. */
static void feed_live(const uint8_t *frame, size_t len, uint64_t t)
{
    gnss_rtcm3_msm_t h;
    obs_cnr_t cnr;
    uint16_t cells[GNSS_RTCM3_MSM_MAX_CELLS];
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(frame, len, &h));
    bool has_cnr = obs_msm_cnr(frame, len, &h, &cnr, cells);
    obs_core_feed_msg(&s_core, h.msg_type, true, h.epoch_time, t);
    obs_core_feed_msm(&s_core, &h, has_cnr ? &cnr : NULL, t);
    obs_core_feed_sats(&s_core, &h, has_cnr ? cells : NULL, t);
}

static const obs_live_sat_t *live_find(const obs_live_sat_t *v, size_t n, gnss_sys_t sys, uint8_t sat)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i].sys == sys && v[i].sat == sat) {
            return &v[i];
        }
    }
    return NULL;
}

TEST_CASE("obs: live view lists every satellite with its bands, CNR and position", "[gnss_obs]")
{
    /* GPS: 2, 13, 24 on L1/L2/L5 (24 without L2), then 30 on L1 in a second
     * frame of the same epoch; Galileo 5 and 9 on E1/E6/E5b/E5a. */
    static const uint8_t gps_sats[] = { 2, 13, 24 };
    static const uint8_t gps_sigs[] = { 2, 16, 23 };                  /* 1C, 2L, 5Q */
    static const uint16_t gps_cnr[8] = {
        46 * 16, 40 * 16, 44 * 16,
        41 * 16, 35 * 16, 39 * 16,
        48 * 16, 49 * 16,
    };
    static const uint8_t gps2_sats[] = { 30 };
    static const uint8_t gps2_sigs[] = { 2 };
    static const uint16_t gps2_cnr[1] = { 30 * 16 };
    static const uint8_t gal_sats[] = { 5, 9 };
    static const uint8_t gal_sigs[] = { 2, 9, 15, 23 };               /* E1, E6, E5b, E5a */
    static const uint16_t gal_cnr[8] = {
        45 * 16, 42 * 16, 46 * 16, 47 * 16,
        43 * 16, 40 * 16, 44 * 16, 45 * 16,
    };
    obs_core_init(&s_core);
    obs_live_totals_t tot;
    obs_live_sat_t v[OBS_LIVE_MAX];
    TEST_ASSERT_EQUAL(0, obs_core_live(&s_core, 0, v, OBS_LIVE_MAX, &tot));
    TEST_ASSERT_EQUAL_UINT16(0, tot.satellites);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, tot.msm_age_ms);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, tot.sky_age_ms);

    msm_desc_t g = { .type = 1077, .epoch = TOW0, .more = true, .sats = gps_sats, .nsats = 3, .sigs = gps_sigs,
                     .nsigs = 3, .absent = (uint64_t)1 << 7, .cnr = gps_cnr };
    feed_live(s_frame, msm_build(s_frame, &g), 0);
    msm_desc_t g2 = { .type = 1077, .epoch = TOW0, .more = true, .sats = gps2_sats, .nsats = 1, .sigs = gps2_sigs,
                      .nsigs = 1, .cnr = gps2_cnr };
    feed_live(s_frame, msm_build(s_frame, &g2), 5);
    msm_desc_t e = { .type = 1097, .epoch = TOW0, .more = false, .sats = gal_sats, .nsats = 2, .sigs = gal_sigs,
                     .nsigs = 4, .cnr = gal_cnr };
    feed_live(s_frame, msm_build(s_frame, &e), 10);

    /* Azimuth and elevation for GPS 2, and GPS 7 the MSMs do not carry. */
    static const uint8_t sky_sats[] = { 2, 7 };
    static const int8_t sky_el[] = { 45, 10 };
    static const uint16_t sky_az[] = { 120, 300 };
    static const uint8_t sky_cnr[] = { 46, 20 };
    obs_gsv_t s = gsv(GNSS_SYS_GPS, sky_sats, sky_el, sky_az, sky_cnr, 2);
    obs_core_feed_gsv(&s_core, &s, 100);

    size_t n = obs_core_live(&s_core, 200, v, OBS_LIVE_MAX, &tot);
    TEST_ASSERT_EQUAL(7, n);
    /* Sorted by constellation, then satellite number. */
    static const uint8_t order[] = { 2, 7, 13, 24, 30, 5, 9 };
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(order[i], v[i].sat);
        TEST_ASSERT_EQUAL(i < 5 ? GNSS_SYS_GPS : GNSS_SYS_GALILEO, v[i].sys);
    }
    const obs_live_sat_t *x = live_find(v, n, GNSS_SYS_GPS, 2);
    TEST_ASSERT_TRUE(x->in_msm);
    TEST_ASSERT_TRUE(x->has_pos);
    TEST_ASSERT_EQUAL_INT8(45, x->elevation_deg);
    TEST_ASSERT_EQUAL_UINT16(120, x->azimuth_deg);
    TEST_ASSERT_EQUAL_UINT32(100, x->pos_age_ms);
    TEST_ASSERT_EQUAL_UINT32(200, x->msm_age_ms);
    TEST_ASSERT_EQUAL_HEX32(sigs(gps_sigs, 3), x->sig_mask);
    TEST_ASSERT_EQUAL_UINT8(46, x->cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(40, x->cnr_dbhz[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT8(44, x->cnr_dbhz[GNSS_BAND_L5]);
    x = live_find(v, n, GNSS_SYS_GPS, 24);
    TEST_ASSERT_EQUAL_HEX8((1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L5), x->band_mask);   /* no L2 cell */
    TEST_ASSERT_EQUAL_UINT8(48, x->cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(0, x->cnr_dbhz[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT8(49, x->cnr_dbhz[GNSS_BAND_L5]);
    TEST_ASSERT_FALSE(x->has_pos);
    x = live_find(v, n, GNSS_SYS_GPS, 30);   /* from the second frame of the epoch */
    TEST_ASSERT_EQUAL_UINT8(30, x->cnr_dbhz[GNSS_BAND_L1]);
    x = live_find(v, n, GNSS_SYS_GPS, 7);    /* only the sky view knows it */
    TEST_ASSERT_FALSE(x->in_msm);
    TEST_ASSERT_TRUE(x->has_pos);
    TEST_ASSERT_EQUAL_UINT8(0, x->band_mask);
    TEST_ASSERT_EQUAL_UINT8(20, x->sky_cnr_dbhz);
    TEST_ASSERT_EQUAL_UINT32(OBS_NEVER, x->msm_age_ms);
    x = live_find(v, n, GNSS_SYS_GALILEO, 9);
    TEST_ASSERT_EQUAL_UINT8(40, x->cnr_dbhz[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT8(44, x->cnr_dbhz[GNSS_BAND_L2]);

    /* Totals: per constellation and band, per band, tiles. */
    TEST_ASSERT_EQUAL_UINT16(7, tot.satellites);
    TEST_ASSERT_EQUAL_UINT16(6, tot.tracked);
    TEST_ASSERT_EQUAL_UINT16(17, tot.signals);
    TEST_ASSERT_EQUAL_UINT16(2, tot.with_pos);
    TEST_ASSERT_EQUAL_UINT16(0, tot.dropped);
    TEST_ASSERT_EQUAL_UINT8(4, tot.sys_sats[GNSS_SYS_GPS]);
    TEST_ASSERT_EQUAL_UINT8(2, tot.sys_sats[GNSS_SYS_GALILEO]);
    TEST_ASSERT_EQUAL_UINT8(4, tot.sys_band_sats[GNSS_SYS_GPS][GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(2, tot.sys_band_sats[GNSS_SYS_GPS][GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT8(3, tot.sys_band_sats[GNSS_SYS_GPS][GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT8(2, tot.sys_band_sats[GNSS_SYS_GALILEO][GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT16(6, tot.band_sats[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT16(4, tot.band_sats[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT16(5, tot.band_sats[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT16(2, tot.band_sats[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT16(0, tot.band_sats[GNSS_BAND_E5AB]);
    TEST_ASSERT_EQUAL_UINT8(42, tot.band_cnr_dbhz[GNSS_BAND_L1]);   /* (46+41+48+30+45+43) / 6 = 42.2 */
    TEST_ASSERT_EQUAL_UINT8(41, tot.band_cnr_dbhz[GNSS_BAND_L2]);   /* (40+35+46+44) / 4 = 41.25 */
    TEST_ASSERT_EQUAL_UINT8(45, tot.band_cnr_dbhz[GNSS_BAND_L5]);   /* (44+39+49+47+45) / 5 = 44.8 */
    TEST_ASSERT_EQUAL_UINT8(41, tot.band_cnr_dbhz[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT8(0, tot.band_cnr_dbhz[GNSS_BAND_E5AB]);
    TEST_ASSERT_EQUAL_UINT8(43, tot.cnr_dbhz);                      /* 724 / 17 = 42.6 */
    TEST_ASSERT_EQUAL_UINT32(190, tot.msm_age_ms);
    TEST_ASSERT_EQUAL_UINT32(100, tot.sky_age_ms);

    /* A small capacity keeps the first ones; the totals still count all. */
    n = obs_core_live(&s_core, 200, v, 3, &tot);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT16(7, tot.satellites);
    TEST_ASSERT_EQUAL_UINT16(4, tot.dropped);

    /* The next epoch starts a satellite over; one it no longer lists stays
     * for the 10 s window, then leaves. */
    static const uint8_t one[] = { 2 };
    static const uint16_t one_cnr[1] = { 50 * 16 };
    msm_desc_t g3 = { .type = 1077, .epoch = TOW0 + 1000u, .more = false, .sats = one, .nsats = 1,
                      .sigs = gps2_sigs, .nsigs = 1, .cnr = one_cnr };
    feed_live(s_frame, msm_build(s_frame, &g3), 1000);
    n = obs_core_live(&s_core, 1000, v, OBS_LIVE_MAX, &tot);
    x = live_find(v, n, GNSS_SYS_GPS, 2);
    TEST_ASSERT_EQUAL_HEX8(1u << GNSS_BAND_L1, x->band_mask);
    TEST_ASSERT_EQUAL_UINT8(50, x->cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(0, x->cnr_dbhz[GNSS_BAND_L2]);
    TEST_ASSERT_NOT_NULL(live_find(v, n, GNSS_SYS_GPS, 13));
    n = obs_core_live(&s_core, OBS_SIGNAL_WINDOW_MS + 10, v, OBS_LIVE_MAX, &tot);
    TEST_ASSERT_NULL(live_find(v, n, GNSS_SYS_GPS, 13));
    TEST_ASSERT_NOT_NULL(live_find(v, n, GNSS_SYS_GPS, 2));
    TEST_ASSERT_NOT_NULL(live_find(v, n, GNSS_SYS_GPS, 7));   /* the sky view keeps it for 30 s */
    TEST_ASSERT_EQUAL_UINT16(1, tot.tracked);

    /* MSM1-3 carry no CNR: bands without values. */
    obs_core_reset(&s_core);
    gnss_rtcm3_msm_t h = msm(GNSS_SYS_GPS, TOW0, false, 1u << 3, sigs(gps_sigs, 3));
    h.num_sats = 1;
    h.num_sigs = 3;
    h.num_cells = 3;
    h.cell_mask = 0x7;
    obs_core_feed_sats(&s_core, &h, NULL, 0);
    n = obs_core_live(&s_core, 0, v, OBS_LIVE_MAX, &tot);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_UINT8(4, v[0].sat);
    TEST_ASSERT_EQUAL_HEX8((1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L2) | (1u << GNSS_BAND_L5), v[0].band_mask);
    TEST_ASSERT_EQUAL_UINT8(0, v[0].cnr_dbhz[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT8(0, tot.cnr_dbhz);
    TEST_ASSERT_EQUAL_UINT16(3, tot.band_sats[GNSS_BAND_L1] + tot.band_sats[GNSS_BAND_L2] + tot.band_sats[GNSS_BAND_L5]);
}
