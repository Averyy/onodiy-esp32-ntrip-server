/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "net_core.h"
#include "unity.h"

TEST_CASE("network backoff: 1, 2, 4, 8, 16 s, then 30 s forever", "[net_core]")
{
    const uint32_t want[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (uint32_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], net_backoff_ms(i));
    }
    TEST_ASSERT_EQUAL_UINT32(NET_BACKOFF_CAP_MS, net_backoff_ms(31));
    TEST_ASSERT_EQUAL_UINT32(NET_BACKOFF_CAP_MS, net_backoff_ms(32));
    TEST_ASSERT_EQUAL_UINT32(NET_BACKOFF_CAP_MS, net_backoff_ms(UINT32_MAX));
}

TEST_CASE("network credential and hostname validation", "[net_core]")
{
    TEST_ASSERT_FALSE(net_ssid_valid(NULL));
    TEST_ASSERT_FALSE(net_ssid_valid(""));
    TEST_ASSERT_TRUE(net_ssid_valid("a"));
    TEST_ASSERT_TRUE(net_ssid_valid("12345678901234567890123456789012"));    /* 32 */
    TEST_ASSERT_FALSE(net_ssid_valid("123456789012345678901234567890123"));  /* 33 */
    TEST_ASSERT_TRUE(net_ssid_valid("caf\xC3\xA9 wifi"));                    /* UTF-8 bytes are fine */

    TEST_ASSERT_FALSE(net_password_valid(NULL));
    TEST_ASSERT_TRUE(net_password_valid(""));            /* open network */
    TEST_ASSERT_FALSE(net_password_valid("1234567"));
    TEST_ASSERT_TRUE(net_password_valid("12345678"));
    TEST_ASSERT_TRUE(net_password_valid("pass with spaces ~!"));
    TEST_ASSERT_FALSE(net_password_valid("tab\there12"));
    char p[80];
    memset(p, 'x', 63);
    p[63] = '\0';
    TEST_ASSERT_TRUE(net_password_valid(p));
    memset(p, 'a', 64);
    p[64] = '\0';
    TEST_ASSERT_TRUE(net_password_valid(p));              /* 64 hex digits: raw PSK */
    p[10] = 'g';
    TEST_ASSERT_FALSE(net_password_valid(p));             /* 64 characters but not hex */
    memset(p, 'a', 65);
    p[65] = '\0';
    TEST_ASSERT_FALSE(net_password_valid(p));

    TEST_ASSERT_FALSE(net_ap_password_valid(NULL));
    TEST_ASSERT_FALSE(net_ap_password_valid(""));
    TEST_ASSERT_FALSE(net_ap_password_valid("short"));
    TEST_ASSERT_TRUE(net_ap_password_valid("long enough"));
    memset(p, 'a', 64);
    p[64] = '\0';
    TEST_ASSERT_FALSE(net_ap_password_valid(p));          /* no raw PSK for the AP */

    TEST_ASSERT_TRUE(net_hostname_valid("onocoydiy"));
    TEST_ASSERT_TRUE(net_hostname_valid("base-01"));
    TEST_ASSERT_FALSE(net_hostname_valid(""));
    TEST_ASSERT_FALSE(net_hostname_valid("-base"));
    TEST_ASSERT_FALSE(net_hostname_valid("base-"));
    TEST_ASSERT_FALSE(net_hostname_valid("base_01"));
    TEST_ASSERT_FALSE(net_hostname_valid("base.local"));
    TEST_ASSERT_TRUE(net_hostname_valid("abcdefghijabcdefghijabcdefghij12"));   /* 32 */
    TEST_ASSERT_FALSE(net_hostname_valid("abcdefghijabcdefghijabcdefghij123")); /* 33 */
}

TEST_CASE("network setup AP name: prefix plus the last MAC bytes", "[net_core]")
{
    const uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x11, 0x3A, 0x7F };
    char out[33];
    TEST_ASSERT_EQUAL_size_t(14, net_ap_ssid("onocoydiy-", mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("onocoydiy-3A7F", out);

    TEST_ASSERT_EQUAL_size_t(4, net_ap_ssid(NULL, mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3A7F", out);

    /* A long prefix is cut so the digits survive the 32-byte limit. */
    TEST_ASSERT_EQUAL_size_t(32, net_ap_ssid("a-very-long-prefix-for-the-station-ap-", mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a-very-long-prefix-for-the-s3A7F", out);

    /* A small buffer: digits first, prefix as far as it fits. */
    char small[8];
    TEST_ASSERT_EQUAL_size_t(7, net_ap_ssid("onocoydiy-", mac, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("ono3A7F", small);
    TEST_ASSERT_EQUAL_size_t(0, net_ap_ssid("x", mac, small, 4));
}

static net_ap_input_t ap_in(void)
{
    net_ap_input_t in = {
        .settings_available = true,
        .sta_configured = true,
        .has_ip = true,
        .offline_s = 0,
        .since_ip_s = 1000,
        .fallback_after_s = 300,
        .recovery_lifetime_s = 900,
        .linger_s = 60,
    };
    return in;
}

TEST_CASE("network AP: setup without credentials, nothing in degraded mode", "[net_core]")
{
    bool expired = true;
    net_ap_input_t in = ap_in();
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
    TEST_ASSERT_FALSE(expired);

    in.sta_configured = false;
    in.has_ip = false;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_SETUP, net_ap_reasons(&in, &expired));

    /* Settings unreadable: no AP of its own, whatever else holds. */
    in.settings_available = false;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
    in.sta_configured = true;
    in.offline_s = 100000;
    in.reasons_now = NET_AP_FALLBACK;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));

    /* ...but the BOOT button opens the recovery AP. */
    in.recovery_requested = true;
    in.recovery_age_s = 0;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, net_ap_reasons(&in, &expired));
    TEST_ASSERT_FALSE(expired);
}

TEST_CASE("network AP: fallback after the offline time, held until the IP, then linger", "[net_core]")
{
    bool expired;
    net_ap_input_t in = ap_in();
    in.has_ip = false;
    in.offline_s = 299;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
    in.offline_s = 300;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));

    /* Up for fallback: stays while the station has no IP, however the
     * offline clock moves (new credentials restart nothing). */
    in.reasons_now = NET_AP_FALLBACK;
    in.offline_s = 5;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));

    /* The station got its IP: linger a minute, then down. */
    in.has_ip = true;
    in.since_ip_s = 0;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_LINGER, net_ap_reasons(&in, &expired));
    in.reasons_now = NET_AP_LINGER;
    in.since_ip_s = 59;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_LINGER, net_ap_reasons(&in, &expired));
    in.since_ip_s = 60;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));

    /* Lost again while lingering: held up again. */
    in.has_ip = false;
    in.offline_s = 1;
    in.reasons_now = NET_AP_LINGER;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));

    /* An AP that is up only for recovery is not held for the station. */
    in.reasons_now = NET_AP_RECOVERY;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
}

TEST_CASE("network AP: credentials entered on the setup AP keep it until the IP", "[net_core]")
{
    bool expired;
    net_ap_input_t in = ap_in();
    in.sta_configured = false;
    in.has_ip = false;
    in.offline_s = 10;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_SETUP, net_ap_reasons(&in, &expired));
    in.reasons_now = NET_AP_SETUP;

    /* Saved: configured now, still trying (or failing): the AP stays. */
    in.sta_configured = true;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));
    in.reasons_now = NET_AP_FALLBACK;
    in.offline_s = 3600;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));

    in.has_ip = true;
    in.since_ip_s = 3;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_LINGER, net_ap_reasons(&in, &expired));
}

TEST_CASE("network AP: recovery lifetime, extended while a client is connected", "[net_core]")
{
    bool expired;
    net_ap_input_t in = ap_in();
    in.recovery_requested = true;
    in.recovery_age_s = 899;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, net_ap_reasons(&in, &expired));
    TEST_ASSERT_FALSE(expired);

    in.recovery_age_s = 900;
    in.ap_clients = 1;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, net_ap_reasons(&in, &expired));
    TEST_ASSERT_FALSE(expired);

    in.ap_clients = 0;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
    TEST_ASSERT_TRUE(expired);

    /* Recovery and fallback together; recovery ends, fallback keeps it up. */
    in.has_ip = false;
    in.offline_s = 400;
    in.recovery_age_s = 10;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY | NET_AP_FALLBACK, net_ap_reasons(&in, &expired));
    in.recovery_age_s = 2000;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, &expired));
    TEST_ASSERT_TRUE(expired);

    /* NULL for the expiry flag is allowed. */
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, net_ap_reasons(&in, NULL));
}

TEST_CASE("network disconnect journal filter bounds a long outage", "[net_core]")
{
    net_drop_log_t s;
    net_drop_log_reset(&s);
    uint32_t t = UINT32_MAX - 100;   /* across the clock wrap */

    TEST_ASSERT_TRUE(net_drop_log_should(&s, true, 8, t));     /* the link dropped */
    TEST_ASSERT_FALSE(net_drop_log_should(&s, false, 8, t + 1));
    TEST_ASSERT_FALSE(net_drop_log_should(&s, false, 201, t + 30));  /* changed, but within a minute */
    TEST_ASSERT_TRUE(net_drop_log_should(&s, false, 201, t + 60));   /* changed, a minute later */
    TEST_ASSERT_FALSE(net_drop_log_should(&s, false, 201, t + 600)); /* same reason: 10 min after the last record */
    TEST_ASSERT_TRUE(net_drop_log_should(&s, false, 201, t + 660));

    /* A day of retries every 30 s with one reason: 144 records at most. */
    uint32_t logged = 0;
    for (uint32_t i = 1; i <= 2880; i++) {
        logged += net_drop_log_should(&s, false, 201, t + 660 + i * 30) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(144, logged);

    /* Alternating reasons every 30 s: a change is recorded only a minute or
     * more after the last record, so every third attempt (40 in an hour). */
    logged = 0;
    uint32_t base = t + 660 + 2880 * 30;
    for (uint32_t i = 1; i <= 120; i++) {
        logged += net_drop_log_should(&s, false, (i & 1) ? 15 : 201, base + i * 30) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(40, logged);

    /* After a successful connection the next failure is recorded at once. */
    net_drop_log_reset(&s);
    TEST_ASSERT_TRUE(net_drop_log_should(&s, false, 201, base + 5000));
}

static net_scan_entry_t scan_e(const char *ssid, int8_t rssi, uint8_t ch)
{
    net_scan_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
    e.rssi = rssi;
    e.channel = ch;
    e.secured = true;
    return e;
}

TEST_CASE("network scan list: one entry per SSID, strongest first, capped", "[net_core]")
{
    net_scan_entry_t list[4];
    size_t n = 0;
    net_scan_entry_t e = scan_e("home", -70, 1);
    n = net_scan_insert(list, n, 4, &e);
    e = scan_e("cafe", -50, 6);
    n = net_scan_insert(list, n, 4, &e);
    e = scan_e("", -30, 11);   /* hidden: skipped */
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("cafe", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("home", list[1].ssid);

    /* A stronger node of the same mesh replaces the weaker one and moves up. */
    e = scan_e("home", -40, 11);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("home", list[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-40, list[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(11, list[0].channel);
    /* A weaker one changes nothing. */
    e = scan_e("home", -80, 3);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_INT8(-40, list[0].rssi);

    e = scan_e("a", -60, 1);
    n = net_scan_insert(list, n, 4, &e);
    e = scan_e("b", -90, 1);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(4, n);
    /* Full: a weaker one is not taken, a stronger one evicts the weakest. */
    e = scan_e("c", -95, 1);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING("b", list[3].ssid);
    e = scan_e("d", -55, 1);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(4, n);
    const char *order[] = { "home", "cafe", "d", "a" };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_STRING(order[i], list[i].ssid);
    }
    for (int i = 1; i < 4; i++) {
        TEST_ASSERT_TRUE(list[i - 1].rssi >= list[i].rssi);
    }
    /* Same SSID already listed in a full list, now stronger: no eviction of others. */
    e = scan_e("a", -20, 1);
    n = net_scan_insert(list, n, 4, &e);
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING("a", list[0].ssid);
    TEST_ASSERT_EQUAL_STRING("d", list[3].ssid);
    TEST_ASSERT_EQUAL_size_t(0, net_scan_insert(list, 0, 0, &e));
}

TEST_CASE("network reboot age survives software resets, not power cycles", "[net_core]")
{
    net_rtc_t r;
    memset(&r, 0xA5, sizeof(r));   /* garbage after power-on */
    TEST_ASSERT_FALSE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* Power-on: nothing known. */
    net_rtc_on_boot(&r, false, NET_AGE_UNKNOWN);
    TEST_ASSERT_TRUE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* A software reset with a known session keeps "unknown". */
    net_rtc_on_boot(&r, false, 3600);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* The network ladder rebooted: age counts from this boot. */
    net_rtc_on_boot(&r, true, 7200);
    TEST_ASSERT_EQUAL_UINT32(0, net_rtc_age_now(&r, 0));
    TEST_ASSERT_EQUAL_UINT32(125, net_rtc_age_now(&r, 125));

    /* A crash 1000 s later, then a user reboot 500 s after that. */
    net_rtc_on_boot(&r, false, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000 + 20, net_rtc_age_now(&r, 20));
    net_rtc_on_boot(&r, false, 500);
    TEST_ASSERT_EQUAL_UINT32(1500, net_rtc_age_now(&r, 0));

    /* Saturates instead of wrapping into "unknown" or a small age. */
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN - 1, net_rtc_age_now(&r, UINT32_MAX - 10));

    /* Brownout / reset pin: session unknown -> age unknown (fresh budget). */
    net_rtc_on_boot(&r, false, NET_AGE_UNKNOWN);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));

    /* A damaged block is a fresh start. */
    net_rtc_on_boot(&r, true, 10);
    r.age_at_boot_s ^= 1;
    TEST_ASSERT_FALSE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));
    net_rtc_on_boot(&r, false, 100);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));
}
