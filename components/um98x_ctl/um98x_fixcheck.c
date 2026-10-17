/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_fixcheck.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "um98x_profile.h"   /* um98x_llh_to_ecef, the height range */

#define FIELDS   15u   /* fields read from the body */
#define NUM_MAX  32u   /* longest number parsed */

#define DEG2RAD (3.14159265358979323846 / 180.0)

_Static_assert(UM98X_FIX_SAMPLES == 3u, "the run's medians take three samples");

typedef struct {
    const char *p;
    size_t len;
} field_t;

static bool same_word(const field_t *f, const char *word)
{
    size_t n = strlen(word);
    if (f->len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = f->p[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

static bool num(const field_t *f, double *out)
{
    char buf[NUM_MAX + 1];
    if (f->len == 0 || f->len > NUM_MAX) {
        return false;
    }
    memcpy(buf, f->p, f->len);
    buf[f->len] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end != buf + f->len || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool count(const field_t *f, uint8_t *out)
{
    double v;
    if (!num(f, &v) || v < 0.0 || v > 255.0 || v != floor(v)) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static uint8_t pos_type(const field_t *f)
{
    if (same_word(f, "NONE")) {
        return UM98X_OWN_POS_NONE;
    }
    if (same_word(f, "FIXEDPOS")) {
        return UM98X_OWN_POS_FIXED;
    }
    if (same_word(f, "SINGLE")) {
        return UM98X_OWN_POS_SINGLE;
    }
    if (same_word(f, "PSRDIFF")) {
        return UM98X_OWN_POS_PSRDIFF;
    }
    if (same_word(f, "SBAS")) {
        return UM98X_OWN_POS_SBAS;
    }
    return UM98X_OWN_POS_OTHER;
}

bool um98x_own_fix_parse(const char *body, size_t len, um98x_own_fix_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (body == NULL) {
        return false;
    }
    field_t f[FIELDS];
    size_t n = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && n < FIELDS; i++) {
        if (i == len || body[i] == ',') {
            f[n].p = body + start;
            f[n].len = i - start;
            n++;
            start = i + 1;
        }
    }
    if (n < FIELDS) {
        return false;
    }
    um98x_own_fix_t r;
    memset(&r, 0, sizeof(r));
    r.computed = same_word(&f[0], "SOL_COMPUTED");
    r.pos_type = pos_type(&f[1]);
    /* f[6] is the datum (WGS84), f[10] the quoted base ID, f[11] and f[12] the ages: not used. */
    if (!num(&f[2], &r.lat_deg) || !num(&f[3], &r.lon_deg) || !num(&f[4], &r.msl_height_m) ||
        !num(&f[5], &r.separation_m) || !num(&f[7], &r.sd_lat_m) || !num(&f[8], &r.sd_lon_m) ||
        !num(&f[9], &r.sd_hgt_m) || !count(&f[13], &r.sats_tracked) || !count(&f[14], &r.sats_used)) {
        return false;
    }
    *out = r;
    return true;
}

bool um98x_own_fix_usable(const um98x_own_fix_t *f)
{
    if (f == NULL || !f->computed) {
        return false;
    }
    if (f->pos_type != UM98X_OWN_POS_SINGLE && f->pos_type != UM98X_OWN_POS_PSRDIFF && f->pos_type != UM98X_OWN_POS_SBAS) {
        return false;
    }
    if (f->sats_used < UM98X_FIX_MIN_SATS) {
        return false;
    }
    /* Every comparison below is false for NaN. */
    if (!(fabs(f->lat_deg) <= 90.0) || !(fabs(f->lon_deg) <= 180.0) || !(fabs(f->separation_m) <= UM98X_FIX_MAX_SEP_M)) {
        return false;
    }
    if (!(f->msl_height_m >= UM98X_HEIGHT_MIN_M - UM98X_FIX_MAX_SEP_M) ||
        !(f->msl_height_m <= UM98X_HEIGHT_MAX_M + UM98X_FIX_MAX_SEP_M)) {
        return false;
    }
    if (!(f->sd_lat_m >= 0.0) || !(f->sd_lon_m >= 0.0) || !(f->sd_hgt_m >= 0.0)) {
        return false;
    }
    return sqrt(f->sd_lat_m * f->sd_lat_m + f->sd_lon_m * f->sd_lon_m) <= UM98X_FIX_MAX_SD_H_M &&
           f->sd_hgt_m <= UM98X_FIX_MAX_SD_V_M;
}

bool um98x_fix_offset(double fixed_lat_deg, double fixed_lon_deg, double fixed_height_m, const um98x_own_fix_t *f,
                      um98x_fix_offset_t *out)
{
    if (out == NULL || !um98x_own_fix_usable(f)) {
        return false;
    }
    if (!(fabs(fixed_lat_deg) <= 90.0) || !(fabs(fixed_lon_deg) <= 180.0) || !(fixed_height_m >= UM98X_HEIGHT_MIN_M) ||
        !(fixed_height_m <= UM98X_HEIGHT_MAX_M)) {
        return false;
    }
    /* Horizontal: both points at the fixed height, the difference in the
     * fixed position's east/north plane. */
    double a[3];
    double b[3];
    um98x_llh_to_ecef(fixed_lat_deg, fixed_lon_deg, fixed_height_m, a);
    um98x_llh_to_ecef(f->lat_deg, f->lon_deg, fixed_height_m, b);
    double dx = b[0] - a[0];
    double dy = b[1] - a[1];
    double dz = b[2] - a[2];
    double lat = fixed_lat_deg * DEG2RAD;
    double lon = fixed_lon_deg * DEG2RAD;
    double east = -sin(lon) * dx + cos(lon) * dy;
    double north = -sin(lat) * cos(lon) * dx - sin(lat) * sin(lon) * dy + cos(lat) * dz;
    /* Vertical: against the receiver's ellipsoidal height (see the header). */
    out->horizontal_m = sqrt(east * east + north * north);
    out->up_m = fixed_height_m - (f->msl_height_m + f->separation_m);
    out->distance_m = sqrt(out->horizontal_m * out->horizontal_m + out->up_m * out->up_m);
    out->up_msl_m = fixed_height_m - f->msl_height_m;
    return true;
}

bool um98x_fix_beyond(const um98x_fix_offset_t *o, double limit_m)
{
    return o != NULL && (o->horizontal_m > limit_m || fabs(o->up_m) > limit_m);
}

bool um98x_fix_height_only(const um98x_fix_offset_t *o)
{
    return o != NULL && o->horizontal_m <= UM98X_FIX_LIMIT_M && fabs(o->up_m) > UM98X_FIX_LIMIT_M;
}

bool um98x_fix_sea_level(const um98x_fix_offset_t *o, bool held)
{
    if (o == NULL) {
        return false;
    }
    double band = held ? UM98X_FIX_SEA_HOLD_BAND_M : UM98X_FIX_SEA_BAND_M;
    double floor_m = held ? UM98X_FIX_SEA_HOLD_FLOOR_M : UM98X_FIX_SEA_FLOOR_M;
    /* Every comparison is false for NaN. */
    return o->horizontal_m <= UM98X_FIX_LIMIT_M && fabs(o->up_msl_m) <= band && fabs(o->up_m) > floor_m;
}

void um98x_fixchk_init(um98x_fixchk_t *c)
{
    memset(c, 0, sizeof(*c));
}

void um98x_fixchk_restart(um98x_fixchk_t *c)
{
    c->n = 0;
    memset(c->run, 0, sizeof(c->run));
}

static double median3(double a, double b, double c)
{
    if (a > b) {
        double t = a;
        a = b;
        b = t;
    }
    if (b > c) {
        b = c;
    }
    return a > b ? a : b;
}

uint8_t um98x_fixchk_feed(um98x_fixchk_t *c, const um98x_fix_offset_t *o)
{
    if (c == NULL || o == NULL) {
        return UM98X_FIX_NONE;
    }
    if (c->n == UM98X_FIX_SAMPLES) {
        memmove(&c->run[0], &c->run[1], sizeof(c->run[0]) * (UM98X_FIX_SAMPLES - 1u));
        c->n--;
    }
    c->run[c->n++] = *o;
    if (c->n < UM98X_FIX_SAMPLES) {
        return UM98X_FIX_NONE;
    }
    /* Once a warning stands, near needs the clear limit: no flapping around the limit. */
    bool warned = c->verdict == UM98X_FIX_FAR || c->verdict == UM98X_FIX_SEA_LEVEL;
    double near_limit = warned ? UM98X_FIX_CLEAR_M : UM98X_FIX_LIMIT_M;
    unsigned far = 0;
    unsigned near = 0;
    for (unsigned i = 0; i < UM98X_FIX_SAMPLES; i++) {
        if (um98x_fix_beyond(&c->run[i], UM98X_FIX_LIMIT_M)) {
            far++;
        } else if (!um98x_fix_beyond(&c->run[i], near_limit)) {
            near++;
        }
    }
    uint8_t v = far == UM98X_FIX_SAMPLES ? UM98X_FIX_FAR : near == UM98X_FIX_SAMPLES ? UM98X_FIX_NEAR : UM98X_FIX_NONE;
    if (v == UM98X_FIX_NONE) {
        return UM98X_FIX_NONE;
    }
    um98x_fix_offset_t m;
    m.horizontal_m = median3(c->run[0].horizontal_m, c->run[1].horizontal_m, c->run[2].horizontal_m);
    m.up_m = median3(c->run[0].up_m, c->run[1].up_m, c->run[2].up_m);
    m.distance_m = sqrt(m.horizontal_m * m.horizontal_m + m.up_m * m.up_m);
    m.up_msl_m = median3(c->run[0].up_msl_m, c->run[1].up_msl_m, c->run[2].up_msl_m);
    /* Far or near, the height alone may be the receiver's height above sea level. */
    if (um98x_fix_sea_level(&m, c->verdict == UM98X_FIX_SEA_LEVEL)) {
        v = UM98X_FIX_SEA_LEVEL;
    }
    c->verdict = v;
    c->shown = m;
    return v;
}
