/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "report_core.h"
#include <math.h>
#include <string.h>
#include "cJSON.h"

bool report_eligible(const ntrip_caster_cfg_t *c)
{
    return c != NULL && c->enabled && c->onocoy_stats && !c->test_onocoy &&
           c->password[0] != '\0' &&
           strspn(c->username, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == strlen(c->username) &&
           ntrip_server_onocoy_ready(c);
}

static bool date_ok(const char *s)
{
    if (s == NULL || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
    for (unsigned i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    }
    unsigned y = (unsigned)(s[0]-'0')*1000u + (unsigned)(s[1]-'0')*100u +
                 (unsigned)(s[2]-'0')*10u + (unsigned)(s[3]-'0');
    unsigned m = (unsigned)(s[5]-'0')*10u + (unsigned)(s[6]-'0');
    unsigned d = (unsigned)(s[8]-'0')*10u + (unsigned)(s[9]-'0');
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = y % 4u == 0 && (y % 100u != 0 || y % 400u == 0);
    return y >= 2025 && y <= 2199 && m >= 1 && m <= 12 && d >= 1 &&
           d <= days[m-1] + (m == 2 && leap ? 1u : 0u) &&
           (s[10] == '\0' || strcmp(s+10, "T00:00:00Z") == 0 || strcmp(s+10, "T00:00:00+00:00") == 0);
}

static bool number(const cJSON *row, const char *key, double max, double *out, uint8_t bit, uint8_t *known)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, key);
    if (v == NULL || cJSON_IsNull(v)) return true;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 || v->valuedouble > max) return false;
    *out = v->valuedouble;
    *known |= bit;
    return true;
}

bool report_parse(const char *json, size_t len, const char *start, const char *end,
                  onocoy_report_day_t *out, size_t *count)
{
    *count = 0;
    if (!date_ok(start) || !date_ok(end) || strcmp(start, end) >= 0 || json == NULL || len == 0) return false;
    const char *parsed_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len + 1, &parsed_end, true);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return false; }
    size_t n = 0;
    bool ok = true;
    const cJSON *v;
    cJSON_ArrayForEach(v, root) {
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(v, "date");
        if (!cJSON_IsObject(v) || !cJSON_IsString(date) || !date_ok(date->valuestring)) { ok = false; break; }
        onocoy_report_day_t d = {0};
        memcpy(d.date, date->valuestring, 10);
        if (strcmp(d.date, start) < 0 || strcmp(d.date, end) >= 0) continue;
        double uptime = 0;
        if (!number(v, "up_time_seconds", 86400, &uptime, ONOCOY_REPORT_UPTIME, &d.known) ||
            floor(uptime) != uptime ||
            !number(v, "latency_avg", 1e12, &d.latency_avg, ONOCOY_REPORT_LATENCY_AVG, &d.known) ||
            !number(v, "latency_max", 1e12, &d.latency_max, ONOCOY_REPORT_LATENCY_MAX, &d.known) ||
            !number(v, "rewards", 1e12, &d.rewards, ONOCOY_REPORT_REWARDS, &d.known)) { ok = false; break; }
        d.uptime_s = (uint32_t)uptime;
        if (n >= ONOCOY_REPORT_DAYS) { ok = false; break; }
        size_t at = 0;
        while (at < n && strcmp(out[at].date, d.date) > 0) at++;
        if (at < n && strcmp(out[at].date, d.date) == 0) { ok = false; break; }
        memmove(out + at + 1, out + at, (n - at) * sizeof(*out));
        out[at] = d;
        n++;
    }
    cJSON_Delete(root);
    if (!ok) { memset(out, 0, ONOCOY_REPORT_DAYS * sizeof(*out)); return false; }
    *count = n;
    return true;
}

uint32_t report_retry_s(bool success, int status, unsigned failures)
{
    if (success || (status >= 400 && status < 500)) return ONOCOY_REPORT_REFRESH_S;
    static const uint32_t delays[] = {900, 3600, 21600, 86400};
    return delays[failures < 4 ? failures : 3];
}

bool report_manual_allowed(onocoy_report_state_t state)
{
    return state == ONOCOY_REPORT_AUTH || state == ONOCOY_REPORT_ERROR;
}
