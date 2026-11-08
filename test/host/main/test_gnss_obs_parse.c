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

    /* The receiver's BeiDou signal IDs above 9 are one hex digit (B2I = B,
     * B2Q = C), and its QZSS talker is GQ with numbers 1..10. */
    TEST_ASSERT_TRUE(parse("$GBGSV,2,1,05,21,45,120,41,08,12,300,33,36,61,055,47,42,22,190,38,B", &g));
    TEST_ASSERT_EQUAL_UINT8(0x0B, g.signal_id);
    TEST_ASSERT_EQUAL_UINT8(4, g.count);
    TEST_ASSERT_EQUAL_UINT8(42, g.sat[3].sat);
    TEST_ASSERT_TRUE(parse("$GBGSV,2,2,05,59,05,150,,C", &g));
    TEST_ASSERT_EQUAL_UINT8(0x0C, g.signal_id);
    TEST_ASSERT_EQUAL_UINT8(1, g.count);
    TEST_ASSERT_EQUAL_UINT8(0, g.sat[0].cnr_dbhz);
    TEST_ASSERT_TRUE(parse("$GQGSV,1,1,01,03,55,140,44,8", &g));
    TEST_ASSERT_EQUAL(GNSS_SYS_QZSS, g.sat[0].sys);
    TEST_ASSERT_EQUAL_UINT8(3, g.sat[0].sat);
    TEST_ASSERT_EQUAL_UINT8(8, g.signal_id);

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

TEST_CASE("obs parse: position sentences give the fix quality and the satellites used", "[gnss_obs]")
{
    obs_gga_t f;
    /* No antenna: no fix, no satellites (the form a receiver sends without sky). */
    static const char none[] = "$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48";
    TEST_ASSERT_TRUE(obs_gga_parse(none, strlen(none), &f));
    TEST_ASSERT_EQUAL_UINT8(0, f.quality);
    TEST_ASSERT_EQUAL_UINT8(0, f.satellites);
    /* The textbook example: a fix with 8 satellites; another talker. */
    static const char fix[] = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";
    TEST_ASSERT_TRUE(obs_gga_parse(fix, strlen(fix), &f));
    TEST_ASSERT_EQUAL_UINT8(1, f.quality);
    TEST_ASSERT_EQUAL_UINT8(8, f.satellites);
    static const char gn[] = "$GNGGA,123519,4807.038,N,01131.000,E,4,23,0.6,545.4,M,46.9,M,1.0,0000*5B";
    TEST_ASSERT_TRUE(obs_gga_parse(gn, strlen(gn), &f));
    TEST_ASSERT_EQUAL_UINT8(4, f.quality);
    TEST_ASSERT_EQUAL_UINT8(23, f.satellites);
    /* Not a position sentence, or without its quality and count. */
    static const char *const bad[] = {
        "$GPGSV,1,1,00,,,,,*55", "$GPGGA,000000.00,,,,,,,99.99,,,,,,*48", "$GPGGA,000000.00,,,,,0", "$GPGGA",
        "$XXGGA,000000.00,,,,,0,00,99.99,,,,,,*48", "#MODE,BASE", "$GPGGA,000000.00,,,,,0,x0,99.99,,,,,,*48",
        "$GPGGA,000000.00,,,,,12,00,99.99,,,,,,*48",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(obs_gga_parse(bad[i], strlen(bad[i]), &f), bad[i]);
        TEST_ASSERT_EQUAL_UINT8(0, f.satellites);
    }
}

/* ---- position and error statistics, field by field ------------------------- */

TEST_CASE("obs parse: an RTK-fixed GGA of a real capture, every field", "[gnss_obs]")
{
    /* test/fixtures/nmea/gpsd_trimble_nmea-rtk.log (gpsd, a Trimble receiver):
     * 8 minute decimals, a negative geoidal separation, the age of the
     * differential data and the reference station. */
    static const char l[] =
        "$GNGGA,133859.80,4134.50180205,N,09345.03586649,W,4,19,0.7,280.827,M,-31.442,M,5.8,0002*6D";
    obs_gga_t g;
    TEST_ASSERT_TRUE(obs_gga_parse(l, strlen(l), &g));
    TEST_ASSERT_EQUAL_STRING("GN", g.talker);
    TEST_ASSERT_EQUAL_UINT8(4, g.quality);
    TEST_ASSERT_EQUAL_UINT8(19, g.satellites);
    TEST_ASSERT_TRUE(g.utc_valid);
    TEST_ASSERT_EQUAL_UINT32(((13u * 60u + 38u) * 60u + 59u) * 1000u + 800u, g.utc_ms);
    TEST_ASSERT_TRUE(g.position_valid);
    TEST_ASSERT_EQUAL_INT64(41575030034LL, g.lat_ndeg);    /* 41 + 34.50180205 / 60 */
    TEST_ASSERT_EQUAL_INT64(-93750597775LL, g.lon_ndeg);   /* west */
    TEST_ASSERT_TRUE(g.hdop_valid);
    TEST_ASSERT_EQUAL_UINT16(70, g.hdop_c);
    TEST_ASSERT_TRUE(g.altitude_valid);
    TEST_ASSERT_EQUAL_INT32(2808270, g.altitude_01mm);
    TEST_ASSERT_TRUE(g.geoid_valid);
    TEST_ASSERT_EQUAL_INT32(-314420, g.geoid_01mm);
    TEST_ASSERT_TRUE(g.diff_age_valid);
    TEST_ASSERT_EQUAL_UINT32(5800, g.diff_age_ms);
    TEST_ASSERT_TRUE(g.diff_station_valid);
    TEST_ASSERT_EQUAL_UINT16(2, g.diff_station);
}

TEST_CASE("obs parse: GGA without a fix, empty fields and absent values are never zeros", "[gnss_obs]")
{
    obs_gga_t g;
    /* No sky (the form the station's own receivers send without an antenna). */
    static const char none[] = "$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48";
    TEST_ASSERT_TRUE(obs_gga_parse(none, strlen(none), &g));
    TEST_ASSERT_TRUE(g.utc_valid);
    TEST_ASSERT_EQUAL_UINT32(0, g.utc_ms);
    TEST_ASSERT_FALSE(g.position_valid);
    TEST_ASSERT_TRUE(g.hdop_valid);
    TEST_ASSERT_EQUAL_UINT16(9999, g.hdop_c);
    TEST_ASSERT_FALSE(g.altitude_valid);
    TEST_ASSERT_FALSE(g.geoid_valid);
    TEST_ASSERT_FALSE(g.diff_age_valid);
    TEST_ASSERT_FALSE(g.diff_station_valid);

    /* SYNTHETIC: quality 0 with a position in the fields: no position, no heights. */
    static const char stale[] = "$GPGGA,123519,4807.038,N,01131.000,E,0,08,0.9,545.4,M,46.9,M,,";
    TEST_ASSERT_TRUE(obs_gga_parse(stale, strlen(stale), &g));
    TEST_ASSERT_FALSE(g.position_valid);
    TEST_ASSERT_EQUAL_INT64(0, g.lat_ndeg);
    TEST_ASSERT_FALSE(g.altitude_valid);
    TEST_ASSERT_FALSE(g.geoid_valid);
    TEST_ASSERT_TRUE(g.hdop_valid);

    /* SYNTHETIC: every field but quality and satellites empty. */
    static const char empty[] = "$GNGGA,,,,,,1,05,,,,,,,";
    TEST_ASSERT_TRUE(obs_gga_parse(empty, strlen(empty), &g));
    TEST_ASSERT_FALSE(g.utc_valid || g.position_valid || g.hdop_valid || g.altitude_valid || g.geoid_valid ||
                      g.diff_age_valid || g.diff_station_valid);

    /* SYNTHETIC: an age of 0.0 s is zero, an empty one is absent. */
    static const char zero[] = "$GNGGA,123519,4807.038,N,01131.000,E,4,12,0.8,100.0,M,10.0,M,0.0,0005";
    TEST_ASSERT_TRUE(obs_gga_parse(zero, strlen(zero), &g));
    TEST_ASSERT_TRUE(g.diff_age_valid);
    TEST_ASSERT_EQUAL_UINT32(0, g.diff_age_ms);
    TEST_ASSERT_EQUAL_UINT16(5, g.diff_station);
    static const char absent[] = "$GNGGA,123519,4807.038,N,01131.000,E,4,12,0.8,100.0,M,10.0,M,,0005";
    TEST_ASSERT_TRUE(obs_gga_parse(absent, strlen(absent), &g));
    TEST_ASSERT_FALSE(g.diff_age_valid);
    TEST_ASSERT_TRUE(g.diff_station_valid);
}

TEST_CASE("obs parse: GGA field rules: UTC fractions, units, ranges, rounding", "[gnss_obs]")
{
    obs_gga_t g;
    char l[128];
    /* SYNTHETIC lines, one field changed at a time. */
    static const struct {
        const char *utc;
        bool ok;
        uint32_t ms;
    } k_utc[] = {
        { "123519", true, 45319000u },     { "123519.5", true, 45319500u }, { "123519.123456", true, 45319123u },
        { "123519.", true, 45319000u },    { "235960.00", true, 86400000u }, { "246000", false, 0 },
        { "123460", false, 0 },            { "235860", false, 0 },
        { "126000", false, 0 },            { "1235", false, 0 },            { "12a519", false, 0 },
        { "123519.5x", false, 0 },
    };
    for (size_t i = 0; i < sizeof(k_utc) / sizeof(k_utc[0]); i++) {
        snprintf(l, sizeof(l), "$GNGGA,%s,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", k_utc[i].utc);
        TEST_ASSERT_TRUE_MESSAGE(obs_gga_parse(l, strlen(l), &g), l);
        TEST_ASSERT_EQUAL_MESSAGE(k_utc[i].ok, g.utc_valid, k_utc[i].utc);
        if (k_utc[i].ok) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(k_utc[i].ms, g.utc_ms, k_utc[i].utc);
        }
    }

    /* Latitude and longitude: minutes below 60, at most 90 / 180 degrees, a hemisphere. */
    static const struct {
        const char *lat;
        const char *lon;
        bool ok;
    } k_pos[] = {
        { "4807.038,N", "01131.000,E", true },  { "9000.000,S", "18000.000,W", true },
        { "4860.000,N", "01131.000,E", false }, { "9000.001,N", "01131.000,E", false },
        { "4807.038,N", "18000.001,E", false }, { "4807.038,X", "01131.000,E", false },
        { "4807.038,", "01131.000,E", false },  { "4807.038,N", "01131.000,N", false },
        { "-4807.038,N", "01131.000,E", false }, { ",N", "01131.000,E", false },
        { "4807.038,N", "131.000,E", true },     { "07.038,N", "01131.000,E", false },
        { "8959.999999995,N", "01131.000,E", true }, { "8959.999999995,N", "17959.999999995,E", true },
    };
    for (size_t i = 0; i < sizeof(k_pos) / sizeof(k_pos[0]); i++) {
        snprintf(l, sizeof(l), "$GNGGA,123519,%s,%s,1,08,0.9,545.4,M,46.9,M,,", k_pos[i].lat, k_pos[i].lon);
        TEST_ASSERT_TRUE_MESSAGE(obs_gga_parse(l, strlen(l), &g), l);
        TEST_ASSERT_EQUAL_MESSAGE(k_pos[i].ok, g.position_valid, l);
    }
    snprintf(l, sizeof(l), "$GNGGA,123519,9000.000,S,18000.000,W,1,08,0.9,545.4,M,46.9,M,,");
    TEST_ASSERT_TRUE(obs_gga_parse(l, strlen(l), &g));
    TEST_ASSERT_EQUAL_INT64(-90000000000LL, g.lat_ndeg);
    TEST_ASSERT_EQUAL_INT64(-180000000000LL, g.lon_ndeg);
    /* A rounding carry into the next degree: 12 deg 59.999999995 min is 13 deg. */
    snprintf(l, sizeof(l), "$GNGGA,123519,1259.999999995,N,01259.9999999,E,1,08,0.9,545.4,M,46.9,M,,");
    TEST_ASSERT_TRUE(obs_gga_parse(l, strlen(l), &g));
    TEST_ASSERT_TRUE(g.position_valid);
    TEST_ASSERT_EQUAL_INT64(13000000000LL, g.lat_ndeg);
    TEST_ASSERT_EQUAL_INT64(12999999998LL, g.lon_ndeg);   /* 59.9999999 min: below the degree */

    /* Heights only with their unit M; half away from zero at the dropped digit. */
    static const struct {
        const char *alt;
        bool ok;
        int32_t v;
    } k_alt[] = {
        { "545.4,M", true, 5454000 }, { "1.00005,M", true, 10001 }, { "-1.00005,M", true, -10001 },
        { "1.00004,M", true, 10000 }, { "545.4,F", false, 0 },      { "545.4,", false, 0 },
        { ",M", false, 0 },           { "5x5.4,M", false, 0 },      { "214749.0,M", false, 0 },
    };
    for (size_t i = 0; i < sizeof(k_alt) / sizeof(k_alt[0]); i++) {
        snprintf(l, sizeof(l), "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,%s,46.9,M,,", k_alt[i].alt);
        TEST_ASSERT_TRUE_MESSAGE(obs_gga_parse(l, strlen(l), &g), l);
        TEST_ASSERT_EQUAL_MESSAGE(k_alt[i].ok, g.altitude_valid, l);
        if (k_alt[i].ok) {
            TEST_ASSERT_EQUAL_INT32_MESSAGE(k_alt[i].v, g.altitude_01mm, l);
        }
        TEST_ASSERT_TRUE(g.geoid_valid);   /* one bad field never costs another */
    }

    /* A field that cannot be read does not refuse the sentence. */
    snprintf(l, sizeof(l), "$GNGGA,123519,4807.038,N,01131.000,E,1,08,x.9,545.4,M,46.9,M,1.0,4096");
    TEST_ASSERT_TRUE(obs_gga_parse(l, strlen(l), &g));
    TEST_ASSERT_FALSE(g.hdop_valid);
    TEST_ASSERT_TRUE(g.diff_age_valid);
    TEST_ASSERT_FALSE(g.diff_station_valid);   /* above 4095 */
    snprintf(l, sizeof(l), "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,1.0,12.5");
    TEST_ASSERT_TRUE(obs_gga_parse(l, strlen(l), &g));
    TEST_ASSERT_FALSE(g.diff_station_valid);
}

TEST_CASE("obs parse: GST of a real capture with empty ellipse fields; zero and absent apart", "[gnss_obs]")
{
    gnss_obs_gst_t g;
    /* test/fixtures/nmea/gpsd_ublox-zed-f9p-nmea_excerpt.log (gpsd, a u-blox
     * ZED-F9P): the ellipse fields empty, the RMS field as that firmware sent it. */
    static const char ub[] = "$GNGST,003956.00,235111,,,,2.3,3.5,4.0*46";
    TEST_ASSERT_TRUE(obs_gst_parse(ub, strlen(ub), &g));
    TEST_ASSERT_EQUAL_STRING("GN", g.talker);
    TEST_ASSERT_TRUE(g.utc_valid);
    TEST_ASSERT_EQUAL_UINT32((39u * 60u + 56u) * 1000u, g.utc_ms);
    TEST_ASSERT_TRUE(g.rms_valid);
    TEST_ASSERT_EQUAL_UINT32(2351110000u, g.rms_01mm);
    TEST_ASSERT_FALSE(g.major_valid);
    TEST_ASSERT_FALSE(g.minor_valid);
    TEST_ASSERT_FALSE(g.orient_valid);
    TEST_ASSERT_TRUE(g.lat_sd_valid && g.lon_sd_valid && g.alt_sd_valid);
    TEST_ASSERT_EQUAL_UINT32(23000, g.lat_sd_01mm);
    TEST_ASSERT_EQUAL_UINT32(35000, g.lon_sd_01mm);
    TEST_ASSERT_EQUAL_UINT32(40000, g.alt_sd_01mm);

    /* SYNTHETIC: every field, millimetre values, an orientation. */
    static const char full[] = "$GPGST,172814.0,0.006,0.023,0.020,273.6,0.0234,0.020,0.031";
    TEST_ASSERT_TRUE(obs_gst_parse(full, strlen(full), &g));
    TEST_ASSERT_EQUAL_UINT32(60, g.rms_01mm);
    TEST_ASSERT_EQUAL_UINT32(230, g.major_01mm);
    TEST_ASSERT_EQUAL_UINT32(200, g.minor_01mm);
    TEST_ASSERT_TRUE(g.orient_valid);
    TEST_ASSERT_EQUAL_UINT16(27360, g.orient_cdeg);
    TEST_ASSERT_EQUAL_UINT32(234, g.lat_sd_01mm);
    TEST_ASSERT_EQUAL_UINT32(310, g.alt_sd_01mm);

    /* SYNTHETIC: a zero deviation is a zero, an empty one is absent; out of range is absent. */
    static const char zero[] = "$GNGST,172814.0,0.000,,,361.0,0.000,,0.031";
    TEST_ASSERT_TRUE(obs_gst_parse(zero, strlen(zero), &g));
    TEST_ASSERT_TRUE(g.rms_valid);
    TEST_ASSERT_EQUAL_UINT32(0, g.rms_01mm);
    TEST_ASSERT_TRUE(g.lat_sd_valid);
    TEST_ASSERT_EQUAL_UINT32(0, g.lat_sd_01mm);
    TEST_ASSERT_FALSE(g.lon_sd_valid);
    TEST_ASSERT_FALSE(g.orient_valid);   /* above 360 degrees */
    static const char huge[] = "$GNGST,172814.0,429497.0,,,,1,1,1";
    TEST_ASSERT_TRUE(obs_gst_parse(huge, strlen(huge), &g));
    TEST_ASSERT_FALSE(g.rms_valid);   /* beyond what 0.1 mm in 32 bits holds */

    /* Not a GST, or one without its 8 fields. */
    static const char *const bad[] = {
        "$GNGST,172814.0,0.006,0.023,0.020,273.6,0.023,0.020", "$GNGGA,172814.0,0.006,0.023,0.020,273.6,0.023,0.020,0.031",
        "$XXGST,172814.0,0.006,0.023,0.020,273.6,0.023,0.020,0.031", "$GNGST",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(obs_gst_parse(bad[i], strlen(bad[i]), &g), bad[i]);
        TEST_ASSERT_FALSE(g.utc_valid);
    }
}
