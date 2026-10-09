/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_profile.h"

#include <math.h>
#include <string.h>

#include "settings_codec.h"

#define PROFILE_BLOB_VERSION  1
#define POSITION_BLOB_VERSION 1

/* Format of um98x_profile_hash(), frozen separately from the blob layout:
 * changing it makes every station's gnss/applied_hash mismatch, so each one
 * reconfigures its receiver once (and an unlocked station surveys again). */
#define PROFILE_HASH_FORMAT 1

static bool height_ok(double h)
{
    return h >= UM98X_HEIGHT_MIN_M && h <= UM98X_HEIGHT_MAX_M;   /* false for NaN */
}

static double plus_zero(double v)
{
    return (v == 0.0) ? 0.0 : v;   /* -0.0 configures the receiver like 0.0 */
}

/* The profile with the fields its base mode does not use cleared, so two
 * profiles that configure the receiver the same way encode and hash the same.
 * Only for a valid profile. */
static void canonical(const um98x_base_profile_t *p, um98x_base_profile_t *out)
{
    memset(out, 0, sizeof(*out));
    out->signal_group_master = p->signal_group_master;
    out->signal_group_slave = p->signal_group_slave;
    out->base_mode = p->base_mode;
    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        out->survey_in_s = p->survey_in_s;
    } else {
        out->fixed_lat_deg = plus_zero(p->fixed_lat_deg);
        out->fixed_lon_deg = plus_zero(p->fixed_lon_deg);
        out->fixed_height_m = plus_zero(p->fixed_height_m);
    }
    out->rtcm_count = p->rtcm_count;
    memcpy(out->rtcm, p->rtcm, sizeof(out->rtcm[0]) * p->rtcm_count);
    out->sat_view_output = p->sat_view_output;
}

void um98x_profile_default(um98x_base_profile_t *out)
{
    static const um98x_rtcm_out_t onocoy_set[] = {
        { 1005, 10 }, { 1033, 10 },
        { 1077, 1 }, { 1087, 1 }, { 1097, 1 }, { 1117, 1 }, { 1127, 1 }, { 1137, 1 },
        { 1230, 10 },
    };
    _Static_assert(sizeof(onocoy_set) / sizeof(onocoy_set[0]) <= UM98X_MAX_RTCM_MSGS,
                   "default RTCM set exceeds UM98X_MAX_RTCM_MSGS");

    memset(out, 0, sizeof(*out));
    out->signal_group_master = 3;
    out->signal_group_slave = 6;
    out->base_mode = UM98X_BASE_SURVEY_IN;
    out->survey_in_s = 60;
    out->rtcm_count = (uint8_t)(sizeof(onocoy_set) / sizeof(onocoy_set[0]));
    memcpy(out->rtcm, onocoy_set, sizeof(onocoy_set));
    out->sat_view_output = true;
}

bool um98x_profile_valid(const um98x_base_profile_t *p)
{
    if (p == NULL || p->rtcm_count > UM98X_MAX_RTCM_MSGS) {
        return false;
    }
    /* NaN or infinity is invalid in every mode, used or not. */
    if (!isfinite(p->fixed_lat_deg) || !isfinite(p->fixed_lon_deg) || !isfinite(p->fixed_height_m)) {
        return false;
    }
    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        if (p->survey_in_s == 0) {
            return false;
        }
    } else if (p->base_mode == UM98X_BASE_FIXED) {
        if (!(p->fixed_lat_deg >= -90.0 && p->fixed_lat_deg <= 90.0) ||
            !(p->fixed_lon_deg >= -180.0 && p->fixed_lon_deg <= 180.0) || !height_ok(p->fixed_height_m)) {
            return false;
        }
    } else {
        return false;
    }
    for (size_t i = 0; i < p->rtcm_count; i++) {
        if (p->rtcm[i].msg < 1001 || p->rtcm[i].msg > 4095 || p->rtcm[i].period_s == 0) {
            return false;
        }
    }
    return true;
}

size_t um98x_profile_encode(const um98x_base_profile_t *profile, uint8_t *buf, size_t cap)
{
    if (!um98x_profile_valid(profile)) {
        return 0;
    }
    um98x_base_profile_t c;
    canonical(profile, &c);
    const um98x_base_profile_t *p = &c;
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, PROFILE_BLOB_VERSION);
    blob_put_u8(&w, p->signal_group_master);
    blob_put_u8(&w, p->signal_group_slave);
    blob_put_u8(&w, (uint8_t)p->base_mode);
    blob_put_u16(&w, p->survey_in_s);
    blob_put_f64(&w, p->fixed_lat_deg);
    blob_put_f64(&w, p->fixed_lon_deg);
    blob_put_f64(&w, p->fixed_height_m);
    blob_put_bool(&w, p->sat_view_output);
    blob_put_u8(&w, p->rtcm_count);
    for (size_t i = 0; i < p->rtcm_count; i++) {
        blob_put_u16(&w, p->rtcm[i].msg);
        blob_put_u16(&w, p->rtcm[i].period_s);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

bool um98x_profile_decode(const uint8_t *buf, size_t len, um98x_base_profile_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) != PROFILE_BLOB_VERSION) {
        return false;   /* newer layouts get their own branch here; older ones are upgraded */
    }
    um98x_base_profile_t p;
    memset(&p, 0, sizeof(p));
    p.signal_group_master = blob_get_u8(&r);
    p.signal_group_slave = blob_get_u8(&r);
    p.base_mode = (um98x_base_mode_t)blob_get_u8(&r);
    p.survey_in_s = blob_get_u16(&r);
    p.fixed_lat_deg = blob_get_f64(&r);
    p.fixed_lon_deg = blob_get_f64(&r);
    p.fixed_height_m = blob_get_f64(&r);
    p.sat_view_output = blob_get_bool(&r);
    p.rtcm_count = blob_get_u8(&r);
    if (p.rtcm_count > UM98X_MAX_RTCM_MSGS) {
        return false;
    }
    for (size_t i = 0; i < p.rtcm_count; i++) {
        p.rtcm[i].msg = blob_get_u16(&r);
        p.rtcm[i].period_s = blob_get_u16(&r);
    }
    if (!blob_reader_ok(&r) || !um98x_profile_valid(&p)) {
        return false;
    }
    canonical(&p, out);
    return true;
}

uint32_t um98x_profile_hash(const um98x_base_profile_t *profile)
{
    if (!um98x_profile_valid(profile)) {
        return 0;
    }
    um98x_base_profile_t c;
    canonical(profile, &c);

    /* Only what reaches the receiver, in a fixed order; no blob version. */
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    blob_writer_t w;
    blob_writer_init(&w, buf, sizeof(buf));
    blob_put_u8(&w, PROFILE_HASH_FORMAT);
    blob_put_u8(&w, c.signal_group_master);
    blob_put_u8(&w, c.signal_group_slave);
    blob_put_u8(&w, (uint8_t)c.base_mode);
    if (c.base_mode == UM98X_BASE_SURVEY_IN) {
        blob_put_u16(&w, c.survey_in_s);
    } else {
        blob_put_f64(&w, c.fixed_lat_deg);
        blob_put_f64(&w, c.fixed_lon_deg);
        blob_put_f64(&w, c.fixed_height_m);
    }
    blob_put_bool(&w, c.sat_view_output);
    blob_put_u8(&w, c.rtcm_count);
    for (size_t i = 0; i < c.rtcm_count; i++) {
        blob_put_u16(&w, c.rtcm[i].msg);
        blob_put_u16(&w, c.rtcm[i].period_s);
    }
    return blob_writer_ok(&w) ? blob_hash32(buf, w.len) : 0;
}

bool um98x_position_valid(const um98x_base_position_t *pos)
{
    return pos != NULL && pos->source <= UM98X_POS_USER && pos->lat_deg >= -90.0 && pos->lat_deg <= 90.0 &&
           pos->lon_deg >= -180.0 && pos->lon_deg <= 180.0 && height_ok(pos->height_m) &&
           isfinite(pos->sigma_m) && pos->sigma_m >= 0.0f;
}

size_t um98x_position_encode(const um98x_base_position_t *pos, uint8_t *buf, size_t cap)
{
    if (!um98x_position_valid(pos)) {
        return 0;
    }
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, POSITION_BLOB_VERSION);
    blob_put_u8(&w, (uint8_t)pos->source);
    blob_put_f64(&w, pos->lat_deg);
    blob_put_f64(&w, pos->lon_deg);
    blob_put_f64(&w, pos->height_m);
    blob_put_f32(&w, pos->sigma_m);
    blob_put_i64(&w, pos->locked_unix);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool um98x_position_decode(const uint8_t *buf, size_t len, um98x_base_position_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) != POSITION_BLOB_VERSION) {
        return false;
    }
    um98x_base_position_t pos;
    memset(&pos, 0, sizeof(pos));
    pos.source = (um98x_pos_source_t)blob_get_u8(&r);
    pos.lat_deg = blob_get_f64(&r);
    pos.lon_deg = blob_get_f64(&r);
    pos.height_m = blob_get_f64(&r);
    pos.sigma_m = blob_get_f32(&r);
    pos.locked_unix = blob_get_i64(&r);
    if (!blob_reader_ok(&r) || !um98x_position_valid(&pos)) {
        return false;
    }
    *out = pos;
    return true;
}
