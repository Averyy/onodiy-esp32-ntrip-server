/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "um98x_profile.h"
#include "unity.h"

/* Station ID used for the default profile in these tests. */
#define TEST_STATION_ID 1234u

/* um98x_profile_hash() of the default profile before 1137 joined its list
 * (TEST_STATION_ID); see "profile hash format is frozen". */
#define OLD_DEFAULT_PROFILE_HASH 0x139710D8u
/* The default with 1137, before the satellite-in-view output joined it. */
#define NO_SKY_DEFAULT_PROFILE_HASH 0x4D4127DFu
/* The default with the sky map's output, before COM1 went to 460800. */
#define BAUD115200_DEFAULT_PROFILE_HASH 0xCD18DA52u
/* The default with COM1 at 460800, before the elevation mask was a setting (the receiver's own 5 degrees). */
#define MASK5_DEFAULT_PROFILE_HASH 0xB7732F46u
/* um98x_profile_hash() of um98x_profile_default(TEST_STATION_ID): 1137, the sky map's output, COM1 at 460800 and
 * the elevation mask at 0. */
#define DEFAULT_PROFILE_HASH 0x10536B32u

/* The default message set before 1137 joined it. */
static const uint16_t k_old_set[] = { 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1230 };
#define OLD_SET_N (sizeof(k_old_set) / sizeof(k_old_set[0]))

TEST_CASE("default profile is the Onocoy set and valid", "[um98x_profile]")
{
    /* Onocoy's guide: 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137 and 1230, at 1 Hz (1005 too). */
    static const uint16_t expected[] = { 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137, 1230 };
    um98x_base_profile_t p;
    um98x_profile_default(&p, TEST_STATION_ID);
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));
    TEST_ASSERT_EQUAL_UINT8(3, p.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(6, p.signal_group_slave);
    TEST_ASSERT_EQUAL(UM98X_BASE_SURVEY_IN, p.base_mode);
    TEST_ASSERT_EQUAL_UINT16(600, p.survey_in_s);
    TEST_ASSERT_EQUAL_UINT16(TEST_STATION_ID, p.station_id);
    TEST_ASSERT_EQUAL_UINT32(460800, p.com1_baud);
    TEST_ASSERT_TRUE(p.sat_view_output);                     /* the sky map's satellite positions */
    TEST_ASSERT_EQUAL_UINT8(sizeof(expected) / sizeof(expected[0]), p.rtcm_count);
    for (size_t i = 0; i < p.rtcm_count; i++) {
        TEST_ASSERT_EQUAL_UINT16(expected[i], p.rtcm[i].msg);
        TEST_ASSERT_EQUAL_UINT16(1, p.rtcm[i].period_s);    /* every message at 1 Hz */
    }

    /* The station ID keeps 12 bits. */
    um98x_profile_default(&p, 0xFFFFu);
    TEST_ASSERT_EQUAL_UINT16(4095, p.station_id);
}

TEST_CASE("station ID is the low 12 bits of the MAC", "[um98x_profile]")
{
    const uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x9A, 0xAB, 0xCD };
    TEST_ASSERT_EQUAL_HEX16(0x0BCD, um98x_station_id_from_mac(mac));
    const uint8_t low[6] = { 0, 0, 0, 0, 0x10, 0x01 };
    TEST_ASSERT_EQUAL_HEX16(0x0001, um98x_station_id_from_mac(low));
}

/* Copy every named field, leaving whatever b's padding holds. */
static void copy_fields(um98x_base_profile_t *b, const um98x_base_profile_t *a)
{
    b->signal_group_master = a->signal_group_master;
    b->signal_group_slave = a->signal_group_slave;
    b->base_mode = a->base_mode;
    b->survey_in_s = a->survey_in_s;
    b->station_id = a->station_id;
    b->com1_baud = a->com1_baud;
    b->fixed_lat_deg = a->fixed_lat_deg;
    b->fixed_lon_deg = a->fixed_lon_deg;
    b->fixed_height_m = a->fixed_height_m;
    b->rtcm_count = a->rtcm_count;
    for (size_t i = 0; i < UM98X_MAX_RTCM_MSGS; i++) {
        b->rtcm[i].msg = a->rtcm[i].msg;
        b->rtcm[i].period_s = a->rtcm[i].period_s;
    }
    b->sat_view_output = a->sat_view_output;
    memcpy(b->antenna_model, a->antenna_model, strlen(a->antenna_model) + 1u);
    memcpy(b->antenna_serial, a->antenna_serial, strlen(a->antenna_serial) + 1u);
    b->antenna_setup_id = a->antenna_setup_id;
    b->elevation_mask_deg = a->elevation_mask_deg;
}

static void fixed_profile(um98x_base_profile_t *p)
{
    um98x_profile_default(p, TEST_STATION_ID);
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
    a.station_id = 4095;
    a.com1_baud = 460800;

    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    memset(&b, 0x5A, sizeof(b));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_master, b.signal_group_master);
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_slave, b.signal_group_slave);
    TEST_ASSERT_EQUAL(a.base_mode, b.base_mode);
    TEST_ASSERT_EQUAL_UINT16(4095, b.station_id);
    TEST_ASSERT_EQUAL_UINT32(460800, b.com1_baud);
    TEST_ASSERT_TRUE(a.fixed_lat_deg == b.fixed_lat_deg);
    TEST_ASSERT_TRUE(a.fixed_lon_deg == b.fixed_lon_deg);
    TEST_ASSERT_TRUE(a.fixed_height_m == b.fixed_height_m);
    TEST_ASSERT_EQUAL_UINT16(0, b.survey_in_s);      /* not used in FIXED mode: cleared */
    TEST_ASSERT_EQUAL(a.sat_view_output, b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT8(a.rtcm_count, b.rtcm_count);
    TEST_ASSERT_EQUAL_MEMORY(a.rtcm, b.rtcm, sizeof(a.rtcm[0]) * a.rtcm_count);

    um98x_profile_default(&a, TEST_STATION_ID);
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL(UM98X_BASE_SURVEY_IN, b.base_mode);
    TEST_ASSERT_EQUAL_UINT16(a.survey_in_s, b.survey_in_s);
    TEST_ASSERT_EQUAL_UINT8(a.signal_group_slave, b.signal_group_slave);
    TEST_ASSERT_EQUAL(a.sat_view_output, b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT16(TEST_STATION_ID, b.station_id);
    TEST_ASSERT_EQUAL_UINT32(460800, b.com1_baud);
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
    um98x_profile_default(&a, TEST_STATION_ID);
    um98x_profile_default(&b, TEST_STATION_ID);
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
    um98x_profile_default(&a, TEST_STATION_ID);
    um98x_profile_default(&b, TEST_STATION_ID);
    b.survey_in_s = 120;
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
    b.survey_in_s = a.survey_in_s;
    b.sat_view_output = !a.sat_view_output;
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
    b.sat_view_output = a.sat_view_output;
    b.station_id = (uint16_t)(a.station_id + 1);
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
    b.station_id = a.station_id;
    b.com1_baud = 230400;                         /* not the default */
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));
}

/* The default profile as it was before 1137 joined it: the old message set,
 * no satellite-in-view output, COM1 at 115200, the receiver's own mask. */
static void old_default(um98x_base_profile_t *p)
{
    um98x_profile_default(p, TEST_STATION_ID);
    p->sat_view_output = false;
    p->com1_baud = 115200;
    p->elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;
    p->rtcm_count = (uint8_t)OLD_SET_N;
    for (size_t i = 0; i < OLD_SET_N; i++) {
        p->rtcm[i].msg = k_old_set[i];
        p->rtcm[i].period_s = 1;
    }
}

TEST_CASE("profile hash format is frozen", "[um98x_profile]")
{
    /* gnss/applied_hash on every deployed station holds values like this one.
     * If the format changes, every station reconfigures its receiver once. */
    um98x_base_profile_t p;
    old_default(&p);
    TEST_ASSERT_EQUAL_HEX32(OLD_DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
    /* The default's list gained 1137, then the satellite-in-view output: a
     * new hash each time (stations without a stored profile, or with an old
     * default read as today's, reconfigure once for it). */
    um98x_profile_default(&p, TEST_STATION_ID);
    p.sat_view_output = false;
    p.com1_baud = 115200;
    p.elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;
    TEST_ASSERT_EQUAL_HEX32(NO_SKY_DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
    um98x_profile_default(&p, TEST_STATION_ID);
    p.com1_baud = 115200;
    p.elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;
    TEST_ASSERT_EQUAL_HEX32(BAUD115200_DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
    /* A mask of 5 (what the receiver ran before it was a setting) hashes as before; 0, the default, differs. */
    um98x_profile_default(&p, TEST_STATION_ID);
    p.elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;
    TEST_ASSERT_EQUAL_HEX32(MASK5_DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
    um98x_profile_default(&p, TEST_STATION_ID);
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_PROFILE_HASH, um98x_profile_hash(&p));
}

/* p encoded, then cut to what a firmware from before the defaults byte wrote (no mask byte either). */
static size_t encode_before_set_byte(const um98x_base_profile_t *p, uint8_t *buf, size_t cap)
{
    size_t len = um98x_profile_encode(p, buf, cap);
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(4, buf[len - 2]);   /* the defaults this firmware writes, before the mask */
    TEST_ASSERT_EQUAL_UINT8(p->elevation_mask_deg, buf[len - 1]);
    return len - 2;
}

/* p encoded as the firmware before the sky map wrote it (the same layout, defaults byte 2, no mask byte). */
static size_t encode_before_sky(const um98x_base_profile_t *p, uint8_t *buf, size_t cap)
{
    size_t len = um98x_profile_encode(p, buf, cap);
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(4, buf[len - 2]);
    buf[len - 2] = 2;
    return len - 1;
}

/* p encoded as the firmware before COM1 went to 460800 wrote it (defaults byte 3, no mask byte). */
static size_t encode_before_baud(const um98x_base_profile_t *p, uint8_t *buf, size_t cap)
{
    size_t len = um98x_profile_encode(p, buf, cap);
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(4, buf[len - 2]);
    buf[len - 2] = 3;
    return len - 1;
}

/* p encoded as the firmware before the elevation mask was a setting wrote it (defaults byte 4, no mask byte). */
static size_t encode_before_mask(const um98x_base_profile_t *p, uint8_t *buf, size_t cap)
{
    size_t len = um98x_profile_encode(p, buf, cap);
    TEST_ASSERT_TRUE(len > 2);
    TEST_ASSERT_EQUAL_UINT8(4, buf[len - 2]);
    return len - 1;
}

static void assert_default_list(const um98x_base_profile_t *p)
{
    um98x_base_profile_t d;
    um98x_profile_default(&d, TEST_STATION_ID);
    TEST_ASSERT_EQUAL_UINT8(d.rtcm_count, p->rtcm_count);
    TEST_ASSERT_EQUAL_MEMORY(d.rtcm, p->rtcm, sizeof(d.rtcm[0]) * d.rtcm_count);
}

TEST_CASE("profile: the old default set saved before 1137 reads as today's default", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    unsigned up = 0;

    /* Saved by an earlier firmware: upgraded (and, off then, the satellite
     * positions too), everything else as stored. */
    old_default(&a);
    a.survey_in_s = 900;
    size_t len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_RTCM | UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD |
                          UM98X_PROFILE_UPGRADED_MASK, up);
    assert_default_list(&b);
    TEST_ASSERT_TRUE(b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT16(900, b.survey_in_s);
    um98x_base_profile_t d;
    um98x_profile_default(&d, TEST_STATION_ID);
    d.survey_in_s = 900;
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&d), um98x_profile_hash(&b));
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));   /* not what was applied: applied once */
    /* The plain decode reads it the same way. */
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    assert_default_list(&b);

    /* The same set in another order (the UI calls it the Onocoy set too), a
     * fixed base, a blob from before the antenna fields: upgraded. */
    old_default(&a);
    a.base_mode = UM98X_BASE_FIXED;
    a.fixed_lat_deg = 43.6532;
    a.fixed_lon_deg = -79.3832;
    a.fixed_height_m = 112.5;
    for (size_t i = 0; i < OLD_SET_N; i++) {
        a.rtcm[i].msg = k_old_set[OLD_SET_N - 1 - i];
    }
    a.sat_view_output = true;                    /* turned on by the user then: no upgrade for it */
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len - 3, &b, &up));   /* two empty strings and the setup ID */
    /* No defaults byte: written under the first defaults, so the link at
     * 115200 (old_default) was the default then too and reads as 460800. */
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_RTCM | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_EQUAL_UINT32(460800, b.com1_baud);
    assert_default_list(&b);
    TEST_ASSERT_EQUAL(UM98X_BASE_FIXED, b.base_mode);
    TEST_ASSERT_TRUE(b.fixed_height_m == 112.5);

    /* Stored again (any save), it carries the defaults byte and decodes as is. */
    len = um98x_profile_encode(&b, buf, sizeof(buf));
    um98x_base_profile_t c;
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &c, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    assert_default_list(&c);
}

TEST_CASE("profile: any other list, or the old set chosen since, stays as stored", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    unsigned up = UM98X_PROFILE_UPGRADED_RTCM;

    /* The old set saved by this firmware (the user removed 1137 on purpose). */
    old_default(&a);
    size_t len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT8(OLD_SET_N, b.rtcm_count);
    TEST_ASSERT_EQUAL_MEMORY(a.rtcm, b.rtcm, sizeof(a.rtcm[0]) * OLD_SET_N);

    /* Saved by an earlier firmware, but not exactly the old set. */
    old_default(&a);
    a.rtcm[0].period_s = 30;                     /* 1005 every 30 s */
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);   /* the output, the link and the mask, defaults then */
    TEST_ASSERT_EQUAL_MEMORY(a.rtcm, b.rtcm, sizeof(a.rtcm[0]) * OLD_SET_N);

    old_default(&a);
    a.rtcm_count--;                               /* without 1230 */
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);   /* the output, the link and the mask, defaults then */
    TEST_ASSERT_EQUAL_UINT8(OLD_SET_N - 1, b.rtcm_count);

    old_default(&a);
    a.rtcm[a.rtcm_count].msg = 1019;              /* one more */
    a.rtcm[a.rtcm_count].period_s = 1;
    a.rtcm_count++;
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);   /* the output, the link and the mask, defaults then */
    TEST_ASSERT_EQUAL_UINT8(OLD_SET_N + 1, b.rtcm_count);

    old_default(&a);
    a.rtcm[1].msg = 1005;                         /* 1005 twice, no 1033 */
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);   /* the output, the link and the mask, defaults then */
    TEST_ASSERT_EQUAL_UINT16(1005, b.rtcm[1].msg);

    old_default(&a);
    a.rtcm_count = 0;                             /* no messages */
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);   /* the output, the link and the mask, defaults then */
    TEST_ASSERT_EQUAL_UINT8(0, b.rtcm_count);

    /* A defaults byte this firmware does not know (a newer one wrote it): as stored. */
    old_default(&a);
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    buf[len - 2] = 7;
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT8(OLD_SET_N, b.rtcm_count);
    TEST_ASSERT_FALSE(b.sat_view_output);

    /* The out flag is optional. */
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, NULL));
}

TEST_CASE("profile: satellite positions off from before the sky map read as on, off chosen since stays", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    unsigned up = 0;

    /* Saved by the firmware before (defaults 2) with the output off, its
     * default then: on, everything else as stored. */
    um98x_profile_default(&a, TEST_STATION_ID);
    a.sat_view_output = false;
    a.survey_in_s = 900;
    a.com1_baud = 460800;
    size_t len = encode_before_sky(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_TRUE(b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT16(900, b.survey_in_s);
    TEST_ASSERT_EQUAL_UINT32(460800, b.com1_baud);
    assert_default_list(&b);
    um98x_base_profile_t d;
    um98x_profile_default(&d, TEST_STATION_ID);
    d.survey_in_s = 900;
    d.com1_baud = 460800;
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&d), um98x_profile_hash(&b));
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));   /* not what was applied: applied once */
    /* The plain decode reads it the same way; nothing is written for it. */
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_TRUE(b.sat_view_output);

    /* Turned on by the user then: on, only the mask upgraded. */
    a.sat_view_output = true;
    len = encode_before_sky(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_TRUE(b.sat_view_output);

    /* Off saved by this firmware: the user's choice, kept. */
    a.sat_view_output = false;
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(4, buf[len - 2]);
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_FALSE(b.sat_view_output);
    TEST_ASSERT_EQUAL_HEX32(um98x_profile_hash(&a), um98x_profile_hash(&b));

    /* The upgraded profile stored again (any save): on, as stored, from then on. */
    len = encode_before_sky(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    len = um98x_profile_encode(&b, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &a, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_TRUE(a.sat_view_output);

    /* Older still (no defaults byte), a list of the user's own: the output only. */
    um98x_profile_default(&a, TEST_STATION_ID);
    a.sat_view_output = false;
    a.rtcm_count = 2;
    len = encode_before_set_byte(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_SKY | UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_TRUE(b.sat_view_output);
    TEST_ASSERT_EQUAL_UINT8(2, b.rtcm_count);
}

TEST_CASE("profile: COM1 at 115200 from before 460800 reads as 460800, a speed chosen since stays", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    unsigned up = 0;

    /* Saved by the firmware before (defaults 3) at 115200, its default then: 460800, everything else as stored. */
    um98x_profile_default(&a, TEST_STATION_ID);
    a.com1_baud = 115200;
    a.survey_in_s = 900;
    size_t len = encode_before_baud(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_BAUD | UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_EQUAL_UINT32(460800, b.com1_baud);
    TEST_ASSERT_EQUAL_UINT16(900, b.survey_in_s);
    TEST_ASSERT_TRUE(b.sat_view_output);
    assert_default_list(&b);
    TEST_ASSERT_NOT_EQUAL(um98x_profile_hash(&a), um98x_profile_hash(&b));   /* not what was applied: applied once */

    /* Another speed chosen then: as stored (only the mask upgraded). */
    a.com1_baud = 230400;
    len = encode_before_baud(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_EQUAL_UINT32(230400, b.com1_baud);

    /* 115200 saved by this firmware: the user's choice, kept. */
    a.com1_baud = 115200;
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT32(115200, b.com1_baud);

    /* The upgraded profile stored again (any save): 460800, as stored, from then on. */
    len = encode_before_baud(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    len = um98x_profile_encode(&b, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &a, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT32(460800, a.com1_baud);
}

TEST_CASE("profile: the elevation mask round-trips, is checked, and a blob without it reads as 0", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    unsigned up = 0;

    um98x_profile_default(&a, TEST_STATION_ID);
    TEST_ASSERT_EQUAL_UINT8(0, a.elevation_mask_deg);
    a.elevation_mask_deg = UM98X_MASK_MAX_DEG;
    TEST_ASSERT_TRUE(um98x_profile_valid(&a));
    a.elevation_mask_deg = UM98X_MASK_MAX_DEG + 1;
    TEST_ASSERT_FALSE(um98x_profile_valid(&a));
    TEST_ASSERT_EQUAL_size_t(0, um98x_profile_encode(&a, buf, sizeof(buf)));

    /* Any value saved by this firmware stays, 5 included. */
    a.elevation_mask_deg = 10;
    size_t len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT8(10, b.elevation_mask_deg);
    a.elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(0, up);
    TEST_ASSERT_EQUAL_UINT8(UM98X_RX_DEFAULT_MASK_DEG, b.elevation_mask_deg);

    /* Saved before it was a setting: today's default, not what was applied (applied once). */
    um98x_profile_default(&a, TEST_STATION_ID);
    a.elevation_mask_deg = UM98X_RX_DEFAULT_MASK_DEG;   /* what that receiver ran */
    len = encode_before_mask(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode_ex(buf, len, &b, &up));
    TEST_ASSERT_EQUAL_HEX(UM98X_PROFILE_UPGRADED_MASK, up);
    TEST_ASSERT_EQUAL_UINT8(UM98X_DEFAULT_MASK_DEG, b.elevation_mask_deg);
    TEST_ASSERT_EQUAL_HEX32(MASK5_DEFAULT_PROFILE_HASH, um98x_profile_hash(&a));
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_PROFILE_HASH, um98x_profile_hash(&b));

    /* Each value hashes apart. */
    um98x_profile_default(&b, TEST_STATION_ID);
    uint32_t h0 = um98x_profile_hash(&b);
    b.elevation_mask_deg = 10;
    TEST_ASSERT_NOT_EQUAL(h0, um98x_profile_hash(&b));
    TEST_ASSERT_NOT_EQUAL(MASK5_DEFAULT_PROFILE_HASH, um98x_profile_hash(&b));
}

TEST_CASE("profile decode rejects foreign or damaged blobs", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_base_profile_t out;
    um98x_profile_default(&p, TEST_STATION_ID);
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = um98x_profile_encode(&p, buf, sizeof(buf));

    TEST_ASSERT_FALSE(um98x_profile_decode(buf, len - 3, &out));   /* truncated inside the antenna fields */
    buf[0] = 99;                                                   /* unknown layout */
    TEST_ASSERT_FALSE(um98x_profile_decode(buf, len, &out));
    TEST_ASSERT_FALSE(um98x_profile_decode(NULL, 0, &out));
}

TEST_CASE("profile validation catches bad values", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_profile_default(&p, TEST_STATION_ID);
    p.survey_in_s = 0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    p.survey_in_s = UM98X_SURVEY_MAX_S + 1;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    p.survey_in_s = UM98X_SURVEY_MAX_S;
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));

    um98x_profile_default(&p, TEST_STATION_ID);
    p.station_id = UM98X_STATION_ID_MAX + 1;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p, TEST_STATION_ID);
    p.com1_baud = 100000;                                /* not a receiver baud */
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    p.com1_baud = 0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    p.com1_baud = 921600;
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));

    um98x_profile_default(&p, TEST_STATION_ID);
    p.base_mode = UM98X_BASE_FIXED;
    p.fixed_lat_deg = 91.0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p, TEST_STATION_ID);
    p.rtcm_count = UM98X_MAX_RTCM_MSGS + 1;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    um98x_profile_default(&p, TEST_STATION_ID);
    p.rtcm[0].period_s = 0;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    TEST_ASSERT_EQUAL_UINT32(0, um98x_profile_hash(&p));

    uint8_t small[8];
    um98x_profile_default(&p, TEST_STATION_ID);
    TEST_ASSERT_EQUAL_size_t(0, um98x_profile_encode(&p, small, sizeof(small)));

    /* NaN or infinity is invalid in every field, used by the mode or not. */
    um98x_profile_default(&p, TEST_STATION_ID);
    p.fixed_lat_deg = NAN;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    um98x_profile_default(&p, TEST_STATION_ID);
    p.fixed_height_m = INFINITY;
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    um98x_profile_default(&p, TEST_STATION_ID);
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

/* Distance between two ECEF points, metres. */
static double ecef_dist(const double a[3], const double b[3])
{
    double dx = a[0] - b[0];
    double dy = a[1] - b[1];
    double dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

TEST_CASE("WGS84 to ECEF matches the ellipsoid axes", "[um98x_profile]")
{
    double xyz[3];
    um98x_llh_to_ecef(0.0, 0.0, 0.0, xyz);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 6378137.0, xyz[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, xyz[1]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, xyz[2]);

    um98x_llh_to_ecef(90.0, 0.0, 0.0, xyz);           /* semi-minor axis */
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, xyz[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 6356752.314245, xyz[2]);

    um98x_llh_to_ecef(0.0, 90.0, 100.0, xyz);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 6378237.0, xyz[1]);
}

TEST_CASE("ECEF and WGS84 round-trip within 0.1 mm", "[um98x_profile]")
{
    static const double points[][3] = {
        { 43.6532, -79.3832, 112.5 },     /* Toronto */
        { -33.8688, 151.2093, 58.25 },    /* Sydney */
        { 51.4779, -0.0015, 45.0 },
        { 0.0, 180.0, -500.0 },
        { -0.000001, -179.999999, 9999.0 },
        { 89.9999999, 12.0, 10.0 },       /* next to the pole */
        { -90.0, 0.0, 0.0 },
        { 90.0, 0.0, 8848.0 },
        { 27.9881, 86.925, 8848.86 },
        { -45.0, 170.0, -999.0 },
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        double xyz[3];
        double back[3];
        double lat;
        double lon;
        double h;
        um98x_llh_to_ecef(points[i][0], points[i][1], points[i][2], xyz);
        TEST_ASSERT_TRUE(um98x_ecef_to_llh(xyz, &lat, &lon, &h));
        TEST_ASSERT_DOUBLE_WITHIN(1e-4, points[i][2], h);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, points[i][0], lat);   /* ~0.1 mm of latitude */
        um98x_llh_to_ecef(lat, lon, h, back);
        TEST_ASSERT_TRUE(ecef_dist(xyz, back) < 1e-4);
    }

    /* A 1005-style position (0.1 mm units, here in metres). */
    const double arp[3] = { 850163.8571, -4542463.3207, 4383373.3092 };
    double lat;
    double lon;
    double h;
    double back[3];
    TEST_ASSERT_TRUE(um98x_ecef_to_llh(arp, &lat, &lon, &h));
    um98x_llh_to_ecef(lat, lon, h, back);
    TEST_ASSERT_TRUE(ecef_dist(arp, back) < 1e-4);
}

TEST_CASE("ECEF to WGS84 rejects points nowhere near the surface", "[um98x_profile]")
{
    double lat;
    double lon;
    double h;
    const double centre[3] = { 0.0, 0.0, 0.0 };
    const double bad[3] = { NAN, 0.0, 6378137.0 };
    TEST_ASSERT_FALSE(um98x_ecef_to_llh(centre, &lat, &lon, &h));
    TEST_ASSERT_FALSE(um98x_ecef_to_llh(bad, &lat, &lon, &h));
}

/* ---- antenna descriptor ------------------------------------------------------ */

#define IGS_NAME "TRM59800.00     SCIS"   /* 16-character name, then the radome */

TEST_CASE("antenna fields: validation", "[um98x_profile]")
{
    um98x_base_profile_t p;
    um98x_profile_default(&p, TEST_STATION_ID);
    TEST_ASSERT_EQUAL_STRING("", p.antenna_model);    /* default: leave the receiver's value */
    TEST_ASSERT_EQUAL_STRING("", p.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(0, p.antenna_setup_id);

    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", IGS_NAME);
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "5000112233");
    p.antenna_setup_id = 255;
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));

    /* 31 characters fit, a string without its terminator does not. */
    memset(p.antenna_model, 'A', 31);
    p.antenna_model[31] = '\0';
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));
    memset(p.antenna_model, 'A', sizeof(p.antenna_model));
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));

    /* Printable ASCII without '"' only (the command quotes the strings). */
    snprintf(p.antenna_model, sizeof(p.antenna_model), "ANT\"NAME");
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    snprintf(p.antenna_model, sizeof(p.antenna_model), "ANT\tNAME");
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    snprintf(p.antenna_model, sizeof(p.antenna_model), "ANT\x7FNAME");
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    snprintf(p.antenna_model, sizeof(p.antenna_model), "%s", IGS_NAME);
    snprintf(p.antenna_serial, sizeof(p.antenna_serial), "SN\"1");
    TEST_ASSERT_FALSE(um98x_profile_valid(&p));
    TEST_ASSERT_EQUAL_UINT32(0, um98x_profile_hash(&p));

    TEST_ASSERT_TRUE(um98x_antenna_str_valid("", 32));
    TEST_ASSERT_FALSE(um98x_antenna_str_valid(NULL, 32));
    TEST_ASSERT_EQUAL_size_t(4, um98x_antenna_str_len("NAME   "));
    TEST_ASSERT_EQUAL_size_t(0, um98x_antenna_str_len("   "));
    TEST_ASSERT_EQUAL_size_t(20, um98x_antenna_str_len(IGS_NAME));   /* inner spaces kept */
}

TEST_CASE("antenna fields: names compare with runs of spaces read as one", "[um98x_profile]")
{
    /* The bench's exact strings: the profile's IGS form and what UM982
     * build 25164 read back after storing it. */
    TEST_ASSERT_TRUE(um98x_antenna_str_same("TRM59800.00     SCIS", "TRM59800.00 SCIS"));
    TEST_ASSERT_TRUE(um98x_antenna_str_same("TRM59800.00 SCIS", "TRM59800.00     SCIS"));
    TEST_ASSERT_TRUE(um98x_antenna_str_same("TRM59800.00     SCIS", "TRM59800.00     SCIS"));
    TEST_ASSERT_TRUE(um98x_antenna_str_same("ADVNULLANTENNA", "ADVNULLANTENNA"));
    TEST_ASSERT_TRUE(um98x_antenna_str_same("5000123", "5000123"));
    /* Leading and trailing spaces do not count; an empty one equals "". */
    TEST_ASSERT_TRUE(um98x_antenna_str_same("  TRM59800.00  SCIS  ", "TRM59800.00 SCIS"));
    TEST_ASSERT_TRUE(um98x_antenna_str_same("", "   "));
    TEST_ASSERT_TRUE(um98x_antenna_str_same(NULL, ""));
    /* Spaces matter where they are: none is not one, and the letters must match. */
    TEST_ASSERT_FALSE(um98x_antenna_str_same("TRM59800.00SCIS", "TRM59800.00 SCIS"));
    TEST_ASSERT_FALSE(um98x_antenna_str_same("TRM59800.00     SCIS", "TRM59800.00     NONE"));
    TEST_ASSERT_FALSE(um98x_antenna_str_same("TRM59800.00 SCIS", "TRM59800.00 SCI"));
    TEST_ASSERT_FALSE(um98x_antenna_str_same("TRM59800.00", "TRM59800.00 SCIS"));
    TEST_ASSERT_FALSE(um98x_antenna_str_same("", "ADVNULLANTENNA"));
    TEST_ASSERT_FALSE(um98x_antenna_str_same("5000123", "5000124"));
}

TEST_CASE("antenna fields: blob round trip, trailing spaces dropped, older blobs", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    um98x_profile_default(&a, TEST_STATION_ID);
    snprintf(a.antenna_model, sizeof(a.antenna_model), "%s  ", IGS_NAME);
    snprintf(a.antenna_serial, sizeof(a.antenna_serial), "5000112233 ");
    a.antenna_setup_id = 7;
    size_t len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    memset(&b, 0x5A, sizeof(b));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL_STRING(IGS_NAME, b.antenna_model);
    TEST_ASSERT_EQUAL_STRING("5000112233", b.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(7, b.antenna_setup_id);

    /* The longest strings fit the blob. */
    memset(a.antenna_model, 'M', 31);
    a.antenna_model[31] = '\0';
    memset(a.antenna_serial, 'S', 31);
    a.antenna_serial[31] = '\0';
    a.rtcm_count = UM98X_MAX_RTCM_MSGS;
    for (size_t i = 0; i < UM98X_MAX_RTCM_MSGS; i++) {
        a.rtcm[i].msg = (uint16_t)(1001 + i);
        a.rtcm[i].period_s = 1;
    }
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL_STRING(a.antenna_model, b.antenna_model);
    TEST_ASSERT_EQUAL_STRING(a.antenna_serial, b.antenna_serial);

    /* Without a model the serial number and setup ID are never sent: cleared. */
    um98x_profile_default(&a, TEST_STATION_ID);
    snprintf(a.antenna_serial, sizeof(a.antenna_serial), "5000112233");
    a.antenna_setup_id = 3;
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len, &b));
    TEST_ASSERT_EQUAL_STRING("", b.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(0, b.antenna_setup_id);

    /* A blob written before the antenna fields existed (it ends after the
     * RTCM list) decodes with an empty antenna. */
    um98x_profile_default(&a, TEST_STATION_ID);
    len = um98x_profile_encode(&a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(um98x_profile_decode(buf, len - 5, &b));   /* two empty strings, the setup ID, the set byte, the mask */
    TEST_ASSERT_EQUAL_STRING("", b.antenna_model);
    TEST_ASSERT_EQUAL_UINT32(um98x_profile_hash(&a), um98x_profile_hash(&b));
}

TEST_CASE("antenna fields: the hash changes with each field, only with a model", "[um98x_profile]")
{
    um98x_base_profile_t a;
    um98x_base_profile_t b;
    um98x_profile_default(&a, TEST_STATION_ID);
    um98x_profile_default(&b, TEST_STATION_ID);

    /* No model: serial and setup ID change nothing (and the hash is the
     * one profiles had before the antenna fields). */
    snprintf(b.antenna_serial, sizeof(b.antenna_serial), "123");
    b.antenna_setup_id = 9;
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_PROFILE_HASH, um98x_profile_hash(&b));
    snprintf(b.antenna_model, sizeof(b.antenna_model), "   ");   /* blank = none */
    TEST_ASSERT_EQUAL_HEX32(DEFAULT_PROFILE_HASH, um98x_profile_hash(&b));

    snprintf(a.antenna_model, sizeof(a.antenna_model), "%s", IGS_NAME);
    snprintf(b.antenna_model, sizeof(b.antenna_model), "%s", IGS_NAME);
    a.antenna_serial[0] = '\0';
    b.antenna_serial[0] = '\0';
    a.antenna_setup_id = 0;
    b.antenna_setup_id = 0;
    uint32_t h = um98x_profile_hash(&a);
    TEST_ASSERT_NOT_EQUAL(DEFAULT_PROFILE_HASH, h);
    TEST_ASSERT_EQUAL_HEX32(h, um98x_profile_hash(&b));
    snprintf(b.antenna_model, sizeof(b.antenna_model), "%s ", IGS_NAME);   /* trailing space */
    TEST_ASSERT_EQUAL_HEX32(h, um98x_profile_hash(&b));
    snprintf(b.antenna_model, sizeof(b.antenna_model), "TRM59800.00     NONE");
    TEST_ASSERT_NOT_EQUAL(h, um98x_profile_hash(&b));
    snprintf(b.antenna_model, sizeof(b.antenna_model), "%s", IGS_NAME);
    snprintf(b.antenna_serial, sizeof(b.antenna_serial), "1");
    TEST_ASSERT_NOT_EQUAL(h, um98x_profile_hash(&b));
    b.antenna_serial[0] = '\0';
    b.antenna_setup_id = 1;
    TEST_ASSERT_NOT_EQUAL(h, um98x_profile_hash(&b));
}
