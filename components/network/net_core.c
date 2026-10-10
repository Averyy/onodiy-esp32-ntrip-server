/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "net_core.h"

#include <string.h>

/* ---- reconnect backoff ------------------------------------------------------------ */

uint32_t net_backoff_ms(uint32_t attempt)
{
    if (attempt >= 5) {
        return NET_BACKOFF_CAP_MS;
    }
    return 1000u << attempt;   /* 1, 2, 4, 8, 16 s */
}

/* ---- validation ------------------------------------------------------------------- */

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }
    return n;   /* max + 1 means "longer than max" */
}

static bool printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool net_ssid_valid(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    size_t n = bounded_len(ssid, 32);
    return n >= 1 && n <= 32;
}

bool net_password_valid(const char *pass)
{
    if (pass == NULL) {
        return false;
    }
    size_t n = bounded_len(pass, 64);
    if (n == 0) {
        return true;
    }
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            if (!is_hex(pass[i])) {
                return false;
            }
        }
        return true;
    }
    return n >= 8 && n <= 63 && printable(pass, n);
}

bool net_ap_password_valid(const char *pass)
{
    if (pass == NULL) {
        return false;
    }
    size_t n = bounded_len(pass, 63);
    return n >= 8 && n <= 63 && printable(pass, n);
}

bool net_hostname_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t n = bounded_len(name, 32);
    if (n < 1 || n > 32 || name[0] == '-' || name[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

size_t net_ap_ssid(const char *prefix, const uint8_t mac[6], char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    const size_t digits = 4;
    if (out == NULL || mac == NULL || out_size < digits + 1) {
        return 0;
    }
    size_t max_total = out_size - 1;
    if (max_total > 32) {
        max_total = 32;
    }
    size_t plen = (prefix != NULL) ? strlen(prefix) : 0;
    if (plen > max_total - digits) {
        plen = max_total - digits;
    }
    if (plen > 0) {
        memcpy(out, prefix, plen);
    }
    out[plen + 0] = hex[mac[4] >> 4];
    out[plen + 1] = hex[mac[4] & 0x0F];
    out[plen + 2] = hex[mac[5] >> 4];
    out[plen + 3] = hex[mac[5] & 0x0F];
    out[plen + digits] = '\0';
    return plen + digits;
}

/* ---- when the AP is up ------------------------------------------------------------- */

uint32_t net_ap_reasons(const net_ap_input_t *in, bool *recovery_expired)
{
    uint32_t r = 0;
    bool expired = false;
    if (in->recovery_requested) {
        if (in->recovery_age_s < in->recovery_lifetime_s || in->ap_clients > 0) {
            r |= NET_AP_RECOVERY;
        } else {
            expired = true;
        }
    }
    if (in->settings_available) {
        bool held = (in->reasons_now & (NET_AP_SETUP | NET_AP_FALLBACK | NET_AP_LINGER)) != 0;
        if (!in->sta_configured) {
            r |= NET_AP_SETUP;
        } else if (!in->has_ip) {
            if (held || in->offline_s >= in->fallback_after_s) {
                r |= NET_AP_FALLBACK;
            }
        } else if (held && in->since_ip_s < in->linger_s) {
            r |= NET_AP_LINGER;
        }
    }
    if (recovery_expired != NULL) {
        *recovery_expired = expired;
    }
    return r;
}

/* ---- disconnect journal filter -------------------------------------------------------- */

void net_drop_log_reset(net_drop_log_t *s)
{
    memset(s, 0, sizeof(*s));
}

bool net_drop_log_should(net_drop_log_t *s, bool was_connected, uint8_t reason, uint32_t now_s)
{
    bool log;
    if (was_connected || !s->any) {
        log = true;
    } else {
        uint32_t elapsed = now_s - s->last_s;
        log = (reason != s->last_reason && elapsed >= NET_DROP_LOG_CHANGE_S) || elapsed >= NET_DROP_LOG_REPEAT_S;
    }
    if (log) {
        s->any = true;
        s->last_reason = reason;
        s->last_s = now_s;
    }
    return log;
}

/* ---- cached scan list ----------------------------------------------------------------- */

size_t net_scan_insert(net_scan_entry_t *list, size_t count, size_t cap, const net_scan_entry_t *e)
{
    if (list == NULL || e == NULL || cap == 0 || e->ssid[0] == '\0') {
        return count;
    }
    if (count > cap) {
        count = cap;
    }
    for (size_t i = 0; i < count; i++) {
        if (strncmp(list[i].ssid, e->ssid, sizeof(list[i].ssid)) == 0) {
            if (e->rssi <= list[i].rssi) {
                return count;   /* this SSID is already listed with a stronger access point */
            }
            memmove(&list[i], &list[i + 1], (count - i - 1) * sizeof(list[0]));
            count--;
            break;
        }
    }
    if (count == cap) {
        if (e->rssi <= list[cap - 1].rssi) {
            return count;
        }
        count--;   /* drop the weakest */
    }
    size_t pos = 0;
    while (pos < count && list[pos].rssi >= e->rssi) {
        pos++;
    }
    memmove(&list[pos + 1], &list[pos], (count - pos) * sizeof(list[0]));
    list[pos] = *e;
    list[pos].ssid[sizeof(list[pos].ssid) - 1] = '\0';
    return count + 1;
}

/* ---- network-ladder reboot age across resets ---------------------------------------- */

static uint32_t rtc_check(const net_rtc_t *r)
{
    /* FNV-1a over the two data words */
    uint32_t h = 2166136261u;
    const uint32_t words[2] = { r->magic, r->age_at_boot_s };
    for (size_t w = 0; w < 2; w++) {
        for (unsigned b = 0; b < 4; b++) {
            h ^= (words[w] >> (8 * b)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

static uint32_t sat_add(uint32_t a, uint32_t b)
{
    uint32_t s = a + b;
    if (s < a || s >= NET_AGE_UNKNOWN) {
        return NET_AGE_UNKNOWN - 1;   /* very long ago, still known */
    }
    return s;
}

bool net_rtc_valid(const net_rtc_t *r)
{
    return r->magic == NET_RTC_MAGIC && r->check == rtc_check(r);
}

void net_rtc_seal(net_rtc_t *r)
{
    r->magic = NET_RTC_MAGIC;
    r->check = rtc_check(r);
}

void net_rtc_on_boot(net_rtc_t *r, bool prev_was_ladder_reboot, uint32_t prev_session_s)
{
    uint32_t age = NET_AGE_UNKNOWN;
    if (prev_was_ladder_reboot) {
        age = 0;
    } else if (net_rtc_valid(r) && r->age_at_boot_s != NET_AGE_UNKNOWN && prev_session_s != NET_AGE_UNKNOWN) {
        age = sat_add(r->age_at_boot_s, prev_session_s);
    }
    r->age_at_boot_s = age;
    net_rtc_seal(r);
}

uint32_t net_rtc_age_now(const net_rtc_t *r, uint32_t uptime_s)
{
    if (!net_rtc_valid(r) || r->age_at_boot_s == NET_AGE_UNKNOWN) {
        return NET_AGE_UNKNOWN;
    }
    return sat_add(r->age_at_boot_s, uptime_s);
}
