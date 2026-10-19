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

/* The defaults a profile blob was written under: a byte appended after the
 * antenna fields (an older reader stops ahead of it). A blob without it was
 * written under the first. Decode reads what each later default changed into
 * a blob written before it (um98x_profile.h). */
#define PROFILE_DEFAULTS_1 1u   /* 1005 1033 1077 1087 1097 1117 1127 1230, all every 1 s; no satellite-in-view output */
#define PROFILE_DEFAULTS_2 2u   /* the first with 1137 (Onocoy's list); still no satellite-in-view output */
#define PROFILE_DEFAULTS   3u   /* the second with satellite-in-view output on (the sky map), written by encode */

/* Format of um98x_profile_hash(), frozen separately from the blob layout
 * once released: changing it makes every station's gnss/applied_hash
 * mismatch, so each one reconfigures its receiver once (and an unlocked
 * station surveys again). */
#define PROFILE_HASH_FORMAT 1

/* WGS84 ellipsoid. */
#define WGS84_A  6378137.0
#define WGS84_F  (1.0 / 298.257223563)
#define WGS84_E2 (WGS84_F * (2.0 - WGS84_F))
#define DEG_TO_RAD (3.14159265358979323846 / 180.0)

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
    out->station_id = p->station_id;
    out->com1_baud = p->com1_baud;
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
    /* Trailing spaces are dropped; without a model the serial number and
     * setup ID are never sent, so they are cleared. */
    size_t n = um98x_antenna_str_len(p->antenna_model);
    if (n > 0) {
        memcpy(out->antenna_model, p->antenna_model, n);
        size_t k = um98x_antenna_str_len(p->antenna_serial);
        memcpy(out->antenna_serial, p->antenna_serial, k);
        out->antenna_setup_id = p->antenna_setup_id;
    }
}

bool um98x_antenna_str_valid(const char *s, size_t cap)
{
    if (s == NULL) {
        return false;
    }
    for (size_t i = 0; i < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0') {
            return i <= UM98X_ANTENNA_STR_MAX;
        }
        if (c < 0x20 || c > 0x7E || c == '"') {
            return false;
        }
    }
    return false;   /* not terminated */
}

size_t um98x_antenna_str_len(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    size_t n = 0;
    while (n < UM98X_ANTENNA_STR_MAX && s[n] != '\0') {
        n++;
    }
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    return n;
}

/* The next character of s[0 .. n) in the compared form (leading spaces
 * skipped, a run of spaces read as one, trailing spaces dropped); '\0' at
 * the end. */
static char spaced_next(const char *s, size_t n, size_t *i)
{
    if (*i >= n) {
        return '\0';
    }
    char c = s[(*i)++];
    if (c == ' ') {
        while (*i < n && s[*i] == ' ') {
            (*i)++;
        }
        if (*i >= n) {
            return '\0';   /* only trailing spaces were left (cannot happen after um98x_antenna_str_len) */
        }
    }
    return c;
}

bool um98x_antenna_str_same(const char *a, const char *b)
{
    size_t na = um98x_antenna_str_len(a);
    size_t nb = um98x_antenna_str_len(b);
    size_t ia = 0;
    size_t ib = 0;
    while (ia < na && a[ia] == ' ') {
        ia++;
    }
    while (ib < nb && b[ib] == ' ') {
        ib++;
    }
    for (;;) {
        char ca = spaced_next(a, na, &ia);
        char cb = spaced_next(b, nb, &ib);
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
}

/* Onocoy's message list (its station connection guide): station messages and
 * MSM7 for every constellation, all at 1 Hz, in the order they are
 * configured. */
static const um98x_rtcm_out_t k_onocoy_set[] = {
    { 1005, 1 }, { 1033, 1 },
    { 1077, 1 }, { 1087, 1 }, { 1097, 1 }, { 1117, 1 }, { 1127, 1 }, { 1137, 1 },
    { 1230, 1 },
};
_Static_assert(sizeof(k_onocoy_set) / sizeof(k_onocoy_set[0]) <= UM98X_MAX_RTCM_MSGS,
               "default RTCM set exceeds UM98X_MAX_RTCM_MSGS");

/* The default set before 1137 was added (PROFILE_DEFAULTS_1). */
static const uint16_t k_old_set[] = { 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1230 };

/* The list is the old default set: the same messages (in any order, as the
 * UI compares them), each once, every one every 1 s. */
static bool is_old_default_set(const um98x_base_profile_t *p)
{
    const size_t n = sizeof(k_old_set) / sizeof(k_old_set[0]);
    if (p->rtcm_count != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (p->rtcm[i].period_s != 1) {
            return false;
        }
        size_t hits = 0;
        for (size_t k = 0; k < n; k++) {
            hits += p->rtcm[k].msg == k_old_set[i] ? 1u : 0u;
        }
        if (hits != 1) {
            return false;
        }
    }
    return true;
}

void um98x_profile_default(um98x_base_profile_t *out, uint16_t station_id)
{
    memset(out, 0, sizeof(*out));
    out->signal_group_master = 3;
    out->signal_group_slave = 6;
    out->base_mode = UM98X_BASE_SURVEY_IN;
    out->survey_in_s = UM98X_DEFAULT_SURVEY_S;
    out->station_id = (uint16_t)(station_id & UM98X_STATION_ID_MAX);
    out->com1_baud = UM98X_DEFAULT_COM1_BAUD;
    out->rtcm_count = (uint8_t)(sizeof(k_onocoy_set) / sizeof(k_onocoy_set[0]));
    memcpy(out->rtcm, k_onocoy_set, sizeof(k_onocoy_set));
    out->sat_view_output = true;    /* the sky map: satellite positions every 10 s */
    /* antenna: empty, so the receiver keeps its own descriptor */
}

uint16_t um98x_station_id_from_mac(const uint8_t mac[6])
{
    return (uint16_t)((((unsigned)mac[4] << 8) | mac[5]) & UM98X_STATION_ID_MAX);
}

bool um98x_baud_supported(uint32_t baud)
{
    static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i] == baud) {
            return true;
        }
    }
    return false;
}

bool um98x_profile_valid(const um98x_base_profile_t *p)
{
    if (p == NULL || p->rtcm_count > UM98X_MAX_RTCM_MSGS) {
        return false;
    }
    if (p->station_id > UM98X_STATION_ID_MAX || !um98x_baud_supported(p->com1_baud)) {
        return false;
    }
    /* NaN or infinity is invalid in every mode, used or not. */
    if (!isfinite(p->fixed_lat_deg) || !isfinite(p->fixed_lon_deg) || !isfinite(p->fixed_height_m)) {
        return false;
    }
    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        if (p->survey_in_s == 0 || p->survey_in_s > UM98X_SURVEY_MAX_S) {
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
    return um98x_antenna_str_valid(p->antenna_model, sizeof(p->antenna_model)) &&
           um98x_antenna_str_valid(p->antenna_serial, sizeof(p->antenna_serial));
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
    blob_put_u16(&w, p->station_id);
    blob_put_u32(&w, p->com1_baud);
    blob_put_u8(&w, p->rtcm_count);
    for (size_t i = 0; i < p->rtcm_count; i++) {
        blob_put_u16(&w, p->rtcm[i].msg);
        blob_put_u16(&w, p->rtcm[i].period_s);
    }
    /* Appended in the same version: a blob that ends before them decodes
     * with an empty antenna. */
    blob_put_str(&w, p->antenna_model);
    blob_put_str(&w, p->antenna_serial);
    blob_put_u8(&w, p->antenna_setup_id);
    /* Appended after them: whatever this holds is the one wanted. */
    blob_put_u8(&w, PROFILE_DEFAULTS);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool um98x_profile_decode(const uint8_t *buf, size_t len, um98x_base_profile_t *out)
{
    return um98x_profile_decode_ex(buf, len, out, NULL);
}

bool um98x_profile_decode_ex(const uint8_t *buf, size_t len, um98x_base_profile_t *out, unsigned *upgraded)
{
    if (upgraded != NULL) {
        *upgraded = 0;
    }
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
    p.station_id = blob_get_u16(&r);
    p.com1_baud = blob_get_u32(&r);
    p.rtcm_count = blob_get_u8(&r);
    if (p.rtcm_count > UM98X_MAX_RTCM_MSGS) {
        return false;
    }
    for (size_t i = 0; i < p.rtcm_count; i++) {
        p.rtcm[i].msg = blob_get_u16(&r);
        p.rtcm[i].period_s = blob_get_u16(&r);
    }
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        blob_get_str(&r, p.antenna_model, sizeof(p.antenna_model));
        blob_get_str(&r, p.antenna_serial, sizeof(p.antenna_serial));
        p.antenna_setup_id = blob_get_u8(&r);
    }
    uint8_t defaults = PROFILE_DEFAULTS_1;
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        defaults = blob_get_u8(&r);
    }
    if (!blob_reader_ok(&r) || !um98x_profile_valid(&p)) {
        return false;
    }
    unsigned up = 0;
    if (defaults == PROFILE_DEFAULTS_1 && is_old_default_set(&p)) {
        /* Written while the old set was the default, and still that set:
         * the default it stood for is Onocoy's list now. */
        p.rtcm_count = (uint8_t)(sizeof(k_onocoy_set) / sizeof(k_onocoy_set[0]));
        memcpy(p.rtcm, k_onocoy_set, sizeof(k_onocoy_set));
        up |= UM98X_PROFILE_UPGRADED_RTCM;
    }
    if ((defaults == PROFILE_DEFAULTS_1 || defaults == PROFILE_DEFAULTS_2) && !p.sat_view_output) {
        /* Off was the default then, a choice nobody made: the default it
         * stood for is on now (the sky map). */
        p.sat_view_output = true;
        up |= UM98X_PROFILE_UPGRADED_SKY;
    }
    canonical(&p, out);
    if (upgraded != NULL) {
        *upgraded = up;
    }
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
    blob_put_u32(&w, c.com1_baud);
    blob_put_u16(&w, c.station_id);
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
    /* The antenna only when the receiver gets one: a profile without a
     * model hashes as it did before the antenna fields existed. */
    if (c.antenna_model[0] != '\0') {
        blob_put_str(&w, c.antenna_model);
        blob_put_str(&w, c.antenna_serial);
        blob_put_u8(&w, c.antenna_setup_id);
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

void um98x_llh_to_ecef(double lat_deg, double lon_deg, double height_m, double xyz[3])
{
    double lat = lat_deg * DEG_TO_RAD;
    double lon = lon_deg * DEG_TO_RAD;
    double sin_lat = sin(lat);
    double cos_lat = cos(lat);
    double n = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);   /* prime vertical radius */
    xyz[0] = (n + height_m) * cos_lat * cos(lon);
    xyz[1] = (n + height_m) * cos_lat * sin(lon);
    xyz[2] = (n * (1.0 - WGS84_E2) + height_m) * sin_lat;
}

bool um98x_ecef_to_llh(const double xyz[3], double *lat_deg, double *lon_deg, double *height_m)
{
    const double x = xyz[0];
    const double y = xyz[1];
    const double z = xyz[2];
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
        return false;
    }
    const double p = sqrt(x * x + y * y);
    if (sqrt(p * p + z * z) < 100000.0) {
        return false;   /* nowhere near the surface: no meaningful geodetic position */
    }
    /* Fixed-point iteration on the latitude; the height uses the form that
     * stays exact at the poles (no division by cos(lat)). Converges to
     * double precision in a handful of steps for any height near the
     * surface. */
    double lat = atan2(z, p * (1.0 - WGS84_E2));
    for (int i = 0; i < 20; i++) {
        double sin_lat = sin(lat);
        double n = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
        double next = atan2(z + WGS84_E2 * n * sin_lat, p);
        if (fabs(next - lat) < 1e-15) {
            lat = next;
            break;
        }
        lat = next;
    }
    double sin_lat = sin(lat);
    double cos_lat = cos(lat);
    double n = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
    *lat_deg = lat / DEG_TO_RAD;
    *lon_deg = (p > 0.0) ? atan2(y, x) / DEG_TO_RAD : 0.0;
    *height_m = p * cos_lat + z * sin_lat - WGS84_A * WGS84_A / n;
    return true;
}
