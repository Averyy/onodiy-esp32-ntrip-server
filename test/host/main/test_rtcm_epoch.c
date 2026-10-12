/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "rtcm_epoch.h"
#include "rtcm_test_frames.h"
#include "unity.h"

#define GAP 250u

typedef struct {
    rtcm_epoch_t e;
    uint32_t now;
    uint32_t serial;
} ep_fx_t;

static void ep_init(ep_fx_t *x, uint32_t first_epoch, uint32_t now)
{
    rtcm_epoch_init(&x->e, GAP, first_epoch);
    x->now = now;
    x->serial = 0;
}

/* Feed one frame `dt` ms after the previous one. */
static uint32_t feed(ep_fx_t *x, uint32_t dt, uint16_t type, uint32_t tow, bool more, rtcm_epoch_start_t *why)
{
    uint8_t f[GNSS_RTCM3_MAX_FRAME];
    size_t len = tf_build(f, type, 40, tow, more, x->serial++);
    x->now += dt;
    return rtcm_epoch_assign(&x->e, f, len, type, x->now, why, NULL);
}

TEST_CASE("epoch: MSM header fields from a published 1074 frame", "[rtcm_epoch]")
{
    /* 1074 GPS MSM4 from a public RTCM example: TOW 270524000 ms, MMB = 1. */
    static const uint8_t v3[] = {
        0xD3, 0x00, 0x8A, 0x43, 0x20, 0x00, 0x40, 0x7F, 0x79, 0x82, 0x00, 0x20, 0x00, 0x22, 0x80, 0x65, 0x80, 0x00,
        0x00, 0x00, 0x20, 0x20, 0x00, 0x00, 0x7F, 0xFF, 0xA7, 0x22, 0x26, 0x26, 0x22, 0xA6, 0xA2, 0xA3, 0x20, 0xFD,
        0xDC, 0x05, 0x9F, 0x5B, 0x1B, 0xC6, 0x36, 0x1C, 0x86, 0x77, 0x0E, 0x32, 0x33, 0x7C, 0x61, 0x97, 0xB4, 0x0F,
        0x5E, 0x7F, 0xE6, 0xBF, 0xDF, 0xF8, 0x73, 0xF1, 0x3A, 0x5F, 0x88, 0xBD, 0x49, 0x6B, 0x82, 0xBC, 0xA6, 0xC4,
        0xCD, 0x85, 0x86, 0xFD, 0xF4, 0x1A, 0xC0, 0xFF, 0xB8, 0x38, 0x01, 0x77, 0xCC, 0x78, 0x42, 0x7D, 0xEC, 0xC5,
        0x40, 0x18, 0xA1, 0x81, 0x7B, 0xEC, 0x86, 0x04, 0x76, 0x0F, 0xEE, 0x28, 0x53, 0x6E, 0xE0, 0x84, 0x36, 0x09,
        0x22, 0x26, 0x0C, 0x72, 0x80, 0xD3, 0x4C, 0xC2, 0x8E, 0x7A, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x80, 0x00, 0x57, 0x4E, 0x18, 0x59, 0x3D, 0x75, 0xE5, 0x8D, 0xD3, 0xE7, 0x86, 0x58, 0x80, 0x71, 0xCE, 0x42,
    };
    TEST_ASSERT_EQUAL_UINT(144, sizeof(v3));
    rtcm_msm_head_t h;
    TEST_ASSERT_TRUE(rtcm_msm_head(v3, sizeof(v3), 1074, &h));
    TEST_ASSERT_EQUAL_UINT8(0, h.gnss);
    TEST_ASSERT_EQUAL_UINT32(270524000u, h.time);
    TEST_ASSERT_TRUE(h.more);

    /* Our own builder round-trips the same fields. */
    uint8_t f[64];
    size_t len = tf_build(f, 1127, 30, 0x3FFFFFFFu, false, 7);
    TEST_ASSERT_TRUE(rtcm_msm_head(f, len, 1127, &h));
    TEST_ASSERT_EQUAL_UINT8(5, h.gnss);
    TEST_ASSERT_EQUAL_UINT32(0x3FFFFFFFu, h.time);
    TEST_ASSERT_FALSE(h.more);
}

TEST_CASE("epoch: MSM numbering, SBAS numbers and short frames", "[rtcm_epoch]")
{
    TEST_ASSERT_FALSE(rtcm_msm_type(1070));
    TEST_ASSERT_TRUE(rtcm_msm_type(1071));
    TEST_ASSERT_TRUE(rtcm_msm_type(1077));
    TEST_ASSERT_FALSE(rtcm_msm_type(1078));
    TEST_ASSERT_FALSE(rtcm_msm_type(1080));
    TEST_ASSERT_TRUE(rtcm_msm_type(1087));
    TEST_ASSERT_TRUE(rtcm_msm_type(1097));
    for (uint16_t t = 1101; t <= 1107; t++) {
        TEST_ASSERT_FALSE(rtcm_msm_type(t));
    }
    TEST_ASSERT_TRUE(rtcm_msm_type(1117));
    TEST_ASSERT_TRUE(rtcm_msm_type(1127));
    TEST_ASSERT_TRUE(rtcm_msm_type(1137));
    TEST_ASSERT_FALSE(rtcm_msm_type(1138));
    TEST_ASSERT_FALSE(rtcm_msm_type(1005));
    TEST_ASSERT_FALSE(rtcm_msm_type(1230));

    /* A frame too short for the epoch time and the bit is no MSM here. */
    const uint8_t tiny[] = { 0xD3, 0x00, 0x04, 0x43, 0x50, 0x00, 0x40, 0x00, 0x00, 0x00 };
    rtcm_msm_head_t h;
    TEST_ASSERT_FALSE(rtcm_msm_head(tiny, sizeof(tiny), 1077, &h));

    /* A stray 1104/1105 never opens or terminates an epoch. */
    ep_fx_t x;
    ep_init(&x, 1, 1000);
    uint32_t e = feed(&x, 0, 1077, 5000, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1104, 9999, false, NULL));
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1105, 1, false, NULL));
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1087, 5000, true, NULL));
}

TEST_CASE("epoch: the last MSM ends the epoch, later frames start the next", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    ep_init(&x, 1, 0);

    uint32_t tow = 100000;
    uint32_t e1 = feed(&x, 0, 1005, 0, false, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_EQUAL_UINT32(1, e1);
    TEST_ASSERT_EQUAL_UINT32(e1, feed(&x, 2, 1033, 0, false, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_EQUAL_UINT32(e1, feed(&x, 5, 1077, tow, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e1, feed(&x, 5, 1087, tow, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e1, feed(&x, 5, 1127, tow, false, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);

    /* Station messages after the last MSM belong to the next epoch, even
     * right after it, and a later gap inside that epoch starts nothing. */
    uint32_t e2 = feed(&x, 1, 1005, 0, false, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    TEST_ASSERT_EQUAL_UINT32(e1 + 1, e2);
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 400, 1019, 0, false, &why));   /* late ephemeris */
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 600, 1077, tow + 1000, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1127, tow + 1000, false, NULL));
    uint32_t e3 = feed(&x, 900, 1077, tow + 2000, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    TEST_ASSERT_EQUAL_UINT32(e2 + 1, e3);

    TEST_ASSERT_EQUAL_UINT64(3, x.e.epochs);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_mmb);
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_time);
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_gap);
}

TEST_CASE("epoch: a receiver that never clears the bit splits on the gap, else on the time", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    ep_init(&x, 1, 0);
    uint32_t tow = 7000;

    /* Bursts 1 s apart: the gap rule starts each new burst. */
    uint32_t e = feed(&x, 0, 1077, tow, true, NULL);
    feed(&x, 5, 1087, tow, true, NULL);
    feed(&x, 5, 1127, tow, true, NULL);
    feed(&x, 2, 1005, 0, false, NULL);   /* inside the burst: same epoch */
    TEST_ASSERT_EQUAL_UINT32(e, x.e.epoch);
    uint32_t e2 = feed(&x, 990, 1005, 0, false, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(e + 1, e2);
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1077, tow + 1000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);

    /* Frames trickling in below the gap: the epoch-time change splits. */
    feed(&x, 100, 1087, tow + 1000, true, NULL);
    feed(&x, 100, 1127, tow + 1000, true, NULL);
    feed(&x, 100, 1033, 0, false, NULL);
    uint32_t e3 = feed(&x, 100, 1077, tow + 2000, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
    TEST_ASSERT_EQUAL_UINT32(e2 + 1, e3);
    /* Another constellation's first MSM of the new epoch does not split again. */
    TEST_ASSERT_EQUAL_UINT32(e3, feed(&x, 100, 1087, tow + 2000, true, NULL));

    /* Time beats gap when both match (checked in that order). */
    uint32_t e4 = feed(&x, 900, 1077, tow + 3000, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
    TEST_ASSERT_EQUAL_UINT32(e3 + 1, e4);

    TEST_ASSERT_EQUAL_UINT64(4, x.e.epochs);
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_mmb);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_time);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_gap);
}

TEST_CASE("epoch: a lost terminating MSM is recovered by the gap rule", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    ep_init(&x, 1, 0);
    uint32_t tow = 50000;

    uint32_t e = feed(&x, 0, 1077, tow, true, NULL);
    feed(&x, 5, 1087, tow, true, NULL);
    /* 1127 with the bit cleared was lost. A frame just under the gap stays. */
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, GAP - 1, 1033, 0, false, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    /* Exactly the gap after the previous frame: new epoch. */
    uint32_t e2 = feed(&x, GAP, 1005, 0, false, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(e + 1, e2);
    /* The new epoch has no MSM yet: further gaps start nothing. */
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 700, 1077, tow + 1000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_gap);
    /* Lost again, and the next epoch begins with its first MSM: the time rule. */
    TEST_ASSERT_EQUAL_UINT32(e2 + 1, feed(&x, 900, 1077, tow + 2000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
}

TEST_CASE("epoch: an MSM read late (a stalled reader) continues its epoch", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    ep_init(&x, 1, 0);
    uint32_t tow = 90000;

    feed(&x, 0, 1077, tow - 1000, false, NULL);   /* the first epoch is unsure; the next one is sure */
    uint32_t e = feed(&x, 900, 1005, 0, false, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    feed(&x, 5, 1077, tow, true, NULL);
    feed(&x, 5, 1087, tow, true, NULL);
    feed(&x, 5, 1097, tow, true, NULL);
    /* Flash writes held the reader: the rest of the epoch is read 400 ms later. */
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 400, 1127, tow - 14000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 2, 1137, tow, false, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    /* A constellation split over two MSMs, the second read late: still one epoch. */
    uint32_t e2 = feed(&x, 600, 1077, tow + 1000, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 300, 1077, tow + 1000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_gap);
}

TEST_CASE("epoch: GPS-time constellations compare times with each other, GLONASS and BeiDou only with themselves",
          "[rtcm_epoch]")
{
    /* A stream whose epochs end with SBAS 1107 (not an MSM here): GLONASS,
     * Galileo, 1107 closing; the next epoch begins with GPS. */
    ep_fx_t x;
    rtcm_epoch_start_t why;
    ep_init(&x, 1, 0);
    uint32_t tow = 333497000;
    uint32_t glo = 85080000;
    uint32_t e = feed(&x, 0, 1087, glo, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1097, tow, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1107, tow, false, NULL));   /* not an MSM: bit unseen */
    uint32_t e2 = feed(&x, 50, 1077, tow + 1000, true, &why);         /* Galileo held tow */
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
    TEST_ASSERT_EQUAL_UINT32(e + 1, e2);
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1087, glo + 1000, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1097, tow + 1000, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1117, tow + 1000, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1137, tow + 1000, true, NULL));
    /* BeiDou 14 s behind and GLONASS on its own scale join without a split. */
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1127, tow + 1000 - 14000, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e2, feed(&x, 5, 1127, tow + 1000, true, &why) - 1u);   /* same scale as itself only */
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_time);
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_mmb);

    /* QZSS or NavIC first in an epoch: GPS of another time splits. */
    ep_init(&x, 1, 0);
    e = feed(&x, 0, 1117, tow, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(e + 1, feed(&x, 5, 1077, tow + 1000, true, &why));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
}

TEST_CASE("epoch: a constellation split over several MSMs stays in one epoch", "[rtcm_epoch]")
{
    ep_fx_t x;
    ep_init(&x, 1, 0);
    uint32_t tow = 14000;   /* BDS time: no conversion needed within the constellation */

    uint32_t e = feed(&x, 0, 1077, tow, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1127, tow, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1127, tow, true, NULL));
    TEST_ASSERT_EQUAL_UINT32(e, feed(&x, 5, 1127, tow, false, NULL));
    TEST_ASSERT_EQUAL_UINT32(e + 1, feed(&x, 950, 1077, tow + 1000, true, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, x.e.by_time);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_mmb);
}

TEST_CASE("epoch: a forced start (framer reset) and the sequence number wrap", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    /* Clock and sequence number both about to wrap. */
    ep_init(&x, UINT32_MAX - 1u, UINT32_MAX - 300u);
    uint32_t tow = 1000;

    uint32_t e1 = feed(&x, 0, 1077, tow, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1u, e1);
    feed(&x, 5, 1087, tow, true, NULL);

    /* Reset mid-epoch: the next frame starts an epoch, counted only in total. */
    rtcm_epoch_force_new(&x.e);
    uint32_t e2 = feed(&x, 5, 1087, tow, true, &why);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, e2);
    feed(&x, 5, 1127, tow, false, NULL);

    /* The gap rule works across the clock wrap (the clock passes 0 here). */
    uint32_t e3 = feed(&x, 100, 1077, tow + 1000, true, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, e3);
    uint32_t e4 = feed(&x, 400, 1005, 0, false, &why);
    TEST_ASSERT_LESS_THAN_UINT32(UINT32_MAX - 300u, x.now);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(1, e4);

    TEST_ASSERT_TRUE(rtcm_epoch_after(e3, e2));
    TEST_ASSERT_TRUE(rtcm_epoch_after(e4, e1));
    TEST_ASSERT_FALSE(rtcm_epoch_after(e1, e4));
    TEST_ASSERT_FALSE(rtcm_epoch_after(e4, e4));
    TEST_ASSERT_EQUAL_UINT64(4, x.e.epochs);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_mmb);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_gap);
}

/* Feed one frame and also report whether its epoch is unsure. */
static uint32_t feed_u(ep_fx_t *x, uint32_t dt, uint16_t type, uint32_t tow, bool more, rtcm_epoch_start_t *why,
                       bool *unsure)
{
    uint8_t f[GNSS_RTCM3_MAX_FRAME];
    size_t len = tf_build(f, type, 40, tow, more, x->serial++);
    x->now += dt;
    return rtcm_epoch_assign(&x->e, f, len, type, x->now, why, unsure);
}

TEST_CASE("epoch: first and forced epochs are unsure; MMB, time and gap epochs are sure", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    bool unsure = false;
    ep_init(&x, 1, 0);
    uint32_t tow = 200000;

    /* The very first frame: unsure, and every frame of that epoch too. */
    uint32_t e1 = feed_u(&x, 0, 1087, tow, true, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e1, feed_u(&x, 5, 1097, tow, true, &why, &unsure));
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e1, feed_u(&x, 5, 1127, tow, false, &why, &unsure));
    TEST_ASSERT_TRUE(unsure);

    /* MMB: sure. */
    uint32_t e2 = feed_u(&x, 800, 1077, tow + 1000, true, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    TEST_ASSERT_EQUAL_UINT32(e1 + 1, e2);
    TEST_ASSERT_FALSE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e2, feed_u(&x, 5, 1087, tow + 1000, true, &why, &unsure));
    TEST_ASSERT_FALSE(unsure);

    /* Time: sure. */
    uint32_t e3 = feed_u(&x, 100, 1077, tow + 2000, true, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_TIME, why);
    TEST_ASSERT_FALSE(unsure);

    /* Gap with an open MSM epoch: sure. */
    uint32_t e4 = feed_u(&x, 900, 1005, 0, false, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(e3 + 1, e4);
    TEST_ASSERT_FALSE(unsure);

    /* Forced (framer reset mid-epoch): unsure again, until the next rule. */
    feed_u(&x, 5, 1077, tow + 3000, true, NULL, NULL);
    rtcm_epoch_force_new(&x.e);
    uint32_t e5 = feed_u(&x, 5, 1097, tow + 3000, true, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e5, feed_u(&x, 5, 1127, tow + 3000, false, &why, &unsure));
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e5 + 1, feed_u(&x, 900, 1005, 0, false, &why, &unsure));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_MMB, why);
    TEST_ASSERT_FALSE(unsure);

    /* A forced start, then a gap inside that unsure epoch: sure again. */
    rtcm_epoch_force_new(&x.e);
    uint32_t e6 = feed_u(&x, 5, 1077, tow + 4000, true, &why, &unsure);
    TEST_ASSERT_TRUE(unsure);
    uint32_t e7 = feed_u(&x, 600, 1077, tow + 4000, true, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(e6 + 1, e7);
    TEST_ASSERT_FALSE(unsure);

    TEST_ASSERT_EQUAL_UINT64(8, x.e.epochs);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_mmb);
    TEST_ASSERT_EQUAL_UINT64(1, x.e.by_time);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_gap);
}

TEST_CASE("epoch: a gap ends an unsure epoch of a stream without MSMs", "[rtcm_epoch]")
{
    ep_fx_t x;
    rtcm_epoch_start_t why;
    bool unsure = false;
    ep_init(&x, 1, 0);

    /* 1 Hz station messages only: the first epoch is unsure ... */
    uint32_t e1 = feed_u(&x, 0, 1033, 0, false, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e1, feed_u(&x, GAP - 1, 1230, 0, false, &why, &unsure));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_TRUE(unsure);
    /* ... and the next frame a gap later starts a sure one. */
    uint32_t e2 = feed_u(&x, 1000, 1005, 0, false, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_EQUAL_UINT32(e1 + 1, e2);
    TEST_ASSERT_FALSE(unsure);
    /* A sure epoch with no MSM open ignores gaps, as before. */
    TEST_ASSERT_EQUAL_UINT32(e2, feed_u(&x, 1000, 1005, 0, false, &why, &unsure));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_CONTINUES, why);
    TEST_ASSERT_FALSE(unsure);

    /* After a reset the same: unsure, then sure at the next gap. */
    rtcm_epoch_force_new(&x.e);
    uint32_t e3 = feed_u(&x, 1000, 1005, 0, false, &why, &unsure);
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_FIRST, why);
    TEST_ASSERT_TRUE(unsure);
    TEST_ASSERT_EQUAL_UINT32(e3 + 1, feed_u(&x, 1000, 1005, 0, false, &why, &unsure));
    TEST_ASSERT_EQUAL(RTCM_EPOCH_START_GAP, why);
    TEST_ASSERT_FALSE(unsure);
    TEST_ASSERT_EQUAL_UINT64(2, x.e.by_gap);
}
