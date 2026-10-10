/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation decoders: per-cell and per-band CNR of MSM4..MSM7 (the
 * published 1074 frame and frames built per the RTCM layout), and
 * satellite-in-view sentences (talkers, numbering, empty fields, signal ID,
 * malformed lines).
 */
#include <stdio.h>
#include <string.h>

#include "gnss_obs_parse.h"
#include "gnss_rtcm3.h"
#include "msm_test_builder.h"
#include "rtcm_vectors.h"
#include "unity.h"

static uint8_t s_frame[GNSS_RTCM3_MAX_FRAME];

/* ---- MSM cells ------------------------------------------------------------- */

TEST_CASE("obs parse: CNR of the published 1074 frame", "[gnss_obs]")
{
    /* Satellites 10 14 16 25 26 29 31 32 with 1C and 2W each, in sent order. */
    static const uint8_t k_dbhz[16] = { 43, 41, 48, 48, 44, 39, 43, 43, 50, 49, 46, 39, 51, 48, 50, 49 };
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(k_v1074, sizeof(k_v1074), &h));
    obs_cnr_t cnr;
    uint16_t cell[64];
    TEST_ASSERT_TRUE(obs_msm_cnr(k_v1074, sizeof(k_v1074), &h, &cnr, cell));
    for (unsigned k = 0; k < 16; k++) {
        TEST_ASSERT_EQUAL_UINT16(k_dbhz[k] * 16u, cell[k]);   /* MSM4: whole dB-Hz */
    }
    TEST_ASSERT_EQUAL_UINT16(8, cnr.cells[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT32(375u * 16u, cnr.sum_q4[GNSS_BAND_L1]);   /* 1C: mean 46.9 dB-Hz */
    TEST_ASSERT_EQUAL_UINT16(8, cnr.cells[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT32(356u * 16u, cnr.sum_q4[GNSS_BAND_L2]);   /* 2W: mean 44.5 dB-Hz */
    TEST_ASSERT_EQUAL_UINT16(0, cnr.cells[GNSS_BAND_L5]);
}

TEST_CASE("obs parse: MSM7 cells with absent cells, per band", "[gnss_obs]")
{
    /* Galileo E1 (2), E6 (9), E5b (15), E5a (23) on three satellites; satellite
     * 11 has no E6 cell. CNR in 1/16 dB-Hz. */
    static const uint8_t sats[] = { 3, 11, 30 };
    static const uint8_t sigs[] = { 2, 9, 15, 23 };
    static const uint16_t cnr_raw[11] = {
        45 * 16 + 8, 40 * 16, 44 * 16, 47 * 16,       /* sat 3 */
        38 * 16 + 4, 0, 41 * 16 + 12,                 /* sat 11: E1, E5b (CNR n/a), E5a */
        50 * 16, 42 * 16 + 2, 46 * 16, 48 * 16,       /* sat 30 */
    };
    msm_desc_t sp = {
        .type = 1097, .station = 77, .epoch = 123456789, .more = false,
        .sats = sats, .nsats = 3, .sigs = sigs, .nsigs = 4,
        .absent = 1ull << (1 * 4 + 1), .cnr = cnr_raw,
    };
    size_t len = msm_build(s_frame, &sp);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(s_frame, len));
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(s_frame, len, &h));
    TEST_ASSERT_EQUAL_UINT8(11, h.num_cells);

    obs_cnr_t cnr;
    uint16_t cell[64];
    TEST_ASSERT_TRUE(obs_msm_cnr(s_frame, len, &h, &cnr, cell));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(cnr_raw, cell, 11);

    TEST_ASSERT_EQUAL_UINT16(3, cnr.cells[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT32((45 * 16 + 8) + (38 * 16 + 4) + 50 * 16, cnr.sum_q4[GNSS_BAND_L1]);
    TEST_ASSERT_EQUAL_UINT16(2, cnr.cells[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT32(40 * 16 + 42 * 16 + 2, cnr.sum_q4[GNSS_BAND_L6]);
    TEST_ASSERT_EQUAL_UINT16(2, cnr.cells[GNSS_BAND_L2]);              /* the n/a cell is left out */
    TEST_ASSERT_EQUAL_UINT32(44 * 16 + 46 * 16, cnr.sum_q4[GNSS_BAND_L2]);
    TEST_ASSERT_EQUAL_UINT16(3, cnr.cells[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT32(47 * 16 + 41 * 16 + 12 + 48 * 16, cnr.sum_q4[GNSS_BAND_L5]);
    TEST_ASSERT_EQUAL_UINT16(0, cnr.cells[GNSS_BAND_E5AB]);
}

TEST_CASE("obs parse: MSM4, MSM5 and MSM6 layouts and units", "[gnss_obs]")
{
    static const uint8_t sats[] = { 1, 7, 12, 31 };
    static const uint8_t sigs[] = { 2, 15 };            /* GPS 1C, 2S */
    static const uint16_t lo[8] = { 40, 33, 47, 30, 52, 44, 1, 63 };                    /* 6-bit, dB-Hz */
    static const uint16_t hi[8] = { 640, 535, 760, 481, 833, 704, 17, 1023 };          /* 10-bit, 1/16 */
    for (uint16_t type = 1074; type <= 1076; type++) {
        const bool fine = (type == 1076);
        msm_desc_t sp = {
            .type = type, .epoch = 1000, .sats = sats, .nsats = 4, .sigs = sigs, .nsigs = 2,
            .cnr = fine ? hi : lo,
        };
        size_t len = msm_build(s_frame, &sp);
        gnss_rtcm3_msm_t h;
        TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(s_frame, len, &h));
        obs_cnr_t cnr;
        uint16_t cell[64];
        TEST_ASSERT_TRUE_MESSAGE(obs_msm_cnr(s_frame, len, &h, &cnr, cell), "decode");
        for (unsigned k = 0; k < 8; k++) {
            TEST_ASSERT_EQUAL_UINT16(fine ? hi[k] : lo[k] * 16u, cell[k]);
        }
        TEST_ASSERT_EQUAL_UINT16(4, cnr.cells[GNSS_BAND_L1]);
        TEST_ASSERT_EQUAL_UINT16(4, cnr.cells[GNSS_BAND_L2]);
    }
}

TEST_CASE("obs parse: no CNR from MSM1-3, short frames or a foreign header", "[gnss_obs]")
{
    static const uint8_t sats[] = { 5, 6 };
    static const uint8_t sigs[] = { 2 };
    static const uint16_t raw[2] = { 700, 720 };
    msm_desc_t sp = { .type = 1127, .epoch = 5, .sats = sats, .nsats = 2, .sigs = sigs, .nsigs = 1, .cnr = raw };
    size_t len = msm_build(s_frame, &sp);
    gnss_rtcm3_msm_t h;
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(s_frame, len, &h));
    obs_cnr_t cnr;
    TEST_ASSERT_TRUE(obs_msm_cnr(s_frame, len, &h, &cnr, NULL));
    TEST_ASSERT_EQUAL_UINT32(1420, cnr.sum_q4[GNSS_BAND_L1]);

    /* The same header claimed by another MSM number: rejected. */
    gnss_rtcm3_msm_t other = h;
    other.msg_type = 1124;
    other.msm = 4;
    TEST_ASSERT_FALSE(obs_msm_cnr(s_frame, len, &other, &cnr, NULL));

    /* MSM3 carries no CNR. */
    sp.type = 1123;
    len = msm_build(s_frame, &sp);
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(s_frame, len, &h));
    TEST_ASSERT_FALSE(obs_msm_cnr(s_frame, len, &h, &cnr, NULL));

    /* A frame cut short of its cells (rebuilt with a valid CRC). */
    sp.type = 1127;
    len = msm_build(s_frame, &sp);
    uint8_t cut[GNSS_RTCM3_MAX_FRAME];
    size_t n = len - 6 - 3;
    size_t cut_len = rtcm_make_frame(cut, s_frame + 3, n);
    TEST_ASSERT_TRUE(gnss_rtcm3_msm_decode(cut, cut_len, &h));       /* the header still decodes */
    TEST_ASSERT_FALSE(obs_msm_cnr(cut, cut_len, &h, &cnr, NULL));
    TEST_ASSERT_EQUAL_UINT16(0, cnr.cells[GNSS_BAND_L1]);

    /* Length field not matching the buffer. */
    TEST_ASSERT_FALSE(obs_msm_cnr(s_frame, len - 1, &h, &cnr, NULL));
}

/* ---- satellite-in-view sentences ------------------------------------------- */

/* A sentence with its NMEA checksum (the framer would have verified it). */
static size_t nmea(char *out, size_t cap, const char *body)
{
    unsigned x = 0;
    for (const char *p = body + 1; *p != '\0'; p++) {
        x ^= (unsigned char)*p;
    }
    int n = snprintf(out, cap, "%s*%02X", body, x);
    return (n > 0) ? (size_t)n : 0;
}

static bool parse(const char *body, obs_gsv_t *g)
{
    char line[160];
    size_t n = nmea(line, sizeof(line), body);
    return obs_gsv_parse(line, n, g);
}

TEST_CASE("obs parse: GPS satellite-in-view sentence", "[gnss_obs]")
{
    obs_gsv_t g;
    TEST_ASSERT_TRUE(parse("$GPGSV,3,1,11,10,63,137,17,07,61,098,15,05,59,290,20,08,54,157,30", &g));
    TEST_ASSERT_EQUAL_UINT8(3, g.total);
    TEST_ASSERT_EQUAL_UINT8(1, g.number);
    TEST_ASSERT_EQUAL_UINT8(11, g.in_view);
    TEST_ASSERT_EQUAL_UINT8(0, g.signal_id);
    TEST_ASSERT_EQUAL_UINT8(4, g.count);
    TEST_ASSERT_EQUAL(GNSS_SYS_GPS, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(10, g.sat[0].sat);
    TEST_ASSERT_TRUE(g.sat[0].has_pos);
    TEST_ASSERT_EQUAL_INT8(63, g.sat[0].elevation_deg);
    TEST_ASSERT_EQUAL_UINT16(137, g.sat[0].azimuth_deg);
    TEST_ASSERT_EQUAL_UINT8(17, g.sat[0].cnr_dbhz);
    TEST_ASSERT_EQUAL_UINT8(8, g.sat[3].sat);
    TEST_ASSERT_EQUAL_UINT8(30, g.sat[3].cnr_dbhz);

    /* Last sentence of a set: fewer blocks, an untracked satellite (empty
     * SNR), one without a position, and the signal ID of NMEA 4.10. */
    TEST_ASSERT_TRUE(parse("$GPGSV,3,3,11,22,42,067,42,24,14,311,,27,,,35,1", &g));
    TEST_ASSERT_EQUAL_UINT8(1, g.signal_id);
    TEST_ASSERT_EQUAL_UINT8(3, g.count);
    TEST_ASSERT_EQUAL_UINT8(42, g.sat[0].cnr_dbhz);
    TEST_ASSERT_TRUE(g.sat[1].has_pos);
    TEST_ASSERT_EQUAL_UINT8(0, g.sat[1].cnr_dbhz);
    TEST_ASSERT_FALSE(g.sat[2].has_pos);
    TEST_ASSERT_EQUAL_UINT8(35, g.sat[2].cnr_dbhz);

    /* Nothing in view. */
    TEST_ASSERT_TRUE(parse("$GPGSV,1,1,00,1", &g));
    TEST_ASSERT_EQUAL_UINT8(0, g.count);
    TEST_ASSERT_EQUAL_UINT8(1, g.signal_id);
    TEST_ASSERT_TRUE(parse("$GPGSV,1,1,00", &g));
    TEST_ASSERT_EQUAL_UINT8(0, g.count);
}

TEST_CASE("obs parse: talkers and satellite numbering", "[gnss_obs]")
{
    obs_gsv_t g;
    TEST_ASSERT_TRUE(parse("$GLGSV,1,1,03,65,30,100,40,88,10,200,,05,45,300,38", &g));
    TEST_ASSERT_EQUAL_UINT8(3, g.count);
    TEST_ASSERT_EQUAL(GNSS_SYS_GLONASS, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(1, g.sat[0].sat);                 /* 65 = slot 1 */
    TEST_ASSERT_EQUAL_UINT8(24, g.sat[1].sat);
    TEST_ASSERT_EQUAL_UINT8(5, g.sat[2].sat);                 /* slot numbering as sent */

    TEST_ASSERT_TRUE(parse("$GAGSV,1,1,02,04,51,180,44,301,20,020,39,7", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(4, g.sat[0].sat);
    TEST_ASSERT_EQUAL_UINT8(1, g.sat[1].sat);
    TEST_ASSERT_EQUAL_UINT8(7, g.signal_id);

    TEST_ASSERT_TRUE(parse("$GBGSV,1,1,02,19,70,010,45,223,05,350,", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_BEIDOU, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(19, g.sat[0].sat);
    TEST_ASSERT_EQUAL_UINT8(23, g.sat[1].sat);
    TEST_ASSERT_TRUE(parse("$BDGSV,1,1,01,60,08,120,30", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_BEIDOU, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(60, g.sat[0].sat);

    TEST_ASSERT_TRUE(parse("$GQGSV,1,1,02,02,60,170,41,195,20,140,33", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_QZSS, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(2, g.sat[0].sat);
    TEST_ASSERT_EQUAL_UINT8(3, g.sat[1].sat);                 /* PRN 195 */

    TEST_ASSERT_TRUE(parse("$GIGSV,1,1,01,05,40,100,36", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_NAVIC, g.sat[0].sys);

    /* GP/GN by number range: SBAS, GLONASS, QZSS, Galileo, BeiDou. */
    TEST_ASSERT_TRUE(parse("$GNGSV,2,1,08,46,30,200,40,133,31,201,41,70,10,010,35,194,50,190,44", &g));
    TEST_ASSERT_EQUAL_UINT8(4, g.count);
    TEST_ASSERT_EQUAL(GNSS_SYS_SBAS, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(14, g.sat[0].sat);                /* 46 = PRN 133 */
    TEST_ASSERT_EQUAL(GNSS_SYS_SBAS, g.sat[1].sys);
    TEST_ASSERT_EQUAL_UINT8(14, g.sat[1].sat);                /* PRN 133 */
    TEST_ASSERT_EQUAL(GNSS_SYS_GLONASS, g.sat[2].sys);
    TEST_ASSERT_EQUAL_UINT8(6, g.sat[2].sat);
    TEST_ASSERT_EQUAL(GNSS_SYS_QZSS, g.sat[3].sys);
    TEST_ASSERT_EQUAL_UINT8(2, g.sat[3].sat);
    TEST_ASSERT_TRUE(parse("$GNGSV,2,2,08,305,20,100,30,412,30,250,40,250,10,10,20,0,10,10,20", &g));
    TEST_ASSERT_EQUAL_UINT8(2, g.count);                      /* 250 and 0 cannot be placed */
    TEST_ASSERT_EQUAL(GNSS_SYS_GALILEO, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(5, g.sat[0].sat);
    TEST_ASSERT_EQUAL(GNSS_SYS_BEIDOU, g.sat[1].sys);
    TEST_ASSERT_EQUAL_UINT8(12, g.sat[1].sat);
}

TEST_CASE("obs parse: values out of range and malformed sentences", "[gnss_obs]")
{
    obs_gsv_t g;
    /* Out-of-range values count as empty; azimuth 360 is north. */
    TEST_ASSERT_TRUE(parse("$GPGSV,1,1,03,01,91,100,40,02,-5,360,0,03,10,400,120", &g));
    TEST_ASSERT_EQUAL_UINT8(3, g.count);
    TEST_ASSERT_FALSE(g.sat[0].has_pos);
    TEST_ASSERT_TRUE(g.sat[1].has_pos);
    TEST_ASSERT_EQUAL_INT8(-5, g.sat[1].elevation_deg);
    TEST_ASSERT_EQUAL_UINT16(0, g.sat[1].azimuth_deg);
    TEST_ASSERT_EQUAL_UINT8(0, g.sat[1].cnr_dbhz);
    TEST_ASSERT_FALSE(g.sat[2].has_pos);
    TEST_ASSERT_EQUAL_UINT8(0, g.sat[2].cnr_dbhz);

    /* Fractions are accepted and dropped. */
    TEST_ASSERT_TRUE(parse("$GPGSV,1,1,01,09,45.5,120.9,38.0", &g));
    TEST_ASSERT_EQUAL_INT8(45, g.sat[0].elevation_deg);
    TEST_ASSERT_EQUAL_UINT16(120, g.sat[0].azimuth_deg);
    TEST_ASSERT_EQUAL_UINT8(38, g.sat[0].cnr_dbhz);

    TEST_ASSERT_FALSE(parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", &g));
    TEST_ASSERT_FALSE(parse("$XXGSV,1,1,01,09,45,120,38", &g));               /* unknown talker */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,1", &g));                               /* header cut */
    TEST_ASSERT_FALSE(parse("$GPGSV,,1,01,09,45,120,38", &g));                /* no sentence count */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,2,01,09,45,120,38", &g));               /* number beyond total */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,1,01,09,45,120", &g));                  /* block cut */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,1,01,09,4x,120,38", &g));               /* not a number */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,1,01,09,45,120,38,Z", &g));             /* bad signal ID */
    TEST_ASSERT_FALSE(parse("$GPGSV,1,1,05,01,1,1,1,02,1,1,1,03,1,1,1,04,1,1,1,05,1,1,1", &g));   /* 5 blocks */
    TEST_ASSERT_FALSE(obs_gsv_parse("$GPGSV", 6, &g));
    TEST_ASSERT_FALSE(obs_gsv_parse("#MODE,BASE", 10, &g));
    TEST_ASSERT_EQUAL_UINT8(0, g.count);
}

/* ---- antenna and receiver descriptor (1033) ------------------------------- */

typedef struct {
    uint8_t payload[GNSS_RTCM3_MAX_PAYLOAD];
    size_t pos;
} bits_t;

static void put_count_str(bits_t *b, unsigned count, const char *s)
{
    msm_put(b->payload, b->pos, 8, count);
    b->pos += 8;
    for (unsigned i = 0; i < count; i++) {
        msm_put(b->payload, b->pos, 8, (uint8_t)s[i]);
        b->pos += 8;
    }
}

static void put_str(bits_t *b, const char *s)
{
    put_count_str(b, (unsigned)strlen(s), s);
}

static bits_t s_bits;

/* A 1033 per the public layout; trim cuts that many bytes off the payload. */
static size_t build_1033(uint16_t station, const char *ant, uint8_t setup, const char *aser, const char *rtype,
                         const char *rfw, const char *rser, size_t trim)
{
    memset(&s_bits, 0, sizeof(s_bits));
    msm_put(s_bits.payload, 0, 12, 1033);
    msm_put(s_bits.payload, 12, 12, station);
    s_bits.pos = 24;
    put_str(&s_bits, ant);
    msm_put(s_bits.payload, s_bits.pos, 8, setup);
    s_bits.pos += 8;
    put_str(&s_bits, aser);
    put_str(&s_bits, rtype);
    put_str(&s_bits, rfw);
    put_str(&s_bits, rser);
    size_t n = (s_bits.pos + 7u) / 8u;
    return rtcm_make_frame(s_frame, s_bits.payload, n - trim);
}

TEST_CASE("obs parse: 1033 antenna and receiver descriptor", "[gnss_obs]")
{
    obs_1033_t d;
    size_t len = build_1033(1234, "TRM59800.00     SCIS", 3, "5000112233", "UNICORE UM982", "R4.10Build18975",
                            "1234567", 0);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(s_frame, len));
    TEST_ASSERT_TRUE(obs_1033_parse(s_frame, len, &d));
    TEST_ASSERT_EQUAL_UINT16(1234, d.station_id);
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     SCIS", d.antenna);
    TEST_ASSERT_EQUAL_UINT8(3, d.setup_id);
    TEST_ASSERT_EQUAL_STRING("5000112233", d.antenna_serial);
    TEST_ASSERT_EQUAL_STRING("UNICORE UM982", d.receiver_type);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", d.receiver_fw);
    TEST_ASSERT_EQUAL_STRING("1234567", d.receiver_serial);

    /* Empty strings everywhere. */
    len = build_1033(0, "", 0, "", "", "", "", 0);
    TEST_ASSERT_TRUE(obs_1033_parse(s_frame, len, &d));
    TEST_ASSERT_EQUAL_STRING("", d.antenna);
    TEST_ASSERT_EQUAL_STRING("", d.receiver_serial);

    /* 31 characters in every string. */
    static const char k31[] = "1234567890123456789012345678901";
    len = build_1033(4095, k31, 255, k31, k31, k31, k31, 0);
    TEST_ASSERT_TRUE(obs_1033_parse(s_frame, len, &d));
    TEST_ASSERT_EQUAL_UINT16(4095, d.station_id);
    TEST_ASSERT_EQUAL_STRING(k31, d.antenna);
    TEST_ASSERT_EQUAL_STRING(k31, d.receiver_serial);
    TEST_ASSERT_EQUAL_UINT8(255, d.setup_id);

    /* Characters outside printable ASCII are shown as '?'. */
    len = build_1033(1, "A\x01" "B", 0, "", "", "\xE9", "", 0);
    TEST_ASSERT_TRUE(obs_1033_parse(s_frame, len, &d));
    TEST_ASSERT_EQUAL_STRING("A?B", d.antenna);
    TEST_ASSERT_EQUAL_STRING("?", d.receiver_fw);
}

TEST_CASE("obs parse: 1033 frames that overrun or are not 1033 are refused", "[gnss_obs]")
{
    obs_1033_t d;
    /* A string that ends beyond the payload. */
    size_t len = build_1033(1, "ANT", 0, "SER", "TYPE", "FW", "SERIAL", 3);
    TEST_ASSERT_FALSE(obs_1033_parse(s_frame, len, &d));
    TEST_ASSERT_EQUAL_STRING("", d.antenna);   /* cleared */
    /* Cut inside the setup ID. */
    len = build_1033(1, "ANT", 0, "", "", "", "", 5);
    TEST_ASSERT_FALSE(obs_1033_parse(s_frame, len, &d));

    /* A count over 31. */
    memset(&s_bits, 0, sizeof(s_bits));
    msm_put(s_bits.payload, 0, 12, 1033);
    s_bits.pos = 24;
    static const char k32[] = "12345678901234567890123456789012";
    put_count_str(&s_bits, 32, k32);
    msm_put(s_bits.payload, s_bits.pos, 8, 0);
    s_bits.pos += 8;
    for (int i = 0; i < 4; i++) {
        put_str(&s_bits, "");
    }
    len = rtcm_make_frame(s_frame, s_bits.payload, (s_bits.pos + 7u) / 8u);
    TEST_ASSERT_FALSE(obs_1033_parse(s_frame, len, &d));

    /* Another message, a length that does not match, no frame. */
    TEST_ASSERT_FALSE(obs_1033_parse(k_v1005, sizeof(k_v1005), &d));
    len = build_1033(1, "ANT", 0, "", "", "", "", 0);
    TEST_ASSERT_FALSE(obs_1033_parse(s_frame, len - 1, &d));
    TEST_ASSERT_FALSE(obs_1033_parse(NULL, 0, &d));
}
