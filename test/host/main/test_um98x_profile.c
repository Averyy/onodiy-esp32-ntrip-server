/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <string.h>

#include "um98x_profile.h"
#include "unity.h"

/* um98x_profile_hash() of um98x_profile_default(); see "profile hash format is frozen". */
#define DEFAULT_PROFILE_HASH 0xCAC97CD8u

TEST_CASE("default profile is the Onocoy set and valid", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_profile_default(&p);
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));
    TEST_ASSERT_EQUAL_UINT8(3, p.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(6, p.signal_group_slave);
    TEST_ASSERT_EQUAL(UM98X_BASE_SURVEY_IN, p.base_mode);
    TEST_ASSERT_EQUAL_UINT8(9, p.rtcm_count);
    TEST_ASSERT_EQUAL_UINT16(1005, p.rtcm[0].msg);
    TEST_ASSERT_EQUAL_UINT16(10, p.rtcm[0].period_s);
    TEST_ASSERT_EQUAL_UINT16(1137, p.rtcm[7].msg);
    TEST_ASSERT_EQUAL_UINT16(1, p.rtcm[7].period_s);
}

/* Copy every named field, leaving whatever b's padding holds. */
static void copy_fields(um98x_base_profile_t *b, const um98x_base_profile_t *a)
{
    b->signal_group_master = a->signal_group_master;
    b->signal_group_slave = a->signal_group_slave;
    b->base_mode = a->base_mode;
    b->survey_in_s = a->survey_in_s;
    b->fixed_lat_deg = a->fixed_lat_deg;
    b->fixed_lon_deg = a->fixed_lon_deg;
    b->fixed_height_m = a->fixed_height_m;
    b->rtcm_count = a->rtcm_count;
    for (size_t i = 0; i < UM98X_MAX_RTCM_MSGS; i++) {
        b->rtcm[i].msg = a->rtcm[i].msg;
        b->rtcm[i].period_s = a->rtcm[i].period_s;
    }
    b->sat_view_output = a->sat_view_output;
}

static void fixed_profile(um98x_base_profile_t *p)
{
    um98x_profile_default(p);
    p->base_mode = UM98X_BASE_FIXED;
    p->fixed_lat_deg = 43.6532;
    p->fixed_lon_deg = -79.3832;
    p->fixed_height_m = 112.5;
    p->signal_group_slave = 0;
    p->sat_view_output = false;
}

TEST_CASE("profile blob round-trips every field", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    fixed_profile(&a);

    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    memset(&b, 0x5A, sizeof(b));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_master, b.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_slave, b.signal_group_slave);
    TEST_ASSERT_EQUAL(a.base_mode, b.base_mode);
    TEST_ASSERT_TRUE(a.fixed_lat_deg == b.fixed_lat_deg);
    TEST_ASSERT_TRUE(a.fixed_lon_deg == b.fixed_lon_deg);
    TEST_ASSERT_TRUE(a.fixed_height_m == b.fixed_height_m);
    TEST_ASSERT_EQUAL_UINT16(0, b.survey_in_s);      /* not used in FIXED mode: cleared */
    TEST_ASSERT_EQUAL(a.sat_view_output, b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT8(a.rtcm_count, b.rtcm_count);
    TEST_ASSERT_EQUAL_MEMORY(a.rtcm, b.rtcm, sizeof(a.rtcm[0]) * a.rtcm_count);

    um98x_profile_default(&a);
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL(UM98X_BASE_SURVEY_IN, b.base_mode);
    TEST_ASSERT_EQUAL_UINT16(a.survey_in_s, b.survey_in_s);
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_slave, b.signal_group_slave);
    TEST_ASSERT_EQUAL(a.sat_view_output, b.sat_view_output);
}

TEST_CASE("profile hash ignores struct padding", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    memset(&a, 0x00, sizeof(a));
    memset(&b, 0xFF, sizeof(b));                   /* different bytes in every padding gap */
    fixed_profile(&a);
    copy_fields(&b, &a);
    TEST_ASSERT_NOT_EQUAL(0, memcmp(&a, &b, sizeof(a)));   /* the raw structs differ */
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&a), um98x_profile_hash(&b));

    b.rtcm[2].period_s = 2;
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
}

TEST_CASE("profile hash and blob ignore fields the mode does not use", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t ba[UM98X_PROFILE_BLOB_MAX];
    uint8_t bb[UM98X_PROFILE_BLOB_MAX];

    /* Survey-in: stale fixed coordinates do not matter. */
    um98x_profile_default(&a);
    um98x_profile_default(&b);
    b.fixed_lat_deg = 12.5;
    b.fixed_lon_deg = -3.25;
    b.fixed_height_m = 100.0;
    TEST_ASSERT_TRUE(um98x_profile_valid(&b));
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&a), um98x_profile_hash(&b));
    size_t la = um98x_profile_encode(&a, ba, sizeof(ba));
    size_t lb = um98x_profile_encode(&b, bb, sizeof(bb));
    TEST_ASSERT_EQUAL_size_t(la, lb);
    TEST_ASSERT_EQUAL_MEMORY(ba, bb, la);

    /* Fixed: the survey duration does not matter, and -0.0 is 0.0. */
    fixed_profile(&a);
    fixed_profile(&b);
    b.survey_in_s = 3600;
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&a), um98x_profile_hash(&b));
    a.fixed_lon_deg = 0.0;
    b.fixed_lon_deg = -0.0;
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&a), um98x_profile_hash(&b));

    /* What the receiver does use still counts. */
    um98x_profile_default(&a);
    um98x_profile_default(&b);
    b.survey_in_s = 120;
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
    b.survey_in_s = a.survey_in_s;
    b.sat_view_output = !a.sat_view_output;
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
}

TEST_CASE("profile hash format is frozen", "[um98x_profile]")
{
    /* gnss/applied_hash on every deployed station holds values like this one.
     * If this changes, every station reconfigures its receiver once. */
    um98x_base_profile_t p;
    um98x_profile_default(&p);
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
}

TEST_CASE("profile decode rejects foreign or damaged blobs", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_base_profile_t out;
    um98x_profile_default(&p);
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = um98x_profile_encode(&p, buf, sizeof(buf));

    TEST_ASSERT_FALSE(um98x_profile_decode(buf, len - 1, &out));   /* truncated */
    buf[0] = 99;                                                   /* unknown layout */
    TEST_ASSERT_FALSE(um98x_profile_decode(buf, len, &out));
    TEST_ASSERT_FALSE(um98x_profile_decode(NULL, 0, &out));
}

TEST_CASE("profile validation catches bad values", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_profile_default(&p);
    p.survey_in_s = 0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p);
    p.base_mode = UM98X_BASE_FIXED;
    p.fixed_lat_deg = 91.0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p);
    p.rtcm_count = UM98X_MAX_RTCM_MSGS + 1;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p);
    p.rtcm[0].period_s = 0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    TEST_ASSERT_EQUAL_UINT32(0, um98x_profile_hash(&p));

    uint8_t small[8];
    um98x_profile_default(&p);
    TEST_ASSERT_EQUAL_size_t(0, um98x_profile_encode(&p, small, sizeof(small)));

    /* NaN or infinity is invalid in every field, used by the mode or not. */
    um98x_profile_default(&p);
    p.fixed_lat_deg = NAN;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    um98x_profile_default(&p);
    p.fixed_height_m = INFINITY;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    um98x_profile_default(&p);
    p.base_mode = UM98X_BASE_FIXED;
    p.fixed_height_m = NAN;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
}

TEST_CASE("base position blob round-trips", "[um98x_profile]")
{
    um98x_base_position_t a = {
        .source = UM98X_POS_AVERAGED,
        .lat_deg = -33.8688,
        .lon_deg = 151.2093,
        .height_m = 58.25,
        .sigma_m = 0.012f,
        .locked_unix = 1791590400,
    };
    um98x_base_position_t b;
    uint8_t buf[UM98X_POSITION_BLOB_MAX];
    size_t len = um98x_position_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    TEST_ASSERT_TRUE(um98x_position_decode(buf, len, &b));
    TEST_ASSERT_EQUAL(a.source, b.source);
    TEST_ASSERT_TRUE(a.lat_deg == b.lat_deg);
    TEST_ASSERT_TRUE(a.lon_deg == b.lon_deg);
    TEST_ASSERT_TRUE(a.height_m == b.height_m);
    TEST_ASSERT_TRUE(a.sigma_m == b.sigma_m);
    TEST_ASSERT_EQUAL_INT64(a.locked_unix, b.locked_unix);
    TEST_ASSERT_FALSE(um98x_position_decode(buf, len - 1, &b));
}

TEST_CASE("base position: encode and decode apply the same check", "[um98x_profile]")
{
    const um98x_base_position_t good = {
        .source = UM98X_POS_SURVEY,
        .lat_deg = 45.5,
        .lon_deg = -73.5,
        .height_m = 30.0,
        .sigma_m = 0.5f,
        .locked_unix = 1791590400,
    };
    um98x_base_position_t bad[] = { good, good, good, good, good, good, good };
    bad[0].lat_deg = NAN;
    bad[1].lon_deg = 180.5;
    bad[2].height_m = NAN;
    bad[3].height_m = UM98X_HEIGHT_MAX_M + 1.0;
    bad[4].sigma_m = NAN;
    bad[5].sigma_m = -1.0f;
    bad[6].source = (um98x_pos_source_t)(UM98X_POS_USER + 1);

    uint8_t buf[UM98X_POSITION_BLOB_MAX];
    um98x_base_position_t out;
    TEST_ASSERT_TRUE(um98x_position_valid(&good));
    TEST_ASSERT_FALSE(um98x_position_valid(NULL));
    size_t good_len = um98x_position_encode(&good, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, good_len);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE(um98x_position_valid(&bad[i]));
        /* Nothing is saved that would not load again. */
        TEST_ASSERT_EQUAL_size_t(0, um98x_position_encode(&bad[i], buf, sizeof(buf)));
    }

    /* A stored record with a NaN height (written by hand or a buggy build)
     * is rejected on load: patch the height bytes of a good record. */
    um98x_position_encode(&good, buf, sizeof(buf));
    const double nan_height = NAN;
    memcpy(&buf[2 + 8 + 8], &nan_height, sizeof(nan_height));   /* version, source, lat, lon, height */
    TEST_ASSERT_FALSE(um98x_position_decode(buf, good_len, &out));
}
