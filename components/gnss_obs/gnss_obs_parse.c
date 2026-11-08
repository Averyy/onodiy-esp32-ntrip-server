/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Written from the public RTCM 10403.x MSM definitions and the public NMEA
 * 0183 satellite-in-view sentence.
 */
#include "gnss_obs_parse.h"

#include <string.h>

#include "gnss_framing.h"

/* ---- MSM signal cells ------------------------------------------------------ */

/* Unsigned field of n <= 32 bits starting at bit `pos` (bit 0 = MSB of p[0]). */
static uint32_t bits_u(const uint8_t *p, size_t pos, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, pos++) {
        v = (v << 1) | ((p[pos >> 3] >> (7u - (pos & 7u))) & 1u);
    }
    return v;
}

/* Bits per satellite and per cell, cell bits in front of the CNR, CNR width
 * and the factor from its unit to 1/16 dB-Hz. */
typedef struct {
    uint8_t sat_bits;
    uint8_t cell_bits;
    uint8_t pre_bits;
    uint8_t cnr_bits;
    uint8_t to_q4;
} msm_layout_t;

static const msm_layout_t k_layout[8] = {
    [4] = { 18, 48, 15 + 22 + 4 + 1, 6, 16 },
    [5] = { 36, 63, 15 + 22 + 4 + 1, 6, 16 },
    [6] = { 18, 65, 20 + 24 + 10 + 1, 10, 1 },
    [7] = { 36, 80, 20 + 24 + 10 + 1, 10, 1 },
};

bool obs_msm_cnr(const uint8_t *frame, size_t len, const gnss_rtcm3_msm_t *h, obs_cnr_t *out,
                 uint16_t *cell_q4)
{
    memset(out, 0, sizeof(*out));
    if (frame == NULL || h == NULL || h->msm < 4 || h->msm > 7 || (unsigned)h->sys >= GNSS_SYS_COUNT ||
        len < GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN || frame[0] != GNSS_RTCM3_PREAMBLE) {
        return false;
    }
    size_t plen = ((size_t)(frame[1] & 0x03u) << 8) | frame[2];
    if (len != plen + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN || plen < 2) {
        return false;
    }
    const uint8_t *p = frame + GNSS_RTCM3_HEADER_LEN;
    if (bits_u(p, 0, 12) != h->msg_type) {
        return false;
    }

    const msm_layout_t *l = &k_layout[h->msm];
    const unsigned nsat = h->num_sats;
    const unsigned nsig = h->num_sigs;
    const unsigned grid = nsat * nsig;
    const unsigned ncell = h->num_cells;
    if (grid > GNSS_RTCM3_MSM_MAX_CELLS || ncell > grid) {
        return false;
    }
    const size_t sig_start = GNSS_RTCM3_MSM_HEADER_BITS + grid + (size_t)nsat * l->sat_bits;
    if (plen * 8u < sig_start + (size_t)ncell * l->cell_bits) {
        return false;
    }

    /* Band of each sent signal, in ascending signal ID order. */
    uint8_t band_of[32];
    unsigned n = 0;
    for (unsigned id = 1; id <= 32; id++) {
        if (h->sig_mask & ((uint32_t)1 << (id - 1))) {
            band_of[n++] = (uint8_t)gnss_rtcm3_msm_band(h->sys, id);
        }
    }
    if (n != nsig) {
        return false;
    }

    obs_cnr_t r;
    memset(&r, 0, sizeof(r));
    const size_t cnr_start = sig_start + (size_t)ncell * l->pre_bits;
    unsigned k = 0;
    for (unsigned x = 0; x < grid; x++) {
        if (!(h->cell_mask & ((uint64_t)1 << x))) {
            continue;
        }
        if (k >= ncell) {
            return false;
        }
        uint16_t q4 = (uint16_t)(bits_u(p, cnr_start + (size_t)k * l->cnr_bits, l->cnr_bits) * l->to_q4);
        if (cell_q4 != NULL) {
            cell_q4[k] = q4;
        }
        uint8_t b = band_of[x % nsig];
        if (q4 != 0 && b < GNSS_BAND_COUNT) {
            r.sum_q4[b] += q4;
            r.cells[b]++;
        }
        k++;
    }
    if (k != ncell) {
        return false;
    }
    *out = r;
    return true;
}

/* ---- satellite-in-view sentences ------------------------------------------ */

#define GSV_MAX_FIELDS (3u + 4u * OBS_GSV_MAX_SATS + 1u)

typedef enum {
    TALKER_NUMBERED = 0,   /**< GP, GN: the constellation follows from the number range */
    TALKER_GLONASS,
    TALKER_GALILEO,
    TALKER_BEIDOU,
    TALKER_QZSS,
    TALKER_NAVIC,
} talker_t;

typedef struct {
    const char *s;
    size_t n;
} field_t;

static bool talker_of(char a, char b, talker_t *t)
{
    static const struct {
        char id[2];
        talker_t t;
    } k_talkers[] = {
        { { 'G', 'P' }, TALKER_NUMBERED }, { { 'G', 'N' }, TALKER_NUMBERED },
        { { 'G', 'L' }, TALKER_GLONASS },  { { 'G', 'A' }, TALKER_GALILEO },
        { { 'G', 'B' }, TALKER_BEIDOU },   { { 'B', 'D' }, TALKER_BEIDOU },
        { { 'G', 'Q' }, TALKER_QZSS },     { { 'Q', 'Z' }, TALKER_QZSS },
        { { 'G', 'I' }, TALKER_NAVIC },
    };
    for (size_t i = 0; i < sizeof(k_talkers) / sizeof(k_talkers[0]); i++) {
        if (k_talkers[i].id[0] == a && k_talkers[i].id[1] == b) {
            *t = k_talkers[i].t;
            return true;
        }
    }
    return false;
}

static bool in(int32_t v, int32_t lo, int32_t hi)
{
    return v >= lo && v <= hi;
}

/* NMEA satellite number -> constellation and MSM satellite number. */
static bool map_sat(talker_t t, int32_t n, gnss_sys_t *sys, int32_t *sat)
{
    switch (t) {
    case TALKER_NUMBERED:
        /* QZSS 193..202 wins over the BeiDou 201.. range of older receivers,
         * so BeiDou is taken only from its 401.. range here. */
        if (in(n, 1, 32)) {
            *sys = GNSS_SYS_GPS;
            *sat = n;
        } else if (in(n, 33, 64)) {
            *sys = GNSS_SYS_SBAS;
            *sat = n + 87 - 119;
        } else if (in(n, 65, 96)) {
            *sys = GNSS_SYS_GLONASS;
            *sat = n - 64;
        } else if (in(n, 120, 158)) {
            *sys = GNSS_SYS_SBAS;
            *sat = n - 119;
        } else if (in(n, 193, 202)) {
            *sys = GNSS_SYS_QZSS;
            *sat = n - 192;
        } else if (in(n, 301, 364)) {
            *sys = GNSS_SYS_GALILEO;
            *sat = n - 300;
        } else if (in(n, 401, 463)) {
            *sys = GNSS_SYS_BEIDOU;
            *sat = n - 400;
        } else {
            return false;
        }
        return true;
    case TALKER_GLONASS:
        *sys = GNSS_SYS_GLONASS;
        *sat = in(n, 65, 96) ? n - 64 : n;
        return in(*sat, 1, 32);
    case TALKER_GALILEO:
        *sys = GNSS_SYS_GALILEO;
        *sat = in(n, 301, 364) ? n - 300 : n;
        return in(*sat, 1, 64);
    case TALKER_BEIDOU:
        *sys = GNSS_SYS_BEIDOU;
        *sat = in(n, 401, 463) ? n - 400 : in(n, 201, 263) ? n - 200 : n;
        return in(*sat, 1, 63);
    case TALKER_QZSS:
        *sys = GNSS_SYS_QZSS;
        *sat = in(n, 193, 202) ? n - 192 : n;
        return in(*sat, 1, 10);
    case TALKER_NAVIC:
        *sys = GNSS_SYS_NAVIC;
        *sat = n;
        return in(*sat, 1, 64);
    default:
        return false;
    }
}

/*
 * Decimal field: optional sign (if allowed), 1..6 digits, optional fraction
 * (dropped). An empty field is valid and absent. false = malformed.
 */
static bool dec_field(field_t f, bool allow_sign, bool *present, int32_t *v)
{
    *present = false;
    *v = 0;
    if (f.n == 0) {
        return true;
    }
    size_t i = 0;
    bool neg = false;
    if (allow_sign && (f.s[0] == '-' || f.s[0] == '+')) {
        neg = (f.s[0] == '-');
        i = 1;
    }
    size_t digits = 0;
    int32_t acc = 0;
    for (; i < f.n && f.s[i] != '.'; i++) {
        if (f.s[i] < '0' || f.s[i] > '9' || ++digits > 6) {
            return false;
        }
        acc = acc * 10 + (f.s[i] - '0');
    }
    if (digits == 0) {
        return false;
    }
    if (i < f.n) {   /* '.' and the fraction: digits only */
        for (i++; i < f.n; i++) {
            if (f.s[i] < '0' || f.s[i] > '9') {
                return false;
            }
        }
    }
    *present = true;
    *v = neg ? -acc : acc;
    return true;
}

/* Signal ID: one or two hex digits. */
static bool hex_field(field_t f, uint8_t *v)
{
    if (f.n == 0 || f.n > 2) {
        return false;
    }
    unsigned acc = 0;
    for (size_t i = 0; i < f.n; i++) {
        char c = f.s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else {
            return false;
        }
        acc = acc * 16u + d;
    }
    *v = (uint8_t)acc;
    return true;
}

/* ---- position and error-statistics sentences ----------------------------- */

#define NMEA_MAX_FIELDS 20u

/* The fields after the address ("$..XXX,"), up to the checksum: count. */
static unsigned split_fields(const char *line, size_t len, size_t start, field_t *f, unsigned cap)
{
    unsigned nf = 0;
    for (size_t i = start; nf < cap; i++) {
        bool end = (i >= len || line[i] == '*');
        if (end || line[i] == ',') {
            f[nf].s = line + start;
            f[nf].n = i - start;
            nf++;
            start = i + 1;
        }
        if (end) {
            break;
        }
    }
    return nf;
}

/*
 * Decimal number: an optional sign (if allowed), 1..12 digits, an optional
 * '.' and fraction digits; the value times 10^scale, rounded half away from
 * zero at the first dropped digit. false for an empty or malformed field,
 * and for more than 18 digits in front of the dropped ones (int64 range).
 */
static bool fixed_field(field_t f, bool allow_sign, unsigned scale, int64_t *v)
{
    size_t i = 0;
    bool neg = false;
    if (f.n > 0 && allow_sign && (f.s[0] == '-' || f.s[0] == '+')) {
        neg = (f.s[0] == '-');
        i = 1;
    }
    int64_t acc = 0;
    size_t digits = 0;
    for (; i < f.n && f.s[i] != '.'; i++) {
        if (f.s[i] < '0' || f.s[i] > '9' || ++digits > 12) {
            return false;
        }
        acc = acc * 10 + (f.s[i] - '0');
    }
    if (digits == 0 || digits + scale > 18) {
        return false;
    }
    unsigned kept = 0;
    bool round_up = false;
    if (i < f.n) {
        size_t frac = 0;
        for (i++; i < f.n; i++, frac++) {
            if (f.s[i] < '0' || f.s[i] > '9') {
                return false;
            }
            if (kept < scale) {
                acc = acc * 10 + (f.s[i] - '0');
                kept++;
            } else if (frac == scale) {
                round_up = (f.s[i] >= '5');
            }
        }
    }
    for (; kept < scale; kept++) {
        acc *= 10;
    }
    if (round_up) {
        acc++;
    }
    *v = neg ? -acc : acc;
    return true;
}

/* A fixed-point field that must fit [lo, hi]. */
static bool fixed_in(field_t f, bool allow_sign, unsigned scale, int64_t lo, int64_t hi, int64_t *v)
{
    return fixed_field(f, allow_sign, scale, v) && *v >= lo && *v <= hi;
}

/* hhmmss with an optional fraction: ms since 00:00, the fraction cut to ms. */
static bool utc_field(field_t f, uint32_t *ms)
{
    if (f.n < 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (f.s[i] < '0' || f.s[i] > '9') {
            return false;
        }
    }
    unsigned hh = (unsigned)(f.s[0] - '0') * 10u + (unsigned)(f.s[1] - '0');
    unsigned mm = (unsigned)(f.s[2] - '0') * 10u + (unsigned)(f.s[3] - '0');
    unsigned ss = (unsigned)(f.s[4] - '0') * 10u + (unsigned)(f.s[5] - '0');
    if (hh > 23 || mm > 59 || ss > 60 || (ss == 60 && (hh != 23 || mm != 59))) {   /* 23:59:60: a leap second */
        return false;
    }
    unsigned frac_ms = 0;
    if (f.n > 6) {
        if (f.s[6] != '.') {
            return false;
        }
        unsigned scale = 100;
        for (size_t i = 7; i < f.n; i++) {
            if (f.s[i] < '0' || f.s[i] > '9') {
                return false;
            }
            frac_ms += (unsigned)(f.s[i] - '0') * scale;
            scale /= 10u;
        }
    }
    *ms = ((hh * 60u + mm) * 60u + ss) * 1000u + frac_ms;
    return true;
}

/* (d)ddmm.mmm... and its hemisphere: 1e-9 degree, positive = pos. */
static bool angle_field(field_t f, field_t hemi, char pos, char neg, unsigned max_deg, int64_t *ndeg)
{
    if (hemi.n != 1 || (hemi.s[0] != pos && hemi.s[0] != neg)) {
        return false;
    }
    int64_t min_e8;   /* degrees * 100 + minutes, in 1e-8 minute */
    if (!fixed_field(f, false, 8, &min_e8)) {
        return false;
    }
    /* The whole minutes as sent (the two digits in front of the point), below 60. */
    const char *dot = memchr(f.s, '.', f.n);
    size_t ints = dot != NULL ? (size_t)(dot - f.s) : f.n;
    if (ints < 3 || (unsigned)(f.s[ints - 2] - '0') * 10u + (unsigned)(f.s[ints - 1] - '0') > 59u) {
        return false;
    }
    int64_t deg = min_e8 / 10000000000LL;              /* the digits in front of the minutes */
    int64_t minutes_e8 = min_e8 % 10000000000LL;       /* mm.mmmmmmmm */
    if (minutes_e8 >= 6000000000LL) {
        /* 59.999999995 and up rounded to 60 minutes: the next degree. */
        deg += 1;
        minutes_e8 -= 6000000000LL;
    }
    if (deg > (int64_t)max_deg || (deg == (int64_t)max_deg && minutes_e8 != 0)) {
        return false;
    }
    /* 1e-8 minute = 1e-9 degree / 6 */
    int64_t v = deg * 1000000000LL + (minutes_e8 + 3) / 6;
    *ndeg = (hemi.s[0] == neg) ? -v : v;
    return true;
}

/* A unit field: exactly "M" (empty: a value without its unit is not taken). */
static bool unit_m(field_t f)
{
    return f.n == 1 && f.s[0] == 'M';
}

static void talker_copy(const char *line, char out[3])
{
    out[0] = line[1];
    out[1] = line[2];
    out[2] = '\0';
}

bool obs_gga_parse(const char *line, size_t len, obs_gga_t *out)
{
    memset(out, 0, sizeof(*out));
    talker_t talker;
    if (line == NULL || len < 7 || line[0] != '$' || memcmp(line + 3, "GGA,", 4) != 0 ||
        !talker_of(line[1], line[2], &talker)) {
        return false;
    }
    /* Fields after the address: 1 time, 2-5 latitude N/S longitude E/W,
     * 6 quality, 7 satellites used, 8 HDOP, 9-10 altitude M, 11-12 geoidal
     * separation M, 13 age of differential data, 14 reference station. */
    field_t f[NMEA_MAX_FIELDS];
    unsigned nf = split_fields(line, len, 7, f, NMEA_MAX_FIELDS);
    if (nf < 7) {
        return false;
    }
    bool p_q, p_n;
    int32_t q, n;
    if (!dec_field(f[5], false, &p_q, &q) || !dec_field(f[6], false, &p_n, &n) || !p_q || !p_n || !in(q, 0, 9) ||
        !in(n, 0, 99)) {
        return false;
    }
    obs_gga_t g;
    memset(&g, 0, sizeof(g));
    talker_copy(line, g.talker);
    g.quality = (uint8_t)q;
    g.satellites = (uint8_t)n;
    g.utc_valid = utc_field(f[0], &g.utc_ms);
    const bool fix = (g.quality != 0);
    g.position_valid = fix && angle_field(f[1], f[2], 'N', 'S', 90, &g.lat_ndeg) &&
                       angle_field(f[3], f[4], 'E', 'W', 180, &g.lon_ndeg);
    if (!g.position_valid) {
        g.lat_ndeg = 0;
        g.lon_ndeg = 0;
    }
    int64_t v;
    if (nf > 7 && fixed_in(f[7], false, 2, 0, UINT16_MAX, &v)) {
        g.hdop_valid = true;
        g.hdop_c = (uint16_t)v;
    }
    if (fix && nf > 9 && unit_m(f[9]) && fixed_in(f[8], true, 4, INT32_MIN, INT32_MAX, &v)) {
        g.altitude_valid = true;
        g.altitude_01mm = (int32_t)v;
    }
    if (fix && nf > 11 && unit_m(f[11]) && fixed_in(f[10], true, 4, INT32_MIN, INT32_MAX, &v)) {
        g.geoid_valid = true;
        g.geoid_01mm = (int32_t)v;
    }
    if (nf > 12 && fixed_in(f[12], false, 3, 0, UINT32_MAX, &v)) {
        g.diff_age_valid = true;
        g.diff_age_ms = (uint32_t)v;
    }
    if (nf > 13 && f[13].n > 0 && f[13].n <= 4 && fixed_in(f[13], false, 0, 0, 4095, &v) &&
        memchr(f[13].s, '.', f[13].n) == NULL) {
        g.diff_station_valid = true;
        g.diff_station = (uint16_t)v;
    }
    *out = g;
    return true;
}

bool obs_gst_parse(const char *line, size_t len, gnss_obs_gst_t *out)
{
    memset(out, 0, sizeof(*out));
    talker_t talker;
    if (line == NULL || len < 7 || line[0] != '$' || memcmp(line + 3, "GST,", 4) != 0 ||
        !talker_of(line[1], line[2], &talker)) {
        return false;
    }
    /* 1 time, 2 RMS, 3 semi-major, 4 semi-minor, 5 orientation, 6-8 latitude,
     * longitude and altitude standard deviations (metres, degrees). */
    field_t f[NMEA_MAX_FIELDS];
    unsigned nf = split_fields(line, len, 7, f, NMEA_MAX_FIELDS);
    if (nf < 8) {
        return false;
    }
    gnss_obs_gst_t g;
    memset(&g, 0, sizeof(g));
    talker_copy(line, g.talker);
    g.utc_valid = utc_field(f[0], &g.utc_ms);
    int64_t v;
    struct {
        unsigned idx;
        bool *valid;
        uint32_t *val;
    } const len_fields[] = {
        { 1, &g.rms_valid, &g.rms_01mm },       { 2, &g.major_valid, &g.major_01mm },
        { 3, &g.minor_valid, &g.minor_01mm },   { 5, &g.lat_sd_valid, &g.lat_sd_01mm },
        { 6, &g.lon_sd_valid, &g.lon_sd_01mm }, { 7, &g.alt_sd_valid, &g.alt_sd_01mm },
    };
    for (size_t i = 0; i < sizeof(len_fields) / sizeof(len_fields[0]); i++) {
        if (fixed_in(f[len_fields[i].idx], false, 4, 0, UINT32_MAX, &v)) {
            *len_fields[i].valid = true;
            *len_fields[i].val = (uint32_t)v;
        }
    }
    if (fixed_in(f[4], false, 2, 0, 36000, &v)) {
        g.orient_valid = true;
        g.orient_cdeg = (uint16_t)v;
    }
    *out = g;
    return true;
}

bool obs_gsv_parse(const char *line, size_t len, obs_gsv_t *out)
{
    memset(out, 0, sizeof(*out));
    talker_t talker;
    if (line == NULL || len < 7 || line[0] != '$' || memcmp(line + 3, "GSV,", 4) != 0 ||
        !talker_of(line[1], line[2], &talker)) {
        return false;
    }

    /* Fields after the address, up to the checksum. */
    field_t f[GSV_MAX_FIELDS];
    unsigned nf = 0;
    size_t start = 7;
    for (size_t i = start;; i++) {
        bool end = (i >= len || line[i] == '*');
        if (end || line[i] == ',') {
            if (nf >= GSV_MAX_FIELDS) {
                return false;
            }
            f[nf].s = line + start;
            f[nf].n = i - start;
            nf++;
            start = i + 1;
        }
        if (end) {
            break;
        }
    }
    if (nf < 3) {
        return false;
    }
    unsigned rest = nf - 3;
    unsigned blocks = rest / 4u;
    if (rest % 4u > 1u || blocks > OBS_GSV_MAX_SATS) {
        return false;
    }

    obs_gsv_t g;
    memset(&g, 0, sizeof(g));
    bool p_total, p_number, p_view;
    int32_t total, number, view;
    if (!dec_field(f[0], false, &p_total, &total) || !dec_field(f[1], false, &p_number, &number) ||
        !dec_field(f[2], false, &p_view, &view) || !p_total || !p_number || !in(total, 1, 99) ||
        !in(number, 1, total)) {
        return false;
    }
    g.total = (uint8_t)total;
    g.number = (uint8_t)number;
    g.in_view = (p_view && in(view, 0, 255)) ? (uint8_t)view : 0;
    if (rest % 4u == 1u && !hex_field(f[nf - 1], &g.signal_id)) {
        return false;
    }

    for (unsigned b = 0; b < blocks; b++) {
        const field_t *bf = &f[3 + 4 * b];
        bool p_sat, p_el, p_az, p_snr;
        int32_t num, el, az, snr;
        if (!dec_field(bf[0], false, &p_sat, &num) || !dec_field(bf[1], true, &p_el, &el) ||
            !dec_field(bf[2], false, &p_az, &az) || !dec_field(bf[3], false, &p_snr, &snr)) {
            return false;
        }
        gnss_sys_t sys;
        int32_t sat;
        if (!p_sat || !map_sat(talker, num, &sys, &sat)) {
            continue;   /* empty padding block, or a number we cannot place */
        }
        obs_gsv_sat_t *s = &g.sat[g.count++];
        s->sys = sys;
        s->sat = (uint8_t)sat;
        if (p_az && az == 360) {
            az = 0;
        }
        if (p_el && p_az && in(el, -90, 90) && in(az, 0, 359)) {
            s->has_pos = true;
            s->elevation_deg = (int8_t)el;
            s->azimuth_deg = (uint16_t)az;
        }
        s->cnr_dbhz = (p_snr && in(snr, 1, 99)) ? (uint8_t)snr : 0;
    }
    *out = g;
    return true;
}

/* ---- antenna and receiver descriptor (1033) ---------------------------- */

/* One counted string at *pos (count byte, then the characters); false when
 * the count is over OBS_1033_STR_MAX or the string ends beyond `bits`. */
static bool counted_str(const uint8_t *p, size_t bits, size_t *pos, char *out)
{
    if (*pos + 8u > bits) {
        return false;
    }
    unsigned n = bits_u(p, *pos, 8);
    *pos += 8u;
    if (n > OBS_1033_STR_MAX || *pos + (size_t)n * 8u > bits) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned c = bits_u(p, *pos, 8);
        out[i] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '?';
        *pos += 8u;
    }
    out[n] = '\0';
    return true;
}

bool obs_1033_parse(const uint8_t *frame, size_t len, obs_1033_t *out)
{
    memset(out, 0, sizeof(*out));
    if (frame == NULL || len < GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN || frame[0] != GNSS_RTCM3_PREAMBLE) {
        return false;
    }
    size_t plen = ((size_t)(frame[1] & 0x03u) << 8) | frame[2];
    if (len != plen + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN || plen < 3) {
        return false;
    }
    const uint8_t *p = frame + GNSS_RTCM3_HEADER_LEN;
    const size_t bits = plen * 8u;
    if (bits_u(p, 0, 12) != 1033u) {
        return false;
    }
    obs_1033_t r;
    memset(&r, 0, sizeof(r));
    r.station_id = (uint16_t)bits_u(p, 12, 12);
    size_t pos = 24;
    if (!counted_str(p, bits, &pos, r.antenna)) {
        return false;
    }
    if (pos + 8u > bits) {
        return false;
    }
    r.setup_id = (uint8_t)bits_u(p, pos, 8);
    pos += 8u;
    if (!counted_str(p, bits, &pos, r.antenna_serial) || !counted_str(p, bits, &pos, r.receiver_type) ||
        !counted_str(p, bits, &pos, r.receiver_fw) || !counted_str(p, bits, &pos, r.receiver_serial)) {
        return false;
    }
    *out = r;
    return true;
}
