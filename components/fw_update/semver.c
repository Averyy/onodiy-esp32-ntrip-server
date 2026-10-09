/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "semver.h"

#include <stddef.h>
#include <string.h>

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* Parse a numeric component without leading zeros; advances *p. */
static bool parse_num(const char **p, uint32_t *out)
{
    const char *s = *p;
    if (!is_digit(*s)) {
        return false;
    }
    if (s[0] == '0' && is_digit(s[1])) {
        return false;
    }
    uint64_t v = 0;
    while (is_digit(*s)) {
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > UINT32_MAX) {
            return false;
        }
        s++;
    }
    *out = (uint32_t)v;
    *p = s;
    return true;
}

static bool valid_pre(const char *pre)
{
    size_t n = strlen(pre);
    if (n == 0) {
        return false;
    }
    const char *id = pre;
    for (size_t i = 0; i <= n; i++) {
        if (pre[i] == '.' || pre[i] == '\0') {
            size_t len = (size_t)(&pre[i] - id);
            if (len == 0) {
                return false;
            }
            bool numeric = true;
            for (size_t k = 0; k < len; k++) {
                numeric = numeric && is_digit(id[k]);
            }
            if (numeric && len > 1 && id[0] == '0') {
                return false;
            }
            id = &pre[i + 1];
        } else if (!is_ident_char(pre[i])) {
            return false;
        }
    }
    return true;
}

bool semver_parse(const char *s, semver_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    semver_t v;
    memset(&v, 0, sizeof(v));
    const char *p = s;
    if (*p == 'v') {
        p++;
    }
    if (!parse_num(&p, &v.major) || *p++ != '.' || !parse_num(&p, &v.minor) || *p++ != '.' ||
        !parse_num(&p, &v.patch)) {
        return false;
    }
    if (*p == '-') {
        p++;
        size_t n = strlen(p);
        if (n >= sizeof(v.pre)) {
            return false;
        }
        memcpy(v.pre, p, n + 1);
        if (!valid_pre(v.pre)) {
            return false;
        }
    } else if (*p != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static int cmp_u32(uint32_t a, uint32_t b)
{
    return (a > b) - (a < b);
}

/* Compare one dot-separated identifier of each pre-release string. */
static int cmp_ident(const char *a, size_t la, const char *b, size_t lb)
{
    bool na = true;
    bool nb = true;
    for (size_t i = 0; i < la; i++) {
        na = na && is_digit(a[i]);
    }
    for (size_t i = 0; i < lb; i++) {
        nb = nb && is_digit(b[i]);
    }
    if (na && nb) {
        if (la != lb) {
            return la < lb ? -1 : 1;   /* no leading zeros: longer is larger */
        }
        int c = memcmp(a, b, la);
        return (c > 0) - (c < 0);
    }
    if (na != nb) {
        return na ? -1 : 1;            /* numeric identifiers sort first */
    }
    size_t n = la < lb ? la : lb;
    int c = memcmp(a, b, n);
    if (c != 0) {
        return (c > 0) - (c < 0);
    }
    return (la > lb) - (la < lb);
}

int semver_compare(const semver_t *a, const semver_t *b)
{
    int c = cmp_u32(a->major, b->major);
    if (c == 0) {
        c = cmp_u32(a->minor, b->minor);
    }
    if (c == 0) {
        c = cmp_u32(a->patch, b->patch);
    }
    if (c != 0) {
        return c;
    }
    bool ra = a->pre[0] == '\0';
    bool rb = b->pre[0] == '\0';
    if (ra || rb) {
        return (ra && !rb) ? 1 : (!ra && rb) ? -1 : 0;
    }
    const char *pa = a->pre;
    const char *pb = b->pre;
    while (*pa != '\0' && *pb != '\0') {
        const char *ea = strchr(pa, '.');
        const char *eb = strchr(pb, '.');
        size_t la = ea ? (size_t)(ea - pa) : strlen(pa);
        size_t lb = eb ? (size_t)(eb - pb) : strlen(pb);
        c = cmp_ident(pa, la, pb, lb);
        if (c != 0) {
            return c;
        }
        pa += la + (ea ? 1 : 0);
        pb += lb + (eb ? 1 : 0);
        if (ea && !eb) {
            return 1;
        }
        if (!ea && eb) {
            return -1;
        }
        if (!ea && !eb) {
            return 0;
        }
    }
    return 0;
}
