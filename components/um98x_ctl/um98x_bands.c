/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_bands.h"

#include <string.h>

#include "gnss_rtcm3.h"

#define MAX_BITS 6u

typedef struct {
    char token[3];                       /* the receiver's constellation token */
    const char *name;                    /* API name */
    uint8_t nbits;
    const char *signal[MAX_BITS];        /* NULL = no known signal on that bit */
    uint8_t band[MAX_BITS];              /* gnss_band_t of each signal */
} sys_def_t;

#define L1 GNSS_BAND_L1
#define L2 GNSS_BAND_L2
#define L5 GNSS_BAND_L5
#define L6 GNSS_BAND_L6
#define NB GNSS_BAND_NONE

static const sys_def_t k_sys[UM98X_BANDS_SYS] = {
    [UM98X_BSYS_GPS] = { "GP", "gps", 5, { "L1C/A", "L1C", "L2C", "L2P", "L5" }, { L1, L1, L2, L2, L5 } },
    /* G3 sits next to Galileo E5b, which the band buckets count as L2. */
    [UM98X_BSYS_GLONASS] = { "GL", "glonass", 3, { "G1", "G2", "G3" }, { L1, L2, L2 } },
    [UM98X_BSYS_GALILEO] = { "GA", "galileo", 4, { "E1", "E5a", "E5b", "E6" }, { L1, L5, L2, L6 } },
    [UM98X_BSYS_BEIDOU] = { "GB", "beidou", 6, { "B1I", "B2I", "B3I", "B1C", "B2a", "B2b" }, { L1, L2, L6, L1, L5, L2 } },
    [UM98X_BSYS_QZSS] = { "GQ", "qzss", 6, { "L1C/A", "L1C", "L2C", NULL, "L5", "L6" }, { L1, L1, L2, NB, L5, L6 } },
};

/* The main antenna input's signals per group: GPS, GLONASS, Galileo, BeiDou, QZSS. */
typedef struct {
    uint8_t master;
    uint32_t mask[UM98X_BANDS_SYS];
} expect_t;

static const expect_t k_expect[] = {
    { 4, { 0x1D, 0x03, 0x07, 0x07, 0x15 } },
    { 3, { 0x1D, 0x03, 0x0F, 0x2D, 0x35 } },
    { 7, { 0x1D, 0x03, 0x07, 0x3F, 0x15 } },
    { 5, { 0x0D, 0x03, 0x05, 0x07, 0x05 } },
};

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Comma-separated fields of a body, spaces around each dropped. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    bool done;      /* the last field (no comma after it) has been read */
} fields_t;

static bool next_field(fields_t *c, const char **f, size_t *n)
{
    if (c->done) {
        return false;
    }
    const char *start = c->s + c->pos;
    size_t left = c->len - c->pos;
    const char *comma = memchr(start, ',', left);
    size_t flen = comma != NULL ? (size_t)(comma - start) : left;
    if (comma != NULL) {
        c->pos += flen + 1u;
    } else {
        c->pos = c->len;
        c->done = true;
    }
    while (flen > 0 && is_space(start[0])) {
        start++;
        flen--;
    }
    while (flen > 0 && is_space(start[flen - 1])) {
        flen--;
    }
    *f = start;
    *n = flen;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decimal, or hexadecimal after "0x". */
static bool parse_num(const char *s, size_t n, uint32_t max, uint32_t *out)
{
    unsigned base = 10;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        n -= 2;
    }
    if (n == 0 || n > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int d = hex_value(s[i]);
        if (d < 0 || (unsigned)d >= base) {
            return false;
        }
        v = v * base + (unsigned)d;
    }
    if (v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int sys_of(const char *tok, size_t n)
{
    for (unsigned i = 0; i < UM98X_BANDS_SYS; i++) {
        if (n == 2 && (tok[0] & ~0x20) == k_sys[i].token[0] && (tok[1] & ~0x20) == k_sys[i].token[1]) {
            return (int)i;
        }
    }
    return -1;
}

bool um98x_bands_parse(const char *body, size_t len, um98x_bands_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->state = UM98X_BANDS_UNREAD;
    if (body == NULL) {
        return false;
    }
    um98x_bands_t b;
    memset(&b, 0, sizeof(b));
    fields_t c = { .s = body, .len = len, .pos = 0, .done = false };
    const char *f;
    size_t n;
    uint32_t group;
    uint32_t count;
    if (!next_field(&c, &f, &n) || !parse_num(f, n, UINT8_MAX, &group) || !next_field(&c, &f, &n) ||
        !parse_num(f, n, 16, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const char *tok;
        size_t tn;
        uint32_t mask;
        if (!next_field(&c, &tok, &tn) || tn == 0 || !next_field(&c, &f, &n) || !parse_num(f, n, UINT32_MAX, &mask)) {
            return false;
        }
        int sys = sys_of(tok, tn);
        if (sys >= 0) {
            b.present |= (uint8_t)(1u << sys);
            b.mask[sys] = mask;
        }
    }
    /* Nothing after the last entry but, at most, one empty field (a trailing comma). */
    if (next_field(&c, &f, &n) && (n != 0 || next_field(&c, &f, &n))) {
        return false;
    }
    b.state = UM98X_BANDS_OK;
    b.group = (uint8_t)group;
    *out = b;
    return true;
}

const char *um98x_bands_sys_name(unsigned sys)
{
    return sys < UM98X_BANDS_SYS ? k_sys[sys].name : NULL;
}

const char *um98x_bands_signal_name(unsigned sys, unsigned bit)
{
    if (sys >= UM98X_BANDS_SYS || bit >= k_sys[sys].nbits) {
        return NULL;
    }
    return k_sys[sys].signal[bit];
}

uint8_t um98x_bands_count(unsigned sys, uint32_t mask)
{
    if (sys >= UM98X_BANDS_SYS) {
        return 0;
    }
    uint32_t seen = 0;
    for (unsigned bit = 0; bit < k_sys[sys].nbits; bit++) {
        uint8_t band = k_sys[sys].band[bit];
        if ((mask & (1u << bit)) != 0 && band != (uint8_t)GNSS_BAND_NONE) {
            seen |= 1u << band;
        }
    }
    uint8_t n = 0;
    for (; seen != 0; seen &= seen - 1u) {
        n++;
    }
    return n;
}

bool um98x_bands_expected(uint8_t master, uint32_t out[UM98X_BANDS_SYS])
{
    memset(out, 0, sizeof(uint32_t) * UM98X_BANDS_SYS);
    for (size_t i = 0; i < sizeof(k_expect) / sizeof(k_expect[0]); i++) {
        if (k_expect[i].master == master) {
            memcpy(out, k_expect[i].mask, sizeof(k_expect[i].mask));
            return true;
        }
    }
    return false;
}

bool um98x_bands_match(uint8_t master, const um98x_bands_t *b, uint32_t missing[UM98X_BANDS_SYS])
{
    uint32_t want[UM98X_BANDS_SYS];
    (void)um98x_bands_expected(master, want);
    if (missing != NULL) {
        memset(missing, 0, sizeof(uint32_t) * UM98X_BANDS_SYS);
    }
    if (b == NULL || b->state != UM98X_BANDS_OK) {
        return false;
    }
    bool ok = b->group == master;
    for (unsigned s = 0; s < UM98X_BANDS_SYS; s++) {
        uint32_t lack = want[s] & ~b->mask[s];
        if (missing != NULL) {
            missing[s] = lack;
        }
        if (lack != 0) {
            ok = false;
        }
    }
    return ok;
}
