/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_core.h"

/* ---- errors ------------------------------------------------------------------------------ */

TEST_CASE("every error code has a name, a status and a message", "[web_core]")
{
    for (int e = 0; e < WEB_ERR_COUNT; e++) {
        TEST_ASSERT_NOT_NULL(web_err_code((web_err_t)e));
        TEST_ASSERT_NOT_NULL(web_err_message((web_err_t)e));
        int st = web_err_status((web_err_t)e);
        TEST_ASSERT_TRUE(st >= 400 && st <= 503);
        TEST_ASSERT_EQUAL(st, atoi(web_status_line(st)));
    }
    TEST_ASSERT_EQUAL_STRING("journal_open", web_err_code(WEB_ERR_JOURNAL_OPEN));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_JOURNAL_OPEN));
    TEST_ASSERT_EQUAL(421, web_err_status(WEB_ERR_BAD_HOST));
    TEST_ASSERT_EQUAL(422, web_err_status(WEB_ERR_NOT_THIS_PROJECT));
    TEST_ASSERT_EQUAL(501, web_err_status(WEB_ERR_NOT_AVAILABLE_YET));
    TEST_ASSERT_EQUAL(503, web_err_status(WEB_ERR_TOO_MANY_CLIENTS));
    TEST_ASSERT_EQUAL(411, web_err_status(WEB_ERR_LENGTH_REQUIRED));
    TEST_ASSERT_EQUAL_STRING("202 Accepted", web_status_line(202));
}

/* ---- request checks -------------------------------------------------------------------------- */

static const web_host_names_t s_names = { .sta_ip = "192.168.1.50", .ap_ip = "192.168.4.1", .hostname = "onocoydiy" };

TEST_CASE("Host check accepts only the device's names", "[web_core]")
{
    static const char *const good[] = {
        "192.168.1.50", "192.168.1.50:80", "192.168.4.1", "onocoydiy", "ONOCOYDIY", "onocoydiy.local",
        "OnocoyDIY.Local:8080", "onocoydiy.lan", "onocoydiy.home", "onocoydiy.home.arpa", "onocoydiy.localdomain",
        "onocoydiy.internal", " onocoydiy ",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(web_host_allowed(good[i], &s_names), good[i]);
    }
    static const char *const bad[] = {
        "", "evil.com", "onocoydiy.evil.com", "onocoydiy.local.evil.com", "xonocoydiy", "onocoydiyx.local",
        "192.168.1.5", "192.168.1.500", "[::1]", "[::1]:80", "onocoydiy:", "onocoydiy:80:80", "onocoydiy:abc",
        "onocoydiy:123456", "onocoydiy.", "connectivitycheck.gstatic.com", "onocoydiy/x",
        /* a publicly delegated zone: whoever controls it could rebind the name */
        "onocoydiy.fritz.box",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(web_host_allowed(bad[i], &s_names), bad[i]);
    }
    TEST_ASSERT_FALSE(web_host_allowed(NULL, &s_names));
    /* Offline: no station address, the AP address still counts. */
    web_host_names_t off = { .sta_ip = "", .ap_ip = "192.168.4.1", .hostname = "roof" };
    TEST_ASSERT_TRUE(web_host_allowed("192.168.4.1", &off));
    TEST_ASSERT_FALSE(web_host_allowed("192.168.1.50", &off));
    TEST_ASSERT_TRUE(web_host_allowed("roof.local", &off));
}

TEST_CASE("Origin must be http and equal the Host", "[web_core]")
{
    TEST_ASSERT_TRUE(web_origin_ok("http://192.168.1.50", "192.168.1.50"));
    TEST_ASSERT_TRUE(web_origin_ok("HTTP://Onocoydiy.local", "onocoydiy.local"));
    TEST_ASSERT_TRUE(web_origin_ok("http://onocoydiy:8080/", "onocoydiy:8080"));
    TEST_ASSERT_FALSE(web_origin_ok("https://192.168.1.50", "192.168.1.50"));
    TEST_ASSERT_FALSE(web_origin_ok("http://evil.com", "192.168.1.50"));
    TEST_ASSERT_FALSE(web_origin_ok("http://192.168.1.50:81", "192.168.1.50"));
    TEST_ASSERT_FALSE(web_origin_ok("null", "192.168.1.50"));
    TEST_ASSERT_FALSE(web_origin_ok("http://", ""));
}

TEST_CASE("content types compare without case and allow parameters", "[web_core]")
{
    TEST_ASSERT_TRUE(web_content_type_is("application/json", "application/json"));
    TEST_ASSERT_TRUE(web_content_type_is("Application/JSON; charset=utf-8", "application/json"));
    TEST_ASSERT_TRUE(web_content_type_is(" application/json ;charset=UTF-8", "application/json"));
    TEST_ASSERT_FALSE(web_content_type_is("application/jsonx", "application/json"));
    TEST_ASSERT_FALSE(web_content_type_is("text/plain", "application/json"));
    TEST_ASSERT_FALSE(web_content_type_is("application/x-www-form-urlencoded", "application/json"));
    TEST_ASSERT_TRUE(web_content_type_is("application/octet-stream", "application/octet-stream"));
}

TEST_CASE("session cookie is found among others", "[web_core]")
{
    char sid[33];
    TEST_ASSERT_TRUE(web_cookie_sid("sid=0123456789abcdef0123456789ABCDEF", sid));
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", sid);
    TEST_ASSERT_TRUE(web_cookie_sid("theme=dark; sid=00112233445566778899aabbccddeeff; x=1", sid));
    TEST_ASSERT_EQUAL_STRING("00112233445566778899aabbccddeeff", sid);
    TEST_ASSERT_FALSE(web_cookie_sid("sid=0011", sid));
    TEST_ASSERT_FALSE(web_cookie_sid("sid=zz112233445566778899aabbccddeeff", sid));
    TEST_ASSERT_FALSE(web_cookie_sid("xsid=00112233445566778899aabbccddeeff", sid));
    TEST_ASSERT_FALSE(web_cookie_sid("", sid));
    TEST_ASSERT_FALSE(web_cookie_sid(NULL, sid));
}

TEST_CASE("hex helpers and query values", "[web_core]")
{
    uint8_t in[16];
    for (int i = 0; i < 16; i++) {
        in[i] = (uint8_t)(i * 17);
    }
    char hex[33];
    web_hex16(in, hex);
    TEST_ASSERT_EQUAL_STRING("00112233445566778899aabbccddeeff", hex);

    uint8_t sha[32];
    TEST_ASSERT_TRUE(web_parse_sha256_hex("9AC25D1D359E5815B13291F61405FAEF53D71E306A602184546B32584B35EC71", sha));
    TEST_ASSERT_EQUAL_HEX8(0x9a, sha[0]);
    TEST_ASSERT_EQUAL_HEX8(0x71, sha[31]);
    TEST_ASSERT_FALSE(web_parse_sha256_hex("9ac2", sha));
    TEST_ASSERT_FALSE(web_parse_sha256_hex("zz25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71x", sha));

    char v[16];
    TEST_ASSERT_TRUE(web_query_value("cursor=51234&boot=prev", "boot", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("prev", v);
    TEST_ASSERT_TRUE(web_query_value("cursor=51234&boot=prev", "cursor", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("51234", v);
    TEST_ASSERT_TRUE(web_query_value("a=x%20y+z", "a", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("x y z", v);
    TEST_ASSERT_FALSE(web_query_value("boots=1", "boot", v, sizeof(v)));
    TEST_ASSERT_FALSE(web_query_value("a=%2", "a", v, sizeof(v)));
    TEST_ASSERT_FALSE(web_query_value("a=0123456789abcdefghij", "a", v, sizeof(v)));
    TEST_ASSERT_TRUE(web_query_value("flag", "flag", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("", v);
}

/* ---- sessions ----------------------------------------------------------------------------------- */

static void token(uint8_t t[16], uint8_t seed)
{
    for (int i = 0; i < 16; i++) {
        t[i] = (uint8_t)(seed + i);
    }
}

TEST_CASE("sessions expire after 30 min idle and 24 h in all", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[16];
    token(a, 1);
    uint32_t now = UINT32_MAX - 100;   /* across the clock wrap */
    int idx = web_sess_create(&t, a, now);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + 1799));
    TEST_ASSERT_EQUAL_UINT32(1, web_sess_left_s(&t, idx, now + 1799));
    web_sess_touch(&t, idx, now + 1799);
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + 3500));
    web_sess_touch(&t, idx, now + 3500);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + 3500 + WEB_SESSION_IDLE_S));

    /* Absolute lifetime, however busy. */
    idx = web_sess_create(&t, a, now);
    uint32_t gen = t.s[idx].gen;
    for (uint32_t s = 600; s < WEB_SESSION_LIFE_S; s += 600) {
        TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + s));
        web_sess_touch(&t, idx, now + s);
    }
    TEST_ASSERT_TRUE(web_sess_alive(&t, idx, gen));
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_expire(&t, now + WEB_SESSION_LIFE_S));
    TEST_ASSERT_FALSE(web_sess_alive(&t, idx, gen));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + WEB_SESSION_LIFE_S));
}

TEST_CASE("a fifth login ends the least recently used session", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t tok[5][16];
    int idx[5];
    uint32_t gen[5];
    for (int i = 0; i < 4; i++) {
        token(tok[i], (uint8_t)(10 * i));
        idx[i] = web_sess_create(&t, tok[i], 100 + (uint32_t)i);
        gen[i] = t.s[idx[i]].gen;
    }
    web_sess_touch(&t, idx[0], 200);   /* 1 is now the least recently used */
    token(tok[4], 200);
    idx[4] = web_sess_create(&t, tok[4], 201);
    TEST_ASSERT_EQUAL(idx[1], idx[4]);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, tok[1], 202));
    TEST_ASSERT_FALSE(web_sess_alive(&t, idx[1], gen[1]));
    TEST_ASSERT_EQUAL(idx[0], web_sess_find(&t, tok[0], 202));
    TEST_ASSERT_EQUAL(idx[4], web_sess_find(&t, tok[4], 202));

    /* Password change: every other session ends. */
    uint32_t mask = web_sess_end_all_except(&t, idx[0]);
    TEST_ASSERT_EQUAL_UINT32(0x0F & ~(1u << idx[0]), mask);
    TEST_ASSERT_EQUAL(idx[0], web_sess_find(&t, tok[0], 203));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, tok[2], 203));
    TEST_ASSERT_TRUE(web_sess_alive(&t, idx[0], gen[0]));
    web_sess_end(&t, idx[0]);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, tok[0], 203));
}

/* ---- throttling ------------------------------------------------------------------------------------ */

TEST_CASE("login throttle: 5 free failures, then doubling up to 300 s", "[web_core]")
{
    TEST_ASSERT_EQUAL_UINT32(0, web_throttle_delay_s(4));
    TEST_ASSERT_EQUAL_UINT32(2, web_throttle_delay_s(5));
    TEST_ASSERT_EQUAL_UINT32(4, web_throttle_delay_s(6));
    TEST_ASSERT_EQUAL_UINT32(256, web_throttle_delay_s(12));
    TEST_ASSERT_EQUAL_UINT32(300, web_throttle_delay_s(13));
    TEST_ASSERT_EQUAL_UINT32(300, web_throttle_delay_s(1000));

    web_throttle_t t = { 0 };
    uint32_t now = UINT32_MAX - 5;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, web_throttle_wait_s(&t, now));
        web_throttle_fail(&t, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0, web_throttle_wait_s(&t, now));
    web_throttle_fail(&t, now);   /* 5th */
    TEST_ASSERT_EQUAL_UINT32(2, web_throttle_wait_s(&t, now));
    TEST_ASSERT_EQUAL_UINT32(1, web_throttle_wait_s(&t, now + 1));
    TEST_ASSERT_EQUAL_UINT32(0, web_throttle_wait_s(&t, now + 2));
    web_throttle_fail(&t, now + 2);   /* 6th */
    TEST_ASSERT_EQUAL_UINT32(4, web_throttle_wait_s(&t, now + 2));
    web_throttle_ok(&t);
    TEST_ASSERT_EQUAL_UINT32(0, web_throttle_wait_s(&t, now + 3));
}

TEST_CASE("login throttle: one client's failures never delay another client", "[web_core]")
{
    web_throttles_t t;
    uint32_t now = UINT32_MAX - 100;
    web_throttles_init(&t, now);
    const uint64_t attacker = 0x0A00000Au;   /* 10.0.0.10 */
    const uint64_t owner = 0x0B00000Au;
    /* The attacker keeps the delay at its cap: one failure as each window opens. */
    for (int i = 0; i < 40; i++) {
        uint32_t w = web_throttles_wait_s(&t, attacker, false, now);
        now += w;
        TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, attacker, false, now));
        web_throttles_fail(&t, attacker, false, now);
        TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, owner, false, now));
        TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, owner, true, now));
    }
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_MAX_S, web_throttles_wait_s(&t, attacker, false, now));
    /* The owner's own mistakes count for the owner only, and a success resets them. */
    for (int i = 0; i < 5; i++) {
        web_throttles_fail(&t, owner, false, now);
    }
    TEST_ASSERT_EQUAL_UINT32(2, web_throttles_wait_s(&t, owner, false, now));
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, owner, true, now));   /* over the AP: another client */
    web_throttles_ok(&t, owner, false);
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, owner, false, now));
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_MAX_S, web_throttles_wait_s(&t, attacker, false, now));
    /* A new password clears everything. */
    web_throttles_init(&t, now);
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, attacker, false, now));
}

TEST_CASE("login throttle: many addresses hit the interface ceiling, the AP keeps its own", "[web_core]")
{
    web_throttles_t t;
    uint32_t now = 1000;
    web_throttles_init(&t, now);
    /* 40 addresses, each with its free failures: the station interface's
     * burst runs out, replaced table entries do not reset the ceiling. */
    unsigned counted = 0;
    for (uint64_t a = 1; a <= 40; a++) {
        for (int i = 0; i < 4; i++) {
            if (web_throttles_wait_s(&t, a, false, now) == 0) {
                web_throttles_fail(&t, a, false, now);
                counted++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_IFACE_BURST, counted);
    uint32_t w = web_throttles_wait_s(&t, 999, false, now);
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_IFACE_REFILL_S, w);
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, 999, true, now));   /* the AP is not affected */
    /* One more failure per refill period after that. */
    now += WEB_THROTTLE_IFACE_REFILL_S;
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, 999, false, now));
    web_throttles_fail(&t, 999, false, now);
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_IFACE_REFILL_S, web_throttles_wait_s(&t, 998, false, now));
    /* A quiet spell refills the whole burst. */
    now += WEB_THROTTLE_IFACE_BURST * WEB_THROTTLE_IFACE_REFILL_S;
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, 998, false, now));
    TEST_ASSERT_EQUAL_UINT32(WEB_THROTTLE_IFACE_BURST, t.iface[0].left);

    /* A single client throttled at the cap never drains the ceiling. */
    web_throttles_init(&t, now);
    for (int i = 0; i < 2000; i++) {
        now += web_throttles_wait_s(&t, 7, false, now);
        web_throttles_fail(&t, 7, false, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, 8, false, now));
}

/* ---- field rules ----------------------------------------------------------------------------------- */

TEST_CASE("admin password rules", "[web_core]")
{
    TEST_ASSERT_TRUE(web_password_valid("12345678"));
    TEST_ASSERT_TRUE(web_password_valid("correct horse battery staple"));
    TEST_ASSERT_TRUE(web_password_valid("p\xc3\xa4sswort!"));
    TEST_ASSERT_TRUE(web_password_valid("1234567890123456789012345678901234567890123456789012345678901234"));
    TEST_ASSERT_FALSE(web_password_valid("1234567"));
    TEST_ASSERT_FALSE(web_password_valid("12345678901234567890123456789012345678901234567890123456789012345"));
    TEST_ASSERT_FALSE(web_password_valid("tab\there!"));
    TEST_ASSERT_FALSE(web_password_valid("bad\xffutf8"));
    TEST_ASSERT_FALSE(web_password_valid("c1 ctl \xc2\x85 x"));
    TEST_ASSERT_FALSE(web_password_valid(NULL));
}

TEST_CASE("hostname, SSID and Wi-Fi password rules", "[web_core]")
{
    TEST_ASSERT_TRUE(web_hostname_valid("roof-station"));
    TEST_ASSERT_TRUE(web_hostname_valid("a"));
    TEST_ASSERT_FALSE(web_hostname_valid("-roof"));
    TEST_ASSERT_FALSE(web_hostname_valid("roof-"));
    TEST_ASSERT_FALSE(web_hostname_valid("roof.station"));
    TEST_ASSERT_FALSE(web_hostname_valid(""));
    TEST_ASSERT_FALSE(web_hostname_valid("123456789012345678901234567890123"));

    TEST_ASSERT_TRUE(web_ssid_valid("Home"));
    TEST_ASSERT_TRUE(web_ssid_valid("12345678901234567890123456789012"));
    TEST_ASSERT_FALSE(web_ssid_valid(""));
    TEST_ASSERT_FALSE(web_ssid_valid("123456789012345678901234567890123"));

    TEST_ASSERT_TRUE(web_wifi_password_valid(""));
    TEST_ASSERT_TRUE(web_wifi_password_valid("12345678"));
    TEST_ASSERT_TRUE(web_wifi_password_valid("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_FALSE(web_wifi_password_valid("1234567"));
    TEST_ASSERT_FALSE(web_wifi_password_valid("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdeg"));
    TEST_ASSERT_FALSE(web_wifi_password_valid("pass\x01word"));
    TEST_ASSERT_FALSE(web_ap_password_valid(""));
    TEST_ASSERT_TRUE(web_ap_password_valid("apsecret"));
}

TEST_CASE("caster host names and domains", "[web_core]")
{
    TEST_ASSERT_TRUE(web_host_name_ok("servers.onocoy.com"));
    TEST_ASSERT_TRUE(web_host_name_ok("35.207.172.57"));
    TEST_ASSERT_TRUE(web_host_name_ok("rtk2go.com"));
    TEST_ASSERT_TRUE(web_host_name_ok("caster-1.example"));
    TEST_ASSERT_FALSE(web_host_name_ok(""));
    TEST_ASSERT_FALSE(web_host_name_ok("-bad.example"));
    TEST_ASSERT_FALSE(web_host_name_ok("bad-.example"));
    TEST_ASSERT_FALSE(web_host_name_ok("a..b"));
    TEST_ASSERT_FALSE(web_host_name_ok("host name"));
    TEST_ASSERT_FALSE(web_host_name_ok("1.2.3.256"));
    TEST_ASSERT_FALSE(web_host_name_ok("1.2.3"));
    TEST_ASSERT_FALSE(web_host_name_ok("host:2101"));

    TEST_ASSERT_TRUE(web_host_in_domain("servers.onocoy.com", "onocoy.com"));
    TEST_ASSERT_TRUE(web_host_in_domain("ONOCOY.COM", "onocoy.com"));
    TEST_ASSERT_FALSE(web_host_in_domain("notonocoy.com", "onocoy.com"));
    TEST_ASSERT_FALSE(web_host_in_domain("onocoy.com.evil", "onocoy.com"));
}

TEST_CASE("expected firmware string needs a Build number", "[web_core]")
{
    TEST_ASSERT_EQUAL_UINT32(19152, web_expected_build("R4.10Build19152"));
    TEST_ASSERT_EQUAL_UINT32(7, web_expected_build("Build7"));
    TEST_ASSERT_EQUAL_UINT32(0, web_expected_build("R4.10"));
    TEST_ASSERT_EQUAL_UINT32(0, web_expected_build("R4.10Build"));
    TEST_ASSERT_EQUAL_UINT32(0, web_expected_build("Build0"));
    TEST_ASSERT_EQUAL_UINT32(0, web_expected_build("R4.10Build19152\n"));
    TEST_ASSERT_EQUAL_UINT32(0, web_expected_build("123456789012345678901234567890123456789012Build1"));
}

TEST_CASE("caster policy: the rule ntrip_server reports keeps its API name", "[web_core]")
{
    /* The rules live in ntrip_server; the web only names what it reports. */
    TEST_ASSERT_EQUAL_STRING("second_onocoy", web_policy_reason(NTRIP_POLICY_SECOND_ONOCOY));
    TEST_ASSERT_EQUAL_STRING("second_rtk2go", web_policy_reason(NTRIP_POLICY_SECOND_RTK2GO));
    TEST_ASSERT_EQUAL_STRING("dup_stream_ack_required", web_policy_reason(NTRIP_POLICY_NEED_DUP_ACK));
    TEST_ASSERT_NULL(web_policy_reason(NTRIP_POLICY_OK));
    TEST_ASSERT_NULL(web_policy_reason(NTRIP_POLICY_COUNT));
    for (int p = NTRIP_POLICY_OK + 1; p < NTRIP_POLICY_COUNT; p++) {
        TEST_ASSERT_NOT_NULL(web_policy_reason((ntrip_policy_t)p));   /* every refusal has a name */
    }
    TEST_ASSERT_EQUAL_STRING("caster_policy", web_err_code(WEB_ERR_CASTER_POLICY));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_CASTER_POLICY));
}

/* ---- app image check ----------------------------------------------------------------------------------- */

static void fake_image(uint8_t *img, size_t len, uint16_t chip, const char *project, const char *version)
{
    memset(img, 0, len);
    img[0] = WEB_IMAGE_MAGIC;
    img[WEB_IMAGE_CHIP_ID_OFS] = (uint8_t)(chip & 0xFF);
    img[WEB_IMAGE_CHIP_ID_OFS + 1] = (uint8_t)(chip >> 8);
    uint8_t *d = img + WEB_IMAGE_DESC_OFS;
    d[0] = 0x32;
    d[1] = 0x54;
    d[2] = 0xCD;
    d[3] = 0xAB;
    strncpy((char *)d + WEB_DESC_VERSION_OFS, version, WEB_DESC_FIELD_LEN);
    strncpy((char *)d + WEB_DESC_PROJECT_OFS, project, WEB_DESC_FIELD_LEN);
}

TEST_CASE("app upload check reads the image header and app description", "[web_core]")
{
    uint8_t img[256];
    web_image_info_t info;
    fake_image(img, sizeof(img), 9, "onocoydiy-esp-um982", "v0.4.0+dev");
    TEST_ASSERT_EQUAL(WEB_IMAGE_OK, web_image_check(img, sizeof(img), 9, "onocoydiy-esp-um982", &info));
    TEST_ASSERT_EQUAL_STRING("v0.4.0+dev", info.version);
    TEST_ASSERT_EQUAL_STRING("onocoydiy-esp-um982", info.project);
    TEST_ASSERT_EQUAL(WEB_IMAGE_NOT_APP, web_image_check(img, sizeof(img), 5, "onocoydiy-esp-um982", &info));
    TEST_ASSERT_EQUAL(WEB_IMAGE_NOT_APP, web_image_check(img, 40, 9, "onocoydiy-esp-um982", &info));

    fake_image(img, sizeof(img), 9, "some-other-app", "1.0");
    TEST_ASSERT_EQUAL(WEB_IMAGE_OTHER_PROJECT, web_image_check(img, sizeof(img), 9, "onocoydiy-esp-um982", &info));
    TEST_ASSERT_EQUAL(WEB_IMAGE_OK, web_image_check(img, sizeof(img), 9, NULL, &info));   /* ?any_project=1 */

    /* A 32-character project name has no terminator in the description. */
    fake_image(img, sizeof(img), 9, "abcdefghijklmnopqrstuvwxyz012345", "1.0");
    TEST_ASSERT_EQUAL(WEB_IMAGE_OK, web_image_check(img, sizeof(img), 9, NULL, &info));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz012345", info.project);

    img[0] = 0;
    TEST_ASSERT_EQUAL(WEB_IMAGE_NOT_APP, web_image_check(img, sizeof(img), 9, NULL, &info));
    fake_image(img, sizeof(img), 9, "x", "1");
    img[WEB_IMAGE_DESC_OFS] = 0;
    TEST_ASSERT_EQUAL(WEB_IMAGE_NOT_APP, web_image_check(img, sizeof(img), 9, NULL, &info));
}

TEST_CASE("app upload: a version rolled back here needs the acknowledgement, like the banner's", "[web_core]")
{
    TEST_ASSERT_EQUAL(WEB_ERR_COUNT, web_app_upload_verdict(WEB_IMAGE_OK, false, false));
    TEST_ASSERT_EQUAL(WEB_ERR_ROLLED_BACK_ACK_REQUIRED, web_app_upload_verdict(WEB_IMAGE_OK, true, false));
    TEST_ASSERT_EQUAL(WEB_ERR_COUNT, web_app_upload_verdict(WEB_IMAGE_OK, true, true));
    TEST_ASSERT_EQUAL(WEB_ERR_NOT_THIS_PROJECT, web_app_upload_verdict(WEB_IMAGE_OTHER_PROJECT, false, true));
    TEST_ASSERT_EQUAL(WEB_ERR_INVALID_IMAGE, web_app_upload_verdict(WEB_IMAGE_NOT_APP, true, true));
    TEST_ASSERT_EQUAL_STRING("rolled_back_ack_required", web_err_code(WEB_ERR_ROLLED_BACK_ACK_REQUIRED));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_ROLLED_BACK_ACK_REQUIRED));
}

/* ---- misc --------------------------------------------------------------------------------------------- */

TEST_CASE("CRC-32, UTC dates and file names", "[web_core]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, web_crc32(0, "123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(web_crc32(0, "123456789", 9), web_crc32(web_crc32(0, "1234", 4), "56789", 5));

    char d[11];
    web_utc_date(0, d);
    TEST_ASSERT_EQUAL_STRING("1970-01-01", d);
    web_utc_date(-1, d);
    TEST_ASSERT_EQUAL_STRING("1969-12-31", d);
    web_utc_date(11016, d);
    TEST_ASSERT_EQUAL_STRING("2000-02-29", d);
    web_utc_date(20736, d);
    TEST_ASSERT_EQUAL_STRING("2026-10-10", d);

    char f[32];
    web_safe_filename_part("v0.3.0-4-g47fc47b-dirty+dev", f, sizeof(f));
    TEST_ASSERT_EQUAL_STRING("v0.3.0-4-g47fc47b-dirty+dev", f);
    web_safe_filename_part("a\"b/c d", f, sizeof(f));
    TEST_ASSERT_EQUAL_STRING("a_b_c_d", f);
    web_safe_filename_part("abcdef", f, 4);
    TEST_ASSERT_EQUAL_STRING("abc", f);
}

TEST_CASE("WebSocket queue keeps the newest progress and drops by kind", "[web_core]")
{
    web_msg_kind_t k[WEB_WSQ_MAX_MSGS];
    size_t s[WEB_WSQ_MAX_MSGS];
    for (size_t i = 0; i < WEB_WSQ_MAX_MSGS; i++) {
        k[i] = WEB_MSG_STATUS;
        s[i] = 100;
    }
    /* Room: anything goes. */
    web_wsq_decision_t d = web_wsq_admit(k, s, 3, WEB_MSG_LOG, 100);
    TEST_ASSERT_TRUE(d.accept);
    TEST_ASSERT_EQUAL(-1, d.evict);

    /* Full by count: status and log refused, an event evicts the oldest event. */
    k[2] = WEB_MSG_EVENT;
    k[5] = WEB_MSG_EVENT;
    TEST_ASSERT_FALSE(web_wsq_admit(k, s, WEB_WSQ_MAX_MSGS, WEB_MSG_STATUS, 100).accept);
    TEST_ASSERT_FALSE(web_wsq_admit(k, s, WEB_WSQ_MAX_MSGS, WEB_MSG_LOG, 100).accept);
    TEST_ASSERT_FALSE(web_wsq_admit(k, s, WEB_WSQ_MAX_MSGS, WEB_MSG_OTHER, 10).accept);
    d = web_wsq_admit(k, s, WEB_WSQ_MAX_MSGS, WEB_MSG_EVENT, 100);
    TEST_ASSERT_TRUE(d.accept);
    TEST_ASSERT_EQUAL(2, d.evict);

    /* Progress replaces the queued progress message, full or not. */
    k[4] = WEB_MSG_PROGRESS;
    d = web_wsq_admit(k, s, WEB_WSQ_MAX_MSGS, WEB_MSG_PROGRESS, 120);
    TEST_ASSERT_TRUE(d.accept);
    TEST_ASSERT_EQUAL(4, d.evict);
    d = web_wsq_admit(k, s, 6, WEB_MSG_PROGRESS, 120);
    TEST_ASSERT_TRUE(d.accept);
    TEST_ASSERT_EQUAL(4, d.evict);

    /* Full by bytes. */
    s[0] = WEB_WSQ_MAX_BYTES - 50;
    TEST_ASSERT_FALSE(web_wsq_admit(k, s, 1, WEB_MSG_STATUS, 100).accept);
    TEST_ASSERT_TRUE(web_wsq_admit(k, s, 1, WEB_MSG_STATUS, 50).accept);
    TEST_ASSERT_FALSE(web_wsq_admit(k, s, 0, WEB_MSG_STATUS, WEB_WSQ_MAX_BYTES + 1).accept);
}
