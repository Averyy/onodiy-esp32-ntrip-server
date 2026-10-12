/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM3 decode tests: MSM header (published 1074 vector and constructed
 * headers), MSM type rules, 1005/1006 antenna reference point, signal ID to
 * band mapping.
 */
#include <string.h>

#include "gnss_framing.h"
#include "gnss_rtcm3.h"
#include "rtcm_vectors.h"
#include "unity.h"

/* Write n <= 64 bits of v at bit `pos` (bit 0 = MSB of p[0]). */
static void put_bits(uint8_t *p, size_t pos, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++) {
        size_t b = pos + i;
        uint8_t mask = (uint8_t)(0x80u >> (b & 7u));
        if ((v >> (n - 1 - i)) & 1u) {
            p[b >> 3] |= mask;
        } else {
            p[b >> 3] &= (uint8_t)~mask;
        }
    }
}

/* An MSM frame with the given header and every cell present; `cut` bytes
 * are removed from the end of the payload. Returns the frame length. */
static size_t make_msm(uint8_t *frame, uint16_t type, uint16_t station, uint32_t epoch, bool mmb,
                       const uint8_t *sats, unsigned nsats, const uint8_t *sigs, unsigned nsigs, size_t cut)
{
    uint8_t payload[GNSS_RTCM3_MSM_HEADER_BITS / 8 + 64];
    memset(payload, 0, sizeof(payload));
    put_bits(payload, 0, 12, type);
    put_bits(payload, 12, 12, station);
    put_bits(payload, 24, 30, epoch);
    put_bits(payload, 54, 1, mmb ? 1u : 0u);
    put_bits(payload, 55, 3, 5);                       /* IODS */
    for (unsigned i = 0; i < nsats; i++) {
        put_bits(payload, 73u + sats[i] - 1u, 1, 1);
    }
    for (unsigned i = 0; i < nsigs; i++) {
        put_bits(payload, 137u + sigs[i] - 1u, 1, 1);
    }
    unsigned cells = nsats * nsigs;
    for (unsigned k = 0; k < cells && k < 64; k++) {
        put_bits(payload, GNSS_RTCM3_MSM_HEADER_BITS + k, 1, 1);
    }
    size_t bits = GNSS_RTCM3_MSM_HEADER_BITS + (cells <= 64 ? cells : 64);
    size_t n = (bits + 7) / 8 - cut;
    return rtcm_make_frame(frame, payload, n);
}

TEST_CASE("MSM header of the published 1074 frame", "[gnss_rtcm3]")
{
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(k_v1074, sizeof(k_v1074), &h));
    TEST_ASSERT_EQUAL_UINT16(1074, h.msg_type);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, h.sys);
    TEST_ASSERT_EQUAL_UINT8(4, h.msm);
    TEST_ASSERT_EQUAL_UINT16(0, h.station_id);
    TEST_ASSERT_EQUAL_UINT32(270524000u, h.epoch_time);
    TEST_ASSERT_TRUE(h.multiple_message);
    TEST_ASSERT_EQUAL_UINT8(0, h.iods);

    static const uint8_t sats[] = { 10, 14, 16, 25, 26, 29, 31, 32 };
    uint64_t sat_mask = 0;
    for (size_t i = 0; i < sizeof(sats); i++) {
        sat_mask |= (uint64_t)1 << (sats[i] - 1);
    }
    TEST_ASSERT_EQUAL_HEX64(sat_mask, h.sat_mask);
    TEST_ASSERT_EQUAL_UINT8(8, h.num_sats);
    TEST_ASSERT_EQUAL_HEX32((1u << 1) | (1u << 9), h.sig_mask);   /* signals 2 (1C) and 10 (2W) */
    TEST_ASSERT_EQUAL_UINT8(2, h.num_sigs);
    TEST_ASSERT_EQUAL_HEX64(0xFFFF, h.cell_mask);
    TEST_ASSERT_EQUAL_UINT8(16, h.num_cells);
}

TEST_CASE("MSM types are 1071..1137 with digit 1..7, SBAS excluded", "[gnss_rtcm3]")
{
    static const struct {
        uint16_t type;
        gnss_sys_t sys;
        uint8_t msm;
    } yes[] = {
        { 1071, GNSS_SYS_GPS, 1 }, { 1077, GNSS_SYS_GPS, 7 }, { 1084, GNSS_SYS_GLONASS, 4 },
        { 1087, GNSS_SYS_GLONASS, 7 }, { 1097, GNSS_SYS_GALILEO, 7 }, { 1117, GNSS_SYS_QZSS, 7 },
        { 1124, GNSS_SYS_BEIDOU, 4 }, { 1127, GNSS_SYS_BEIDOU, 7 }, { 1137, GNSS_SYS_NAVIC, 7 },
    };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        gnss_sys_t sys = GNSS_SYS_COUNT;
        uint8_t msm = 0;
        TEST_ASSERT_TRUE(gnss_rtcm3_msm_type(yes[i].type, &sys, &msm));
        TEST_ASSERT_EQUAL(yes[i].sys, sys);
        TEST_ASSERT_EQUAL_UINT8(yes[i].msm, msm);
    }
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_type(1077, NULL, NULL));

    static const uint16_t no[] = {
        0, 1005, 1006, 1033, 1070, 1078, 1079, 1080, 1090, 1101, 1104, 1105, 1107,
        1110, 1118, 1130, 1138, 1230, 4074,
    };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        TEST_ASSERT_FALSE(gnss_rtcm3_msm_type(no[i], NULL, NULL));
    }
}

TEST_CASE("MSM decode of constructed headers and its limits", "[gnss_rtcm3]")
{
    uint8_t f[GNSS_RTCM3_MAX_FRAME];
    gnss_rtcm3_msm_t h;

    /* BeiDou MSM7, 16 satellites x 4 signals = 64 cells (the most one MSM holds). */
    static const uint8_t sats[] = { 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 19, 20, 21, 63 };
    static const uint8_t sigs[] = { 2, 8, 14, 22 };
    size_t n = make_msm(f, 1127, 4095, 0x3FFFFFFFu, false, sats, 16, sigs, 4, 0);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(f, n));
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(f, n, &h));
    TEST_ASSERT_EQUAL(GNSS_SYS_BEIDOU, h.sys);
    TEST_ASSERT_EQUAL_UINT8(7, h.msm);
    TEST_ASSERT_EQUAL_UINT16(4095, h.station_id);
    TEST_ASSERT_EQUAL_UINT32(0x3FFFFFFFu, h.epoch_time);
    TEST_ASSERT_FALSE(h.multiple_message);
    TEST_ASSERT_EQUAL_UINT8(5, h.iods);
    TEST_ASSERT_EQUAL_UINT8(16, h.num_sats);
    TEST_ASSERT_TRUE((h.sat_mask >> 62) & 1u);                   /* satellite 63 */
    TEST_ASSERT_EQUAL_UINT8(4, h.num_sigs);
    TEST_ASSERT_EQUAL_UINT8(64, h.num_cells);
    TEST_ASSERT_EQUAL_HEX64(UINT64_MAX, h.cell_mask);

    /* Payload one byte short of the cell mask. */
    n = make_msm(f, 1127, 1, 1, false, sats, 16, sigs, 4, 1);
    memset(&h, 0x5A, sizeof(h));
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(f, n, &h));
    TEST_ASSERT_EQUAL_UINT16(0, h.msg_type);

    /* 9 satellites x 8 signals = 72 cells: not a valid MSM. */
    static const uint8_t sats9[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const uint8_t sigs8[] = { 2, 3, 4, 8, 9, 10, 15, 16 };
    n = make_msm(f, 1077, 1, 1, false, sats9, 9, sigs8, 8, 0);
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(f, n, &h));

    /* An SBAS number and a non-MSM frame are refused. */
    n = make_msm(f, 1104, 1, 1, false, sats, 2, sigs, 2, 0);
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(f, n, &h));
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(k_v1005, sizeof(k_v1005), &h));

    /* Header shorter than 169 bits. */
    n = rtcm_make_frame(f, k_v1074 + 3, 20);
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(f, n, &h));
    /* Frame structure: length field must match. */
    TEST_ASSERT_FALSE(gnss_rtcm3_msm_decode(k_v1074, sizeof(k_v1074) - 1, &h));
}

TEST_CASE("1005 frame decodes to the published antenna reference point", "[gnss_rtcm3]")
{
    gnss_rtcm3_arp_t a;
    TEST_ASSERT_TRUE(gnss_rtcm3_arp_decode(k_v1005, sizeof(k_v1005), &a));
    TEST_ASSERT_EQUAL_UINT16(1005, a.msg_type);
    TEST_ASSERT_EQUAL_UINT16(2003, a.station_id);
    TEST_ASSERT_EQUAL_UINT8(0, a.itrf_year);
    TEST_ASSERT_TRUE(a.gps);
    TEST_ASSERT_FALSE(a.glonass);
    TEST_ASSERT_FALSE(a.galileo);
    TEST_ASSERT_FALSE(a.ref_station);
    TEST_ASSERT_EQUAL_INT64(11141045999LL, a.ecef_01mm[0]);        /* 1114104.5999 m */
    TEST_ASSERT_EQUAL_INT64(-48507297108LL, a.ecef_01mm[1]);       /* -4850729.7108 m */
    TEST_ASSERT_EQUAL_INT64(39755214643LL, a.ecef_01mm[2]);        /* 3975521.4643 m */
    TEST_ASSERT_EQUAL_UINT16(0, a.height_01mm);

    memset(&a, 0x5A, sizeof(a));
    TEST_ASSERT_FALSE(gnss_rtcm3_arp_decode(k_v1074, sizeof(k_v1074), &a));
    TEST_ASSERT_EQUAL_UINT16(0, a.msg_type);
    TEST_ASSERT_FALSE(gnss_rtcm3_arp_decode(k_v1005, sizeof(k_v1005) - 1, &a));
}

TEST_CASE("1006 adds the antenna height", "[gnss_rtcm3]")
{
    uint8_t payload[21];
    memset(payload, 0, sizeof(payload));
    put_bits(payload, 0, 12, 1006);
    put_bits(payload, 12, 12, 77);
    put_bits(payload, 24, 6, 20);
    put_bits(payload, 30, 1, 1);
    put_bits(payload, 31, 1, 1);
    put_bits(payload, 32, 1, 1);
    put_bits(payload, 34, 38, (uint64_t)-12345678901LL & ((1ULL << 38) - 1));
    put_bits(payload, 74, 38, 1);
    put_bits(payload, 114, 38, (uint64_t)-1LL & ((1ULL << 38) - 1));
    put_bits(payload, 152, 16, 15000);
    uint8_t f[32];
    size_t n = rtcm_make_frame(f, payload, sizeof(payload));

    gnss_rtcm3_arp_t a;
    TEST_ASSERT_TRUE(gnss_rtcm3_arp_decode(f, n, &a));
    TEST_ASSERT_EQUAL_UINT16(1006, a.msg_type);
    TEST_ASSERT_EQUAL_UINT16(77, a.station_id);
    TEST_ASSERT_EQUAL_UINT8(20, a.itrf_year);
    TEST_ASSERT_TRUE(a.glonass && a.galileo);
    TEST_ASSERT_EQUAL_INT64(-12345678901LL, a.ecef_01mm[0]);
    TEST_ASSERT_EQUAL_INT64(1, a.ecef_01mm[1]);
    TEST_ASSERT_EQUAL_INT64(-1, a.ecef_01mm[2]);
    TEST_ASSERT_EQUAL_UINT16(15000, a.height_01mm);

    /* 1006 needs 168 bits. */
    n = rtcm_make_frame(f, payload, 20);
    TEST_ASSERT_FALSE(gnss_rtcm3_arp_decode(f, n, &a));
}

TEST_CASE("MSM signal IDs map to band buckets", "[gnss_rtcm3]")
{
    static const struct {
        gnss_sys_t sys;
        unsigned id;
        gnss_band_t band;
    } map[] = {
        { GNSS_SYS_GPS, 2, GNSS_BAND_L1 }, { GNSS_SYS_GPS, 4, GNSS_BAND_L1 },
        { GNSS_SYS_GPS, 10, GNSS_BAND_L2 }, { GNSS_SYS_GPS, 16, GNSS_BAND_L2 },
        { GNSS_SYS_GPS, 24, GNSS_BAND_L5 }, { GNSS_SYS_GPS, 31, GNSS_BAND_L1 },
        { GNSS_SYS_GPS, 5, GNSS_BAND_NONE }, { GNSS_SYS_GPS, 1, GNSS_BAND_NONE },
        { GNSS_SYS_GLONASS, 2, GNSS_BAND_L1 }, { GNSS_SYS_GLONASS, 9, GNSS_BAND_L2 },
        { GNSS_SYS_GLONASS, 4, GNSS_BAND_NONE },
        { GNSS_SYS_GALILEO, 2, GNSS_BAND_L1 }, { GNSS_SYS_GALILEO, 8, GNSS_BAND_L6 },
        { GNSS_SYS_GALILEO, 12, GNSS_BAND_L6 }, { GNSS_SYS_GALILEO, 15, GNSS_BAND_L2 },
        { GNSS_SYS_GALILEO, 19, GNSS_BAND_E5AB }, { GNSS_SYS_GALILEO, 23, GNSS_BAND_L5 },
        { GNSS_SYS_GALILEO, 13, GNSS_BAND_NONE },
        { GNSS_SYS_QZSS, 2, GNSS_BAND_L1 }, { GNSS_SYS_QZSS, 10, GNSS_BAND_L6 },
        { GNSS_SYS_QZSS, 16, GNSS_BAND_L2 }, { GNSS_SYS_QZSS, 22, GNSS_BAND_L5 },
        { GNSS_SYS_QZSS, 32, GNSS_BAND_L1 },
        { GNSS_SYS_BEIDOU, 2, GNSS_BAND_L1 }, { GNSS_SYS_BEIDOU, 9, GNSS_BAND_L6 },
        { GNSS_SYS_BEIDOU, 14, GNSS_BAND_L2 }, { GNSS_SYS_BEIDOU, 25, GNSS_BAND_L2 },
        { GNSS_SYS_BEIDOU, 23, GNSS_BAND_L5 }, { GNSS_SYS_BEIDOU, 30, GNSS_BAND_L1 },
        { GNSS_SYS_NAVIC, 22, GNSS_BAND_NONE }, { GNSS_SYS_SBAS, 2, GNSS_BAND_NONE },
        { GNSS_SYS_GPS, 0, GNSS_BAND_NONE }, { GNSS_SYS_GPS, 33, GNSS_BAND_NONE },
        { GNSS_SYS_COUNT, 2, GNSS_BAND_NONE },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(map[i].band, gnss_rtcm3_msm_band(map[i].sys, map[i].id), "band");
    }

    /* The published 1074: L1 + L2. */
    TEST_ASSERT_EQUAL_HEX8((1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L2),
                           gnss_rtcm3_msm_band_mask(GNSS_SYS_GPS, (1u << 1) | (1u << 9)));
    /* Galileo E1 + E5a + E5b + E6 (signal group with E6): four bands. */
    uint32_t gal = (1u << (2 - 1)) | (1u << (22 - 1)) | (1u << (14 - 1)) | (1u << (8 - 1));
    TEST_ASSERT_EQUAL_HEX8(0x0F, gnss_rtcm3_msm_band_mask(GNSS_SYS_GALILEO, gal));
    /* Two signals in one band count once. */
    TEST_ASSERT_EQUAL_HEX8(1u << GNSS_BAND_L1,
                           gnss_rtcm3_msm_band_mask(GNSS_SYS_BEIDOU, (1u << 1) | (1u << 31)));
    TEST_ASSERT_EQUAL_HEX8(0, gnss_rtcm3_msm_band_mask(GNSS_SYS_NAVIC, UINT32_MAX));
}

/* ---- MSM epoch time on the GPS time scale ---------------------------------------- */

#define WEEK_MS GNSS_RTCM3_WEEK_MS
#define DAY_MS  GNSS_RTCM3_DAY_MS
#define HOUR_MS 3600000LL

/* GPS week 2400 starts on 2026-01-04 (Sunday) at 00:00:00 GPS time. */
#define W2400 (2400LL * WEEK_MS)

TEST_CASE("MSM epoch to GPS time: GPS time of week near the week wrap", "[gnss_rtcm3]")
{
    int64_t out = 0;
    /* An epoch from the end of week 2400, read just after the wrap: it stays in week 2400. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, 604799500u, W2400 + WEEK_MS + 300, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + 604799500LL, out);
    /* An epoch from the start of week 2401, read just before the wrap (a fast clock): week 2401. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, 200u, W2400 + 604799900LL, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + WEEK_MS + 200, out);
    /* Mid-week: the same week. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, 270524000u, W2400 + 270524850LL, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + 270524000LL, out);
    /* Out of range, no clock, no MSM. */
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, (uint32_t)WEEK_MS, W2400, &out));
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, 1000u, 0, &out));
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_SBAS, 1000u, W2400, &out));
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_COUNT, 1000u, W2400, &out));
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, 1000u, W2400, NULL));
}

TEST_CASE("MSM epoch to GPS time: Galileo, QZSS and NavIC count as GPS", "[gnss_rtcm3]")
{
    const gnss_sys_t same[] = { GNSS_SYS_GALILEO, GNSS_SYS_QZSS, GNSS_SYS_NAVIC };
    const uint32_t fields[] = { 0u, 1000u, 270524000u, 604799999u };
    for (size_t s = 0; s < sizeof(same) / sizeof(same[0]); s++) {
        for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            int64_t now = W2400 + fields[f] + 700;
            int64_t gps = 0;
            int64_t other = 0;
            TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GPS, fields[f], now, &gps));
            TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(same[s], fields[f], now, &other));
            TEST_ASSERT_EQUAL_INT64(gps, other);
        }
    }
}

TEST_CASE("MSM epoch to GPS time: BeiDou time is 14 s behind GPS", "[gnss_rtcm3]")
{
    int64_t out = 0;
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_BEIDOU, 1000u, W2400 + 15500, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + 15000, out);
    /* The BeiDou week ends 14 s after the GPS week: its last seconds fall into the next GPS week. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_BEIDOU, 604790000u, W2400 + WEEK_MS + 4300, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + WEEK_MS + 4000, out);
    /* BeiDou second 0 of a week is GPS second 14 of it. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_BEIDOU, 0u, W2400 + 14200, &out));
    TEST_ASSERT_EQUAL_INT64(W2400 + 14000, out);
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_BEIDOU, (uint32_t)WEEK_MS, W2400, &out));
}

/* GLONASS epoch field: day of week (0 = Sunday) and ms of the day in Moscow time. */
static uint32_t glo_field(unsigned dow, int64_t tod_ms)
{
    return ((uint32_t)dow << 27) | (uint32_t)tod_ms;
}

TEST_CASE("MSM epoch to GPS time: GLONASS around 21:00 UTC and the Moscow day change", "[gnss_rtcm3]")
{
    int64_t out = 0;
    /* Monday 20:59:59 UTC = Monday 23:59:59 Moscow: GPS Monday 21:00:17. */
    int64_t mon_2059_59_utc = W2400 + 1 * DAY_MS + 20 * HOUR_MS + 59 * 60000 + 59000;
    int64_t gps = mon_2059_59_utc + GNSS_RTCM3_GPS_UTC_LEAP_S * 1000;
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GLONASS, glo_field(1, 23 * HOUR_MS + 59 * 60000 + 59000),
                                             gps + 400, &out));
    TEST_ASSERT_EQUAL_INT64(gps, out);
    /* Two seconds later Moscow is on Tuesday: day 2, 1 s into the day. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GLONASS, glo_field(2, 1000), gps + 2400, &out));
    TEST_ASSERT_EQUAL_INT64(gps + 2000, out);
    /* Sunday 01:00 Moscow is Saturday 22:00 UTC: the GPS week before. */
    int64_t sat_2200_gps = W2400 - 2 * HOUR_MS + GNSS_RTCM3_GPS_UTC_LEAP_S * 1000;
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GLONASS, glo_field(0, HOUR_MS), sat_2200_gps + 900, &out));
    TEST_ASSERT_EQUAL_INT64(sat_2200_gps, out);
    /* Day unknown (7): the nearest day. */
    TEST_ASSERT_TRUE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GLONASS, glo_field(7, 1000), gps + 2400, &out));
    TEST_ASSERT_EQUAL_INT64(gps + 2000, out);
    /* A time of day past the day (and its leap second) is not a GLONASS time. */
    TEST_ASSERT_FALSE(gnss_rtcm3_epoch_gps_ms(GNSS_SYS_GLONASS, glo_field(2, DAY_MS + 1000), gps, &out));
}

TEST_CASE("MSM epoch to GPS time: the leap seconds and the GPS epoch agree with the ledger's", "[gnss_rtcm3]")
{
    TEST_ASSERT_EQUAL(18, GNSS_RTCM3_GPS_UTC_LEAP_S);
    TEST_ASSERT_EQUAL_INT64(315964800000LL, GNSS_RTCM3_GPS_EPOCH_UNIX_MS);
}
