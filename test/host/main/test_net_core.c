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

TEST_CASE("network RSSI: a reading is -127..-1 dBm, anything else is unknown", "[net_core]")
{
    TEST_ASSERT_FALSE(net_rssi_known(-128));   /* the driver's "no signal" */
    TEST_ASSERT_FALSE(net_rssi_known(0));
    TEST_ASSERT_FALSE(net_rssi_known(1));
    TEST_ASSERT_FALSE(net_rssi_known(127));
    TEST_ASSERT_TRUE(net_rssi_known(-1));
    TEST_ASSERT_TRUE(net_rssi_known(-67));
    TEST_ASSERT_TRUE(net_rssi_known(-127));
}

TEST_CASE("network IPv6: only fe80::/10 is link-local", "[net_core]")
{
    static const uint8_t ll[] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x01 };
    static const uint8_t ll_top[] = { 0xFE, 0xBF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };   /* fe80::/10's end */
    static const uint8_t ula[] = { 0xFD, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };              /* fd40:: */
    static const uint8_t site[] = { 0xFE, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };            /* fec0:: (slirp) */
    static const uint8_t doc[] = { 0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };       /* 2001:db8:: */
    static const uint8_t any[16] = { 0 };                                                                 /* :: */
    static const uint8_t fe00[] = { 0xFE, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    TEST_ASSERT_TRUE(net_ip6_is_link_local(ll));
    TEST_ASSERT_TRUE(net_ip6_is_link_local(ll_top));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(ula));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(site));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(doc));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(any));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(fe00));
    TEST_ASSERT_FALSE(net_ip6_is_link_local(NULL));
}

TEST_CASE("network TIME_WAIT: only a reset with exactly RCV.NXT ends it", "[net_core]")
{
    /* The station's bare ACK to a SYN on a port it held (Ack=1040052053) and
     * the client's reset to it (Seq=1040052053): a capture on a test board. */
    const uint32_t rcv_nxt = 1040052053u;
    const uint8_t ack = 0x10, fin = 0x01, psh = 0x08;
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST | ack, rcv_nxt, rcv_nxt, false));
    /* RFC 5961: anything but an exact RCV.NXT leaves it (lwIP then ignores the reset). */
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt + 1u, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt - 1u, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt + 100u, rcv_nxt, false));    /* inside the window */
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, 0, rcv_nxt, false));
    /* Not a reset, or a reset with SYN: the SYN itself, the client's FIN or ACK again. */
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_SYN, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_SYN | NET_TCP_RST, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(fin | ack, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(ack, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(psh | ack, rcv_nxt, rcv_nxt, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(0, rcv_nxt, rcv_nxt, false));
    /* Sequence numbers wrap. */
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST, 0, 0, false));
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST, UINT32_MAX, UINT32_MAX, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, UINT32_MAX, 0, false));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, 0, UINT32_MAX, false));
}

TEST_CASE("network TIME_WAIT: a connection a socket still holds is left to lwIP", "[net_core]")
{
    /* The QEMU smoke run that found it: a refused 16 KB upload, the station's
     * lingering close (shutdown(SHUT_WR), then the drain), the client's FIN
     * (TIME_WAIT, the socket still open) and 1.5 ms later the client's reset
     * with exactly RCV.NXT (Seq=2512492). Ending that TIME_WAIT freed the pcb
     * under the socket. */
    const uint32_t rcv_nxt = 2512492u;
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt, rcv_nxt, true));
    TEST_ASSERT_FALSE(net_tw_reset_ends(NET_TCP_RST | 0x10, rcv_nxt, rcv_nxt, true));
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt, rcv_nxt, false));   /* once the socket is closed */
    /* Nor does a sweep free it, however long its TIME_WAIT is over. */
    for (uint32_t d = 0; d <= 100u; d++) {
        TEST_ASSERT_FALSE(net_tw_free_now(1000u + d, 1000u, 20u, true));
        TEST_ASSERT_EQUAL(d > 20u, net_tw_free_now(1000u + d, 1000u, 20u, false));
    }
}

TEST_CASE("network TIME_WAIT: a half-closed socket's connection is watched until the client's FIN", "[net_core]")
{
    /* A socket that shut only its sending side holds it, our FIN is out, the client's is not in. */
    TEST_ASSERT_TRUE(net_tw_watch(NET_TCP_FIN_WAIT, true));
    /* Closed whole: the socket let go of it in its close(). */
    TEST_ASSERT_FALSE(net_tw_watch(NET_TCP_FIN_WAIT, false));
    /* ESTABLISHED, CLOSE_WAIT, LAST_ACK: lwIP itself tells the socket when it frees one of those. */
    TEST_ASSERT_FALSE(net_tw_watch(NET_TCP_OTHER, true));
    /* Watched already when it got there (the FIN came through the watched callback). */
    TEST_ASSERT_FALSE(net_tw_watch(NET_TCP_CLOSING, true));
    TEST_ASSERT_FALSE(net_tw_watch(NET_TCP_TIME_WAIT, true));
}

TEST_CASE("network TIME_WAIT: the socket lets go once the client's FIN reached it in CLOSING or TIME_WAIT",
          "[net_core]")
{
    /* Each way lwIP's tcp_input calls the socket layer's receive callback on a
     * connection whose sending side alone the socket shut (tcp_in.c: the state
     * is the one tcp_process left; tcp.c, tcp_process_refused_data). */
    const struct {
        net_tcp_state_t st;
        bool fin;
        bool taken;
        bool let_go;
        bool drop;
        const char *path;
    } cases[] = {
        { NET_TCP_FIN_WAIT, false, true, false, false, "data before the client's FIN: the socket reads on" },
        { NET_TCP_FIN_WAIT, false, false, false, false, "data refused before the FIN: lwIP offers it again" },
        { NET_TCP_TIME_WAIT, false, true, false, false, "FIN_WAIT_2, data with the FIN: the FIN comes next" },
        { NET_TCP_TIME_WAIT, true, true, true, false, "FIN_WAIT_1 or FIN_WAIT_2 to TIME_WAIT: the FIN" },
        { NET_TCP_TIME_WAIT, true, false, true, false, "the FIN the socket's queue had no room for" },
        { NET_TCP_TIME_WAIT, false, false, true, true, "data refused in TIME_WAIT: the FIN never comes" },
        { NET_TCP_CLOSING, true, true, true, false, "the FINs crossed: the client's FIN in FIN_WAIT_1" },
        { NET_TCP_CLOSING, true, false, true, false, "the same, the socket's queue full" },
        { NET_TCP_CLOSING, false, true, false, false, "refused data offered again in CLOSING: its FIN follows" },
        { NET_TCP_CLOSING, false, false, true, false, "refused again: lwIP drops it on the way to TIME_WAIT" },
        { NET_TCP_OTHER, true, true, false, false, "not a state a watched connection gets to with a FIN" },
        { NET_TCP_OTHER, false, false, false, false, "nor with data" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].let_go, net_tw_let_go(cases[i].st, cases[i].fin, cases[i].taken),
                                  cases[i].path);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].drop, net_tw_drop_refused(cases[i].st, cases[i].fin, cases[i].taken),
                                  cases[i].path);
        /* Dropped only where the socket lets go: nothing is left that would read it. */
        TEST_ASSERT_TRUE_MESSAGE(!cases[i].drop || cases[i].let_go, cases[i].path);
    }
}

TEST_CASE("network TIME_WAIT: one a socket still holds is let go at its next segment", "[net_core]")
{
    TEST_ASSERT_TRUE(net_tw_let_go_now(NET_TCP_TIME_WAIT, true));
    TEST_ASSERT_FALSE(net_tw_let_go_now(NET_TCP_TIME_WAIT, false));
    TEST_ASSERT_FALSE(net_tw_let_go_now(NET_TCP_CLOSING, true));    /* lwIP tells the socket if it frees one */
    TEST_ASSERT_FALSE(net_tw_let_go_now(NET_TCP_FIN_WAIT, true));
    TEST_ASSERT_FALSE(net_tw_let_go_now(NET_TCP_OTHER, true));
    /* Let go, the client's exact reset ends it (the drain check's reset, 1.5 ms after the FIN). */
    const uint32_t rcv_nxt = 2512492u;
    TEST_ASSERT_TRUE(net_tw_reset_ends(NET_TCP_RST, rcv_nxt, rcv_nxt, false));
}

/* lwIP's test for a TIME_WAIT that is over, as tcp.c (tcp_slowtmr) writes it. */
static bool tw_over(uint32_t ticks, uint32_t start, uint32_t tw_ticks)
{
    return (uint32_t)(ticks - start) > tw_ticks;
}

TEST_CASE("network TIME_WAIT: over exactly when lwIP's timer takes it as over", "[net_core]")
{
    const uint32_t tw = 20;
    const uint32_t starts[] = { 0, 7, 0x7FFFFFFFu, UINT32_MAX - 10u, UINT32_MAX };
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        for (uint32_t d = 0; d <= 2u * tw; d++) {
            const uint32_t now = starts[i] + d;   /* wraps */
            TEST_ASSERT_EQUAL(tw_over(now, starts[i], tw), net_tw_over(now, starts[i], tw));
            TEST_ASSERT_EQUAL(d > tw, net_tw_over(now, starts[i], tw));
        }
    }
}

TEST_CASE("network TIME_WAIT: the moved start is over at lwIP's next tick, also across the wrap", "[net_core]")
{
    const uint32_t tw = 2u * 5000u / 500u;   /* 2 x MSL (5 s) in 500 ms ticks: 20 */
    const uint32_t nows[] = { 0, 1, 19, 20, 21, 12345, 0x80000000u, UINT32_MAX - 20u, UINT32_MAX - 1u, UINT32_MAX };
    for (size_t i = 0; i < sizeof(nows) / sizeof(nows[0]); i++) {
        const uint32_t now = nows[i];
        /* lwIP's own start, the tick TIME_WAIT began, is over 2 x MSL later and not sooner. */
        TEST_ASSERT_FALSE(tw_over(now + tw, now, tw));
        TEST_ASSERT_TRUE(tw_over(now + tw + 1u, now, tw));
        /* The moved start is over now and at every following tick, counted before
         * the comparison (tcp_slowtmr's ++tcp_ticks) or not. */
        const uint32_t start = net_tw_over_start(now, tw);
        TEST_ASSERT_TRUE(tw_over(now, start, tw));
        TEST_ASSERT_TRUE(tw_over(now + 1u, start, tw));
        TEST_ASSERT_TRUE(tw_over(now + 2u, start, tw));
        TEST_ASSERT_TRUE(tw_over(now + 1000u, start, tw));
        TEST_ASSERT_EQUAL_UINT32(tw + 1u, now - start);
    }
    /* Any TIME_WAIT length, including none. */
    TEST_ASSERT_TRUE(tw_over(7, net_tw_over_start(7, 0), 0));
    TEST_ASSERT_TRUE(tw_over(7, net_tw_over_start(7, 240), 240));   /* lwIP's default MSL, 60 s */
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

    TEST_ASSERT_TRUE(net_hostname_valid("onodiy"));
    TEST_ASSERT_TRUE(net_hostname_valid("base-01"));
    TEST_ASSERT_FALSE(net_hostname_valid(""));
    TEST_ASSERT_FALSE(net_hostname_valid("-base"));
    TEST_ASSERT_FALSE(net_hostname_valid("base-"));
    TEST_ASSERT_FALSE(net_hostname_valid("base_01"));
    TEST_ASSERT_FALSE(net_hostname_valid("base.local"));
    TEST_ASSERT_TRUE(net_hostname_valid("abcdefghijabcdefghijabcdefghij12"));   /* 32 */
    TEST_ASSERT_FALSE(net_hostname_valid("abcdefghijabcdefghijabcdefghij123")); /* 33 */
}

TEST_CASE("network credentials: a stored SSID whose password cannot be read fails closed", "[net_core]")
{
    TEST_ASSERT_EQUAL(NET_CRED_OK, net_sta_credentials(true, "home", true, "secret-pass"));
    TEST_ASSERT_EQUAL(NET_CRED_OK, net_sta_credentials(true, "cafe", true, ""));   /* stored as open */
    /* No (valid) SSID: simply not configured. */
    TEST_ASSERT_EQUAL(NET_CRED_NONE, net_sta_credentials(false, "", false, ""));
    TEST_ASSERT_EQUAL(NET_CRED_NONE, net_sta_credentials(true, "", true, ""));
    /* SSID read, password not: never an open network of the same name
     * (a damaged entry reads as "not found", so that fails closed too). */
    TEST_ASSERT_EQUAL(NET_CRED_UNREADABLE, net_sta_credentials(true, "home", false, ""));
    TEST_ASSERT_EQUAL(NET_CRED_UNREADABLE, net_sta_credentials(true, "home", true, "short"));
}

TEST_CASE("network setup AP name: prefix plus the last MAC bytes", "[net_core]")
{
    const uint8_t mac[6] = { 0x24, 0x6F, 0x28, 0x11, 0x3A, 0x7F };
    char out[33];
    TEST_ASSERT_EQUAL_size_t(11, net_ap_ssid("onodiy-", mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("onodiy-3A7F", out);

    TEST_ASSERT_EQUAL_size_t(4, net_ap_ssid(NULL, mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3A7F", out);

    /* A long prefix is cut so the digits survive the 32-byte limit. */
    TEST_ASSERT_EQUAL_size_t(32, net_ap_ssid("a-very-long-prefix-for-the-station-ap-", mac, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a-very-long-prefix-for-the-s3A7F", out);

    /* A small buffer: digits first, prefix as far as it fits. */
    char small[8];
    TEST_ASSERT_EQUAL_size_t(7, net_ap_ssid("onodiy-", mac, small, sizeof(small)));
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

/* A boot whose RTC stamp lifecycle accepts (same image generation, here 7). */
static void on_boot(net_rtc_t *r, bool ladder, uint32_t prev_session_s)
{
    net_rtc_on_boot(r, true, 7u, ladder, prev_session_s);
}

TEST_CASE("network reboot age survives software resets, not power cycles", "[net_core]")
{
    net_rtc_t r;
    memset(&r, 0xA5, sizeof(r));   /* garbage after power-on */
    TEST_ASSERT_FALSE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* Power-on: nothing known. */
    on_boot(&r, false, NET_AGE_UNKNOWN);
    TEST_ASSERT_TRUE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* A software reset with a known session keeps "unknown". */
    on_boot(&r, false, 3600);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 10));

    /* The network ladder rebooted: age counts from this boot. */
    on_boot(&r, true, 7200);
    TEST_ASSERT_EQUAL_UINT32(0, net_rtc_age_now(&r, 0));
    TEST_ASSERT_EQUAL_UINT32(125, net_rtc_age_now(&r, 125));

    /* A crash 1000 s later, then a user reboot 500 s after that. */
    on_boot(&r, false, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000 + 20, net_rtc_age_now(&r, 20));
    on_boot(&r, false, 500);
    TEST_ASSERT_EQUAL_UINT32(1500, net_rtc_age_now(&r, 0));

    /* Saturates instead of wrapping into "unknown" or a small age. */
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN - 1, net_rtc_age_now(&r, UINT32_MAX - 10));

    /* Brownout / reset pin: session unknown -> age unknown (fresh budget). */
    on_boot(&r, false, NET_AGE_UNKNOWN);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));

    /* A damaged block is a fresh start. */
    on_boot(&r, true, 10);
    r.age_at_boot_s ^= 1;
    TEST_ASSERT_FALSE(net_rtc_valid(&r));
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));
    on_boot(&r, false, 100);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&r, 5));
}

TEST_CASE("network reboot age: a block from before another image's run starts afresh", "[net_core]")
{
    net_rtc_t r;
    memset(&r, 0, sizeof(r));
    net_rtc_on_boot(&r, true, 3u, true, 100);         /* image A, generation 3: ladder reboot */
    TEST_ASSERT_EQUAL_UINT32(3u, r.stamp);
    TEST_ASSERT_EQUAL_UINT32(50, net_rtc_age_now(&r, 50));

    /* Image B ran (generation 4) and the station came back to A: A's block
     * is intact, but lifecycle does not accept its stamp. */
    net_rtc_t stale = r;
    TEST_ASSERT_TRUE(net_rtc_valid(&stale));
    net_rtc_on_boot(&stale, false, 5u, false, 600);
    TEST_ASSERT_EQUAL_UINT32(NET_AGE_UNKNOWN, net_rtc_age_now(&stale, 5));
    TEST_ASSERT_EQUAL_UINT32(5u, stale.stamp);          /* restamped for this run */

    /* Accepted, it carries on; a ladder reboot is age 0 whatever the stamp. */
    net_rtc_t ok = r;
    net_rtc_on_boot(&ok, true, 3u, false, 600);
    TEST_ASSERT_EQUAL_UINT32(600 + 5, net_rtc_age_now(&ok, 5));
    net_rtc_on_boot(&stale, false, 5u, true, 600);
    TEST_ASSERT_EQUAL_UINT32(0, net_rtc_age_now(&stale, 0));

    /* The check covers the stamp. */
    ok.stamp ^= 1;
    TEST_ASSERT_FALSE(net_rtc_valid(&ok));
}

TEST_CASE("network AP: a connected client keeps the recovery AP at most 30 min past its lifetime", "[net_core]")
{
    bool expired;
    net_ap_input_t in = ap_in();
    in.recovery_requested = true;
    in.ap_clients = 1;
    in.recovery_age_s = in.recovery_lifetime_s + NET_AP_CLIENT_HOLD_MAX_S - 1;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, net_ap_reasons(&in, &expired));
    TEST_ASSERT_FALSE(expired);
    in.recovery_age_s = in.recovery_lifetime_s + NET_AP_CLIENT_HOLD_MAX_S;
    TEST_ASSERT_EQUAL_UINT32(0, net_ap_reasons(&in, &expired));
    TEST_ASSERT_TRUE(expired);

    /* a lifetime at the top of the range does not wrap the limit */
    in.recovery_lifetime_s = UINT32_MAX - 10u;
    in.recovery_age_s = UINT32_MAX - 5u;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, net_ap_reasons(&in, &expired));
}

TEST_CASE("network credentials: changed ones are tried first and confirmed by an IP within 90 s", "[net_core]")
{
    /* first setup, or the same credentials again: stored as the station's */
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_ACTIVE, net_cred_plan(false, false, true));
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_ACTIVE, net_cred_plan(true, true, true));
    /* changed credentials: pending, on trial */
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_TRIAL, net_cred_plan(true, false, true));
    /* not on Wi-Fi (the QEMU build): stored, never applied or tried, whatever came before */
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_STORE_ONLY, net_cred_plan(false, false, false));
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_STORE_ONLY, net_cred_plan(true, false, false));
    TEST_ASSERT_EQUAL(NET_CRED_PLAN_STORE_ONLY, net_cred_plan(true, true, false));

    net_cred_trial_t t = { 0 };
    uint32_t now = UINT32_MAX - 30u;   /* across the clock wrap */
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, true, now));   /* no trial: an IP changes nothing */
    TEST_ASSERT_EQUAL_UINT32(0, net_cred_trial_left_s(&t, now));
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL_UINT32(NET_CRED_TRIAL_S, net_cred_trial_left_s(&t, now));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S - 1));
    TEST_ASSERT_EQUAL_UINT32(1, net_cred_trial_left_s(&t, now + NET_CRED_TRIAL_S - 1));
    TEST_ASSERT_EQUAL(NET_TRIAL_CONFIRMED, net_cred_trial_tick(&t, true, now + NET_CRED_TRIAL_S - 1));
    TEST_ASSERT_FALSE(t.on);

    /* no IP in time: back to the previous credentials, once */
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S + 1));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, true, now + NET_CRED_TRIAL_S + 2));
}

/* ---- when the AP closes -------------------------------------------------------- */

/* One step of the AP policy, the way network.c runs it: reasons, then the
 * countdown for those reasons, then the reasons become "now". */
static uint32_t ap_step(net_ap_input_t *in, uint32_t *close_in)
{
    uint32_t r = net_ap_reasons(in, NULL);
    *close_in = net_ap_close_in_s(in, r);
    in->reasons_now = r;
    return r;
}

TEST_CASE("network AP: first setup closes 15 s after the IP, counting down", "[net_core]")
{
    net_ap_input_t in = ap_in();
    in.linger_s = 15;
    in.sta_configured = false;
    in.has_ip = false;
    uint32_t close_in = 0;
    /* no credentials: setup, nothing scheduled */
    TEST_ASSERT_EQUAL_UINT32(NET_AP_SETUP, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    /* credentials stored, no IP yet: held as fallback, still nothing scheduled */
    in.sta_configured = true;
    in.offline_s = 3;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    /* the IP at t: linger 15, then down at t + 15 */
    in.has_ip = true;
    in.since_ip_s = 0;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_LINGER, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(15, close_in);
    in.since_ip_s = 14;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_LINGER, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(1, close_in);
    in.since_ip_s = 15;
    TEST_ASSERT_EQUAL_UINT32(0, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
}

TEST_CASE("network AP: the recovery AP's countdown is its lifetime left", "[net_core]")
{
    net_ap_input_t in = ap_in();
    in.linger_s = 15;
    in.recovery_requested = true;
    in.recovery_age_s = 100;
    uint32_t close_in = 0;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(800, close_in);
    /* recovery plus a station that just got its IP: the later of the two */
    in.reasons_now |= NET_AP_FALLBACK;
    in.since_ip_s = 2;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY | NET_AP_LINGER, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(800, close_in);
    in.recovery_age_s = 895;
    in.since_ip_s = 3;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY | NET_AP_LINGER, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(12, close_in);
    /* a client keeps it past its lifetime: nothing scheduled until the 30 min hold ends */
    in = ap_in();
    in.recovery_requested = true;
    in.ap_clients = 1;
    in.recovery_age_s = 900;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    in.recovery_age_s = 900 + NET_AP_CLIENT_HOLD_MAX_S - 1;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_RECOVERY, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    in.recovery_age_s = 900 + NET_AP_CLIENT_HOLD_MAX_S;
    TEST_ASSERT_EQUAL_UINT32(0, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
}

TEST_CASE("network AP: offline 300 s opens the fallback with nothing scheduled", "[net_core]")
{
    net_ap_input_t in = ap_in();
    in.linger_s = 15;
    in.has_ip = false;
    in.offline_s = 299;
    uint32_t close_in = 0;
    TEST_ASSERT_EQUAL_UINT32(0, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    in.offline_s = 300;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_FALLBACK, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
    /* setup together with recovery: setup has no deadline, so neither has the AP */
    in = ap_in();
    in.sta_configured = false;
    in.recovery_requested = true;
    TEST_ASSERT_EQUAL_UINT32(NET_AP_SETUP | NET_AP_RECOVERY, ap_step(&in, &close_in));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, close_in);
}

/* ---- a refused password ends the trial early ------------------------------------------ */

TEST_CASE("network credentials: three refusals before any association revert at once", "[net_core]")
{
    const uint32_t now = 4294967000u;
    net_cred_trial_t t = { 0 };
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));   /* no trial */
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT_AUTH, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_FALSE(t.on);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S));   /* ended */

    /* every refusal reason counts, mixed */
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_4WAY_HANDSHAKE_TIMEOUT));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_HANDSHAKE_TIMEOUT));
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT_AUTH, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY));
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD));
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT_AUTH, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD));
}

TEST_CASE("network credentials: refusals, an association, a refusal: no early revert", "[net_core]")
{
    const uint32_t now = 1000;
    net_cred_trial_t t = { 0 };
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    net_cred_trial_associated(&t);
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    }
    TEST_ASSERT_TRUE(t.on);
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S));   /* the 90 s rule */

    /* another reason in between starts the count again */
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT_AUTH, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
}

TEST_CASE("network credentials: a network out of reach keeps the 90 s rule", "[net_core]")
{
    const uint32_t now = 50;
    net_cred_trial_t t = { 0 };
    net_cred_trial_start(&t, now);
    for (int i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_NO_AP_FOUND));
    }
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S - 1));
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT, net_cred_trial_tick(&t, false, now + NET_CRED_TRIAL_S));

    /* an IP inside the trial confirms as before, refusals or not */
    net_cred_trial_start(&t, now);
    TEST_ASSERT_EQUAL(NET_TRIAL_NONE, net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL));
    net_cred_trial_associated(&t);
    TEST_ASSERT_EQUAL(NET_TRIAL_CONFIRMED, net_cred_trial_tick(&t, true, now + 20));
    TEST_ASSERT_TRUE(net_trial_reason_refused(NET_REASON_AUTH_FAIL));
    TEST_ASSERT_FALSE(net_trial_reason_refused(NET_REASON_NO_AP_FOUND));
    TEST_ASSERT_FALSE(net_trial_reason_refused(8));   /* left the network (our own disconnect) */
}

/* ---- a refused attempt during a trial is retried at once ------------------------------- */

TEST_CASE("network credentials: a refused attempt of an unassociated trial is retried at once", "[net_core]")
{
    net_cred_trial_t t = { 0 };
    /* outside a trial: the backoff, refused or not */
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(0), net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, 0));
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(3), net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, 3));
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(1), net_trial_retry_ms(NULL, NET_REASON_AUTH_FAIL, 1));

    net_cred_trial_start(&t, 100);
    /* every refusal reason: 0 ms */
    TEST_ASSERT_EQUAL_UINT32(0, net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, net_trial_retry_ms(&t, NET_REASON_4WAY_HANDSHAKE_TIMEOUT, 1));
    TEST_ASSERT_EQUAL_UINT32(0, net_trial_retry_ms(&t, NET_REASON_HANDSHAKE_TIMEOUT, 2));
    TEST_ASSERT_EQUAL_UINT32(0, net_trial_retry_ms(&t, NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY, 3));
    TEST_ASSERT_EQUAL_UINT32(0, net_trial_retry_ms(&t, NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD, 4));
    /* out of reach (and any other reason): the backoff */
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(0), net_trial_retry_ms(&t, NET_REASON_NO_AP_FOUND, 0));
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(2), net_trial_retry_ms(&t, 8, 2));
    /* after the trial credentials associated once: the backoff */
    net_cred_trial_associated(&t);
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(1), net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, 1));
    /* a trial that ended: the backoff */
    net_cred_trial_start(&t, 100);
    TEST_ASSERT_EQUAL(NET_TRIAL_REVERT, net_cred_trial_tick(&t, false, 100 + NET_CRED_TRIAL_S));
    TEST_ASSERT_EQUAL_UINT32(net_backoff_ms(0), net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, 0));
}

/* Wrong password, measured on a board: the first attempt 3.6 s after the
 * request (apply delay and the first scan), about 3.1 s of authentication
 * per attempt, about 2.7 s of scan before every further attempt. Returns
 * when the trial gives up, in ms after the request. */
static uint32_t refused_trial_timeline(bool old_backoff)
{
    const uint32_t first_ms = 3600;
    const uint32_t auth_ms = 3100;
    const uint32_t scan_ms = 2700;
    net_cred_trial_t t = { 0 };
    net_cred_trial_start(&t, 0);
    uint32_t at = first_ms;
    for (uint32_t attempt = 0; attempt < 10; attempt++) {
        at += auth_ms;
        uint32_t wait = old_backoff ? net_backoff_ms(attempt) : net_trial_retry_ms(&t, NET_REASON_AUTH_FAIL, attempt);
        if (net_cred_trial_disconnect(&t, NET_REASON_AUTH_FAIL) == NET_TRIAL_REVERT_AUTH) {
            return at;
        }
        at += wait + scan_ms;
    }
    return UINT32_MAX;
}

TEST_CASE("network credentials: a refused password reverts within 19 s", "[net_core]")
{
    uint32_t now_ms = refused_trial_timeline(false);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(19000, now_ms);
    TEST_ASSERT_EQUAL_UINT32(18300, now_ms);
    /* With the backoff between refusals (1 s, then 2 s) it took 3 s longer: the 21 s seen on a board. */
    TEST_ASSERT_EQUAL_UINT32(21300, refused_trial_timeline(true));
}

/* ---- what the status shows about changed credentials -------------------------------------- */

static net_cred_view_t cred_view(const net_cred_in_t *in)
{
    net_cred_view_t v;
    memset(&v, 0xA5, sizeof(v));   /* every field is written */
    net_cred_view(in, &v);
    return v;
}

TEST_CASE("network credentials view: a queued trial counts from 91, then the running one from 90", "[net_core]")
{
    net_cred_in_t in = { .pend_valid = true, .pend_ssid = "Next", .trial_queued = true,
                         .apply_left_ms = NET_CRED_APPLY_DELAY_MS };
    net_cred_view_t v = cred_view(&in);
    TEST_ASSERT_TRUE(v.cred_trial);
    TEST_ASSERT_EQUAL_UINT32(91, v.cred_trial_left_s);
    TEST_ASSERT_TRUE(v.cred_pending);
    TEST_ASSERT_EQUAL_STRING("Next", v.pending_ssid);
    TEST_ASSERT_FALSE(v.cred_reverted);
    TEST_ASSERT_EQUAL_UINT8(0, v.cred_reverted_why);
    in.apply_left_ms = 400;
    TEST_ASSERT_EQUAL_UINT32(91, cred_view(&in).cred_trial_left_s);
    in.apply_left_ms = 0;
    TEST_ASSERT_EQUAL_UINT32(90, cred_view(&in).cred_trial_left_s);

    /* the net task started it */
    net_cred_trial_t t = { 0 };
    net_cred_trial_start(&t, 500);
    in.trial_queued = false;
    in.trial_running = true;
    in.trial_left_s = net_cred_trial_left_s(&t, 500);
    v = cred_view(&in);
    TEST_ASSERT_TRUE(v.cred_trial);
    TEST_ASSERT_EQUAL_UINT32(90, v.cred_trial_left_s);
    in.trial_left_s = net_cred_trial_left_s(&t, 530);
    TEST_ASSERT_EQUAL_UINT32(60, cred_view(&in).cred_trial_left_s);
    in.trial_left_s = net_cred_trial_left_s(&t, 589);
    TEST_ASSERT_EQUAL_UINT32(1, cred_view(&in).cred_trial_left_s);

    /* a newer change queued while one runs: the new trial's count */
    in.trial_queued = true;
    in.apply_left_ms = 1000;
    TEST_ASSERT_EQUAL_UINT32(91, cred_view(&in).cred_trial_left_s);
}

TEST_CASE("network credentials view: reverted and pending; a discard clears every field", "[net_core]")
{
    net_cred_in_t in = { .pend_valid = true, .pend_ssid = "Typo", .reverted = true, .reverted_why = 2 };
    net_cred_view_t v = cred_view(&in);
    TEST_ASSERT_FALSE(v.cred_trial);
    TEST_ASSERT_EQUAL_UINT32(0, v.cred_trial_left_s);
    TEST_ASSERT_TRUE(v.cred_pending);
    TEST_ASSERT_EQUAL_STRING("Typo", v.pending_ssid);
    TEST_ASSERT_TRUE(v.cred_reverted);
    TEST_ASSERT_EQUAL_UINT8(2, v.cred_reverted_why);

    /* discard: nothing pending, and the revert of the dropped pair is gone too */
    in.pend_valid = false;
    v = cred_view(&in);
    TEST_ASSERT_FALSE(v.cred_trial);
    TEST_ASSERT_EQUAL_UINT32(0, v.cred_trial_left_s);
    TEST_ASSERT_FALSE(v.cred_pending);
    TEST_ASSERT_EQUAL_STRING("", v.pending_ssid);
    TEST_ASSERT_FALSE(v.cred_reverted);
    TEST_ASSERT_EQUAL_UINT8(0, v.cred_reverted_why);

    /* discard during a trial: the station's own credentials are queued, the trial is over */
    in = (net_cred_in_t){ .trial_running = true, .trial_left_s = 70, .other_queued = true, .apply_left_ms = 1000 };
    v = cred_view(&in);
    TEST_ASSERT_FALSE(v.cred_trial);
    TEST_ASSERT_EQUAL_UINT32(0, v.cred_trial_left_s);
    TEST_ASSERT_FALSE(v.cred_pending);
    TEST_ASSERT_EQUAL_STRING("", v.pending_ssid);
}

TEST_CASE("network credentials view: a retry after a revert is a trial, not a revert", "[net_core]")
{
    /* the retry clears the revert under the lock and queues the trial */
    net_cred_in_t in = { .pend_valid = true, .pend_ssid = "Next", .reverted = false, .trial_queued = true,
                         .apply_left_ms = 1000 };
    net_cred_view_t v = cred_view(&in);
    TEST_ASSERT_TRUE(v.cred_trial);
    TEST_ASSERT_FALSE(v.cred_reverted);
    TEST_ASSERT_EQUAL_UINT32(91, v.cred_trial_left_s);
    /* even with a revert flag still set, a trial of the pending pair is never shown as reverted */
    in.reverted = true;
    in.reverted_why = 1;
    v = cred_view(&in);
    TEST_ASSERT_TRUE(v.cred_trial);
    TEST_ASSERT_FALSE(v.cred_reverted);
    TEST_ASSERT_EQUAL_UINT8(0, v.cred_reverted_why);
    /* a 32-byte SSID fits */
    in.pend_ssid = "12345678901234567890123456789012";
    TEST_ASSERT_EQUAL_STRING("12345678901234567890123456789012", cred_view(&in).pending_ssid);
}
