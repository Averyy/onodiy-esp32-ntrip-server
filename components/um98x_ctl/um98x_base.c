/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_base.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "um98x_profile.h"

#define MAX_TOKENS 12
#define NUM_MAX    40   /* longest numeric token parsed */

typedef struct {
    const char *p;
    size_t len;
} tok_t;

static bool is_sep(char c)
{
    return c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n';
}

/* Split on spaces and commas. @return token count, or -1 if there are more
 * than max (an unexpected read-back compares as different). */
static int tokenize(const char *s, tok_t *t, int max)
{
    int n = 0;
    while (*s != '\0') {
        while (*s != '\0' && is_sep(*s)) {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        if (n == max) {
            return -1;
        }
        t[n].p = s;
        while (*s != '\0' && !is_sep(*s)) {
            s++;
        }
        t[n].len = (size_t)(s - t[n].p);
        n++;
    }
    return n;
}

static bool tok_is(const tok_t *t, const char *word)
{
    size_t n = strlen(word);
    if (t->len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = t->p[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

/* The whole token is a finite decimal number. */
static bool tok_number(const tok_t *t, double *out)
{
    char buf[NUM_MAX];
    if (t->len == 0 || t->len >= sizeof(buf)) {
        return false;
    }
    for (size_t i = 0; i < t->len; i++) {
        char c = t->p[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) {
            return false;   /* no exponents, hex or words */
        }
    }
    memcpy(buf, t->p, t->len);
    buf[t->len] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end != buf + t->len || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

/* The whole token is a plain unsigned integer. */
static bool tok_uint(const tok_t *t, uint32_t *out)
{
    if (t->len == 0 || t->len > 9) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < t->len; i++) {
        if (t->p[i] < '0' || t->p[i] > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(t->p[i] - '0');
    }
    *out = v;
    return true;
}

/* Tokens of a base-mode body: MODE BASE first. @return count, or -1 if not a base mode. */
static int base_tokens(const char *body, tok_t *t)
{
    if (body == NULL) {
        return -1;
    }
    int n = tokenize(body, t, MAX_TOKENS);
    if (n < 2 || !tok_is(&t[0], "MODE") || !tok_is(&t[1], "BASE")) {
        return -1;
    }
    return n;
}

static int find_time(const tok_t *t, int n)
{
    for (int i = 2; i < n; i++) {
        if (tok_is(&t[i], "TIME")) {
            return i;
        }
    }
    return -1;
}

size_t um98x_mode_command(const um98x_base_profile_t *p, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!um98x_profile_valid(p)) {
        return 0;
    }
    int n;
    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        n = snprintf(buf, cap, "MODE BASE %u TIME %u 0", (unsigned)p->station_id, (unsigned)p->survey_in_s);
    } else {
        double xyz[3];
        um98x_llh_to_ecef(p->fixed_lat_deg, p->fixed_lon_deg, p->fixed_height_m, xyz);
        n = snprintf(buf, cap, "MODE BASE %u %.4f %.4f %.4f", (unsigned)p->station_id, xyz[0], xyz[1], xyz[2]);
    }
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool um98x_mode_matches(const um98x_base_profile_t *p, const char *body)
{
    if (!um98x_profile_valid(p)) {
        return false;
    }
    tok_t t[MAX_TOKENS];
    int n = base_tokens(body, t);
    if (n < 0) {
        return false;
    }
    int k = find_time(t, n);
    uint32_t id;

    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        if (k < 0 || k > 3 || k + 1 >= n) {
            return false;
        }
        if (k == 3 && (!tok_uint(&t[2], &id) || id != p->station_id)) {
            return false;   /* another station ID echoed */
        }
        double secs;
        if (!tok_number(&t[k + 1], &secs) || secs != (double)p->survey_in_s) {
            return false;
        }
        double dist;
        if (k + 2 < n && tok_number(&t[k + 2], &dist) && dist != 0.0) {
            return false;   /* a survey that reuses an old position within a distance */
        }
        return true;
    }

    /* Fixed: every token after BASE is a number; three coordinates, or an
     * ID and three coordinates. */
    if (k >= 0) {
        return false;
    }
    int count = n - 2;
    double v[4];
    if (count != 3 && count != 4) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!tok_number(&t[2 + i], &v[i])) {
            return false;
        }
    }
    const double *c = v;
    if (count == 4) {
        if (!tok_uint(&t[2], &id) || id != p->station_id) {
            return false;
        }
        c = v + 1;
    }
    double xyz[3];
    um98x_llh_to_ecef(p->fixed_lat_deg, p->fixed_lon_deg, p->fixed_height_m, xyz);
    for (int i = 0; i < 3; i++) {
        if (!(fabs(c[i] - xyz[i]) <= UM98X_MODE_MATCH_TOL_M)) {
            return false;
        }
    }
    return true;
}

bool um98x_mode_is_survey(const char *body)
{
    tok_t t[MAX_TOKENS];
    int n = base_tokens(body, t);
    return n >= 0 && find_time(t, n) >= 0;
}

bool um98x_mode_is_fixed_base(const char *body)
{
    tok_t t[MAX_TOKENS];
    int n = base_tokens(body, t);
    if (n < 0 || find_time(t, n) >= 0) {
        return false;
    }
    int numbers = 0;
    for (int i = 2; i < n; i++) {
        double v;
        if (tok_number(&t[i], &v)) {
            numbers++;
        }
    }
    return numbers >= 3;
}

void um98x_survey_start(um98x_survey_t *s, int64_t now_ms, uint32_t survey_s)
{
    memset(s, 0, sizeof(*s));
    s->active = true;
    s->start_ms = now_ms;
    s->survey_s = survey_s;
}

void um98x_survey_on_mode(um98x_survey_t *s, const char *body)
{
    if (s->active && um98x_mode_is_fixed_base(body)) {
        s->complete = true;
    }
}

void um98x_survey_on_arp(um98x_survey_t *s, const int64_t ecef_01mm[3])
{
    if (!s->active) {
        return;
    }
    bool same = s->have_ref;
    for (int i = 0; same && i < 3; i++) {
        int64_t d = ecef_01mm[i] - s->ref_01mm[i];
        if (d > UM98X_SURVEY_STABLE_TOL_01MM || d < -UM98X_SURVEY_STABLE_TOL_01MM) {
            same = false;
        }
    }
    if (same) {
        if (s->stable_frames < UINT32_MAX) {
            s->stable_frames++;
        }
    } else {
        memcpy(s->ref_01mm, ecef_01mm, sizeof(s->ref_01mm));
        s->have_ref = true;
        s->stable_frames = 1;
    }
}

bool um98x_survey_done(const um98x_survey_t *s, int64_t now_ms)
{
    if (!s->active) {
        return false;
    }
    if (s->complete) {
        return true;
    }
    return now_ms - s->start_ms >= (int64_t)s->survey_s * 1000 && s->stable_frames >= UM98X_SURVEY_STABLE_FRAMES;
}

static int64_t window_end(const um98x_survey_t *s)
{
    return s->start_ms + ((int64_t)s->survey_s + UM98X_SURVEY_GRACE_S) * 1000;
}

bool um98x_survey_window_open(const um98x_survey_t *s, int64_t now_ms)
{
    return s->active && !um98x_survey_done(s, now_ms) && now_ms < window_end(s);
}

uint32_t um98x_survey_window_left_s(const um98x_survey_t *s, int64_t now_ms)
{
    if (!um98x_survey_window_open(s, now_ms)) {
        return 0;
    }
    return (uint32_t)((window_end(s) - now_ms + 999) / 1000);
}

bool um98x_arp_to_position(const int64_t ecef_01mm[3], um98x_base_position_t *out)
{
    double xyz[3] = { (double)ecef_01mm[0] * 1e-4, (double)ecef_01mm[1] * 1e-4, (double)ecef_01mm[2] * 1e-4 };
    um98x_base_position_t pos;
    memset(&pos, 0, sizeof(pos));
    if (!um98x_ecef_to_llh(xyz, &pos.lat_deg, &pos.lon_deg, &pos.height_m)) {
        return false;
    }
    pos.source = UM98X_POS_SURVEY;
    if (!um98x_position_valid(&pos)) {
        return false;
    }
    *out = pos;
    return true;
}

bool um98x_lock_profile(const um98x_base_profile_t *cur, const int64_t ecef_01mm[3], int64_t now_unix,
                        um98x_base_profile_t *next, um98x_base_position_t *pos)
{
    if (cur == NULL || ecef_01mm == NULL || next == NULL || pos == NULL) {
        return false;
    }
    um98x_base_position_t p;
    if (!um98x_arp_to_position(ecef_01mm, &p)) {
        return false;
    }
    p.locked_unix = now_unix;
    um98x_base_profile_t n = *cur;
    n.base_mode = UM98X_BASE_FIXED;
    n.fixed_lat_deg = p.lat_deg;
    n.fixed_lon_deg = p.lon_deg;
    n.fixed_height_m = p.height_m;
    if (!um98x_profile_valid(&n)) {
        return false;
    }
    *next = n;
    *pos = p;
    return true;
}

void um98x_resurvey_profile(const um98x_base_profile_t *cur, um98x_base_profile_t *next)
{
    um98x_base_profile_t n = *cur;
    n.base_mode = UM98X_BASE_SURVEY_IN;
    if (n.survey_in_s == 0 || n.survey_in_s > UM98X_SURVEY_MAX_S) {
        n.survey_in_s = UM98X_DEFAULT_SURVEY_S;
    }
    n.fixed_lat_deg = 0.0;
    n.fixed_lon_deg = 0.0;
    n.fixed_height_m = 0.0;
    *next = n;
}

bool um98x_arp_near_profile(const um98x_base_profile_t *p, const int64_t ecef_01mm[3], double tol_m)
{
    if (!um98x_profile_valid(p) || p->base_mode != UM98X_BASE_FIXED || ecef_01mm == NULL) {
        return false;
    }
    double xyz[3];
    um98x_llh_to_ecef(p->fixed_lat_deg, p->fixed_lon_deg, p->fixed_height_m, xyz);
    for (int i = 0; i < 3; i++) {
        if (!(fabs((double)ecef_01mm[i] * 1e-4 - xyz[i]) <= tol_m)) {
            return false;
        }
    }
    return true;
}
