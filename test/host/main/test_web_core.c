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
    /* The comparison's refusal without a receiver; not the detection's not_no_receiver. */
    TEST_ASSERT_EQUAL_STRING("no_receiver", web_err_code(WEB_ERR_NO_RECEIVER));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_NO_RECEIVER));
    TEST_ASSERT_EQUAL_STRING("No receiver is connected", web_err_message(WEB_ERR_NO_RECEIVER));
    TEST_ASSERT_EQUAL_STRING("not_no_receiver", web_err_code(WEB_ERR_NOT_NO_RECEIVER));
    for (int e = 0; e < WEB_ERR_COUNT; e++) {   /* every code once */
        for (int f = e + 1; f < WEB_ERR_COUNT; f++) {
            TEST_ASSERT_TRUE_MESSAGE(strcmp(web_err_code((web_err_t)e), web_err_code((web_err_t)f)) != 0,
                                     web_err_code((web_err_t)e));
        }
    }
}

/* ---- request checks -------------------------------------------------------------------------- */

static const web_host_names_t s_names = {
    .sta_ip = "192.168.1.50", .ap_ip = "192.168.4.1", .hostname = "onocoydiy", .mdns_host = "onocoydiy",
};

TEST_CASE("Host check accepts only the device's names", "[web_core]")
{
    static const char *const good[] = {
        "192.168.1.50", "192.168.1.50:80", "192.168.4.1", "onocoydiy", "ONOCOYDIY", "onocoydiy.local",
        "OnocoyDIY.Local:8080", "onocoydiy.lan", "onocoydiy.home", "onocoydiy.home.arpa", "onocoydiy.localdomain",
        "onocoydiy.internal", " onocoydiy ",
        /* the fully qualified forms (a trailing dot), as a DNS-SD target or a typed FQDN gives them */
        "onocoydiy.local.", "onocoydiy.local.:80", "onocoydiy.", "onocoydiy.lan.",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(web_host_allowed(good[i], &s_names), good[i]);
    }
    static const char *const bad[] = {
        "", "evil.com", "onocoydiy.evil.com", "onocoydiy.local.evil.com", "xonocoydiy", "onocoydiyx.local",
        "192.168.1.5", "192.168.1.500", "[::1]", "[::1]:80", "onocoydiy:", "onocoydiy:80:80", "onocoydiy:abc",
        "onocoydiy:123456", "onocoydiy..", ".", ".:80", "onocoydiy.local..", "192.168.1.50.",
        "connectivitycheck.gstatic.com", "onocoydiy/x",
        /* a publicly delegated zone: whoever controls it could rebind the name */
        "onocoydiy.fritz.box",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(web_host_allowed(bad[i], &s_names), bad[i]);
    }
    TEST_ASSERT_FALSE(web_host_allowed(NULL, &s_names));
    /* Offline: no station address, the AP address still counts. */
    web_host_names_t off = { .sta_ip = "", .ap_ip = "192.168.4.1", .hostname = "roof", .mdns_host = "roof" };
    TEST_ASSERT_TRUE(web_host_allowed("192.168.4.1", &off));
    TEST_ASSERT_FALSE(web_host_allowed("192.168.1.50", &off));
    TEST_ASSERT_TRUE(web_host_allowed("roof.local", &off));
}

TEST_CASE("Host check: <mdns_host>.local follows the name the responder answers", "[web_core]")
{
    web_host_names_t n = { .sta_ip = "192.168.1.50", .ap_ip = "192.168.4.1", .hostname = "onocoydiy",
                           .mdns_host = "onocoydiy" };
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy.local", &n));
    TEST_ASSERT_TRUE(web_host_allowed("ONOCOYDIY.LOCAL:80", &n));
    TEST_ASSERT_FALSE(web_host_allowed("onocoydiy-2.local", &n));
    TEST_ASSERT_FALSE(web_host_allowed("evil.local", &n));
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy.local.", &n));    /* the _http._tcp service's SRV target */
    TEST_ASSERT_FALSE(web_host_allowed("x.onocoydiy.local", &n));
    TEST_ASSERT_FALSE(web_host_allowed("evil.local.", &n));

    /* After a name conflict the responder answers onocoydiy-2.local: that
     * name passes; the DHCP name and its search domains stay as they were. */
    n.mdns_host = "onocoydiy-2";
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy-2.local", &n));
    TEST_ASSERT_TRUE(web_host_allowed("Onocoydiy-2.Local:8080", &n));
    TEST_ASSERT_FALSE(web_host_allowed("onocoydiy-2", &n));
    TEST_ASSERT_FALSE(web_host_allowed("onocoydiy-2.lan", &n));
    TEST_ASSERT_FALSE(web_host_allowed("onocoydiy.local", &n));   /* another device answers that name */
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy", &n));
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy.lan", &n));
    TEST_ASSERT_FALSE(web_host_allowed("evil.local", &n));

    /* mDNS not running: only the IPs and the DHCP name forms. */
    static const char *const no_mdns_good[] = { "192.168.1.50", "192.168.4.1", "onocoydiy", "onocoydiy.lan",
                                                "onocoydiy.home", "onocoydiy.home.arpa", "onocoydiy.localdomain",
                                                "onocoydiy.internal" };
    static const char *const no_mdns_bad[] = { "onocoydiy.local", "onocoydiy-2.local", "evil.local", ".local" };
    const char *const off[] = { "", NULL };
    for (size_t k = 0; k < 2; k++) {
        n.mdns_host = off[k];
        for (size_t i = 0; i < sizeof(no_mdns_good) / sizeof(no_mdns_good[0]); i++) {
            TEST_ASSERT_TRUE_MESSAGE(web_host_allowed(no_mdns_good[i], &n), no_mdns_good[i]);
        }
        for (size_t i = 0; i < sizeof(no_mdns_bad) / sizeof(no_mdns_bad[0]); i++) {
            TEST_ASSERT_FALSE_MESSAGE(web_host_allowed(no_mdns_bad[i], &n), no_mdns_bad[i]);
        }
    }
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

/* A session's key (the SHA-256 of its token in the firmware): any 32 bytes here. */
static void key(uint8_t k[WEB_SESSION_KEY_LEN], uint8_t seed)
{
    for (int i = 0; i < (int)WEB_SESSION_KEY_LEN; i++) {
        k[i] = (uint8_t)(seed + i);
    }
}

#define DAY_S     86400u
#define T_UNIX    1790000000LL   /* a UTC time in 2026 */

TEST_CASE("sessions expire after 30 min idle and 24 h in all", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[WEB_SESSION_KEY_LEN];
    key(a, 1);
    uint32_t now = UINT32_MAX - 100;   /* across the clock wrap */
    int idx = web_sess_create(&t, a, false, now, T_UNIX);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_INT64(0, t.s[idx].end_unix);   /* not remembered: no UTC end */
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + 1799));
    TEST_ASSERT_EQUAL_UINT32(1, web_sess_left_s(&t, idx, now + 1799));
    web_sess_touch(&t, idx, now + 1799);
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + 3500));
    web_sess_touch(&t, idx, now + 3500);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + 3500 + WEB_SESSION_IDLE_S));

    /* Absolute lifetime, however busy. */
    idx = web_sess_create(&t, a, false, now, 0);
    uint32_t gen = t.s[idx].gen;
    for (uint32_t s = 600; s < WEB_SESSION_LIFE_S; s += 600) {
        TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + s));
        web_sess_touch(&t, idx, now + s);
    }
    TEST_ASSERT_TRUE(web_sess_alive(&t, idx, gen));
    /* Expired: found no more, ended (and reported) by the purge. */
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + WEB_SESSION_LIFE_S));
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_expire(&t, now + WEB_SESSION_LIFE_S));
    TEST_ASSERT_FALSE(web_sess_alive(&t, idx, gen));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + WEB_SESSION_LIFE_S));
}

TEST_CASE("a remembered session lasts 30 days from its login, with no idle timeout", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[WEB_SESSION_KEY_LEN];
    key(a, 7);
    uint32_t now = UINT32_MAX - 1000;
    int idx = web_sess_create(&t, a, true, now, T_UNIX);
    TEST_ASSERT_TRUE(t.s[idx].remember);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, t.s[idx].end_unix);
    TEST_ASSERT_EQUAL_UINT32(WEB_SESSION_REMEMBER_S, web_sess_left_s(&t, idx, now));
    /* Untouched for days (no 30 min idle cutoff, no 24 h limit). */
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + 10 * DAY_S));
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_expire(&t, now + 10 * DAY_S));
    TEST_ASSERT_EQUAL_UINT32(20 * DAY_S, web_sess_left_s(&t, idx, now + 10 * DAY_S));
    /* Touching it does not extend it: 30 days from the login. */
    web_sess_touch(&t, idx, now + 29 * DAY_S);
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, now + WEB_SESSION_REMEMBER_S - 1));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, now + WEB_SESSION_REMEMBER_S));
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_expire(&t, now + WEB_SESSION_REMEMBER_S));

    /* Made without the clock: no UTC end until the clock is seen. */
    idx = web_sess_create(&t, a, true, now, 0);
    TEST_ASSERT_EQUAL_INT64(0, t.s[idx].end_unix);
}

TEST_CASE("a full table ends the least recently used session, a remembered one last", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t k[WEB_SESSIONS_MAX + 2][WEB_SESSION_KEY_LEN];
    int idx[WEB_SESSIONS_MAX + 2];
    uint32_t gen[WEB_SESSIONS_MAX + 2];
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        key(k[i], (uint8_t)(10 * i));
        idx[i] = web_sess_create(&t, k[i], false, 100 + (uint32_t)i, 0);
        gen[i] = t.s[idx[i]].gen;
    }
    web_sess_touch(&t, idx[0], 200);   /* 1 is now the least recently used */
    size_t m = WEB_SESSIONS_MAX;
    key(k[m], 200);
    idx[m] = web_sess_create(&t, k[m], false, 201, 0);
    TEST_ASSERT_EQUAL(idx[1], idx[m]);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, k[1], 202));
    TEST_ASSERT_FALSE(web_sess_alive(&t, idx[1], gen[1]));
    TEST_ASSERT_EQUAL(idx[0], web_sess_find(&t, k[0], 202));
    TEST_ASSERT_EQUAL(idx[m], web_sess_find(&t, k[m], 202));

    /* Password change: every other session ends. */
    uint32_t all = (1u << WEB_SESSIONS_MAX) - 1u;
    uint32_t mask = web_sess_end_all_except(&t, idx[0]);
    TEST_ASSERT_EQUAL_UINT32(all & ~(1u << idx[0]), mask);
    TEST_ASSERT_EQUAL(idx[0], web_sess_find(&t, k[0], 203));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, k[2], 203));
    TEST_ASSERT_TRUE(web_sess_alive(&t, idx[0], gen[0]));
    web_sess_end(&t, idx[0]);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, k[0], 203));

    /* Remembered devices keep their place: a session that is not remembered goes first, even the newest one. */
    web_sess_init(&t);
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        idx[i] = web_sess_create(&t, k[i], i != 3, 100 + (uint32_t)i, T_UNIX);
    }
    key(k[m + 1], 222);
    TEST_ASSERT_EQUAL(idx[3], web_sess_create(&t, k[m + 1], false, 300, T_UNIX));
    TEST_ASSERT_EQUAL(idx[0], web_sess_find(&t, k[0], 301));
    TEST_ASSERT_EQUAL(idx[3], web_sess_create(&t, k[m + 1], true, 302, T_UNIX));
    /* Every session remembered now: the least recently used of them. */
    web_sess_touch(&t, idx[0], 303);
    TEST_ASSERT_EQUAL(idx[1], web_sess_create(&t, k[m], true, 304, T_UNIX));
}

TEST_CASE("the clock stamps a remembered session's UTC end and ends one whose end has passed", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[WEB_SESSION_KEY_LEN];
    uint8_t b[WEB_SESSION_KEY_LEN];
    uint8_t c[WEB_SESSION_KEY_LEN];
    key(a, 1);
    key(b, 2);
    key(c, 3);
    int ia = web_sess_create(&t, a, true, 1000, 0);        /* before the clock */
    int ib = web_sess_create(&t, b, true, 1000, T_UNIX);   /* with it */
    int ic = web_sess_create(&t, c, false, 1000, 0);
    bool changed = true;
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 1500, 0, &changed));   /* no clock: nothing */
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 1500, T_UNIX + 500, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 500 + WEB_SESSION_REMEMBER_S - 500, t.s[ia].end_unix);   /* now + time left */
    TEST_ASSERT_EQUAL_INT64(0, t.s[ic].end_unix);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 500, web_sess_mark_now(&t, 1500, 0));                    /* the mark follows */
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 1501, T_UNIX + 501, &changed));
    TEST_ASSERT_FALSE(changed);   /* once */

    /* The clock jumps ahead (the station learns the true date): a UTC end sooner than the monotonic one counts. */
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 1600, T_UNIX + 20 * (int64_t)DAY_S, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(10 * DAY_S, web_sess_left_s(&t, ib, 1600));
    /* And past it: ended, reported in the mask. */
    uint32_t mask = web_sess_clock(&t, 1700, T_UNIX + 31 * (int64_t)DAY_S, &changed);
    TEST_ASSERT_EQUAL_UINT32((1u << ia) | (1u << ib), mask);
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, a, 1700));
    TEST_ASSERT_EQUAL(ic, web_sess_find(&t, c, 1700));
}

TEST_CASE("an end is never more than 30 days ahead of the clock, and is stored cut back", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[WEB_SESSION_KEY_LEN];
    key(a, 4);
    /* A login while the trusted clock is a year ahead. */
    const int64_t ahead = T_UNIX + 365 * (int64_t)DAY_S;
    int idx = web_sess_create(&t, a, true, 100, ahead);
    TEST_ASSERT_EQUAL_INT64(ahead + WEB_SESSION_REMEMBER_S, t.s[idx].end_unix);
    /* The clock is put right: the end is cut back to it + 30 days, and the store must follow. */
    bool changed = false;
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 110, T_UNIX, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, t.s[idx].end_unix);
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 111, T_UNIX + 1, &changed));
    TEST_ASSERT_FALSE(changed);
    uint8_t bind[WEB_SESS_BIND_LEN] = { 0 };
    uint8_t buf[WEB_SESS_STORE_MAX_LEN];
    size_t count = 0;
    size_t len = web_sess_store_encode(&t, bind, web_sess_mark_now(&t, 111, T_UNIX + 1), buf, sizeof(buf), &count);
    web_sess_rec_t recs[WEB_SESSIONS_MAX];
    int64_t mark = 0;
    size_t n = 0;
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, bind, &mark, recs, &n));
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, recs[0].end_unix);
    /* The mark kept the year-ahead time (a clock behind it moves nothing back): at the next boot that session is past. */
    TEST_ASSERT_TRUE(mark >= ahead);
    web_sess_init(&t);
    web_sess_mark(&t, mark, 5);
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, T_UNIX + DAY_S));
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, 0));

    /* A stored end 90 days ahead of the clock at boot (the clock went back since): cut to 30 days, also as stored, so
     * the next boot does not get 30 days again. */
    web_sess_init(&t);
    web_sess_mark(&t, T_UNIX - DAY_S, 5);
    web_sess_rec_t r;
    key(r.key, 5);
    r.end_unix = T_UNIX + 90 * (int64_t)DAY_S;
    idx = web_sess_restore(&t, &r, 5, T_UNIX);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_UINT32(WEB_SESSION_REMEMBER_S, web_sess_left_s(&t, idx, 5));
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, t.s[idx].end_unix);
    len = web_sess_store_encode(&t, bind, web_sess_mark_now(&t, 5, T_UNIX), buf, sizeof(buf), &count);
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, bind, &mark, recs, &n));
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, recs[0].end_unix);
    web_sess_init(&t);
    web_sess_mark(&t, mark, 5);
    idx = web_sess_restore(&t, &recs[0], 5, T_UNIX + 10 * (int64_t)DAY_S);   /* ten days later */
    TEST_ASSERT_EQUAL_UINT32(20 * DAY_S, web_sess_left_s(&t, idx, 5));
}

TEST_CASE("a clock stepping back and forth past an end revives nothing at the next boot", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t a[WEB_SESSION_KEY_LEN];
    key(a, 6);
    uint8_t bind[WEB_SESS_BIND_LEN] = { 0 };
    uint8_t buf[WEB_SESS_STORE_MAX_LEN];
    web_sess_rec_t recs[WEB_SESSIONS_MAX];
    int64_t mark = 0;
    size_t count = 0;
    size_t n = 0;

    /* Back 40 days: the end is cut to the clock + 30 days, the mark stays; the session lives on (its uptime count). */
    int idx = web_sess_create(&t, a, true, 100, T_UNIX);
    bool changed = false;
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 200, T_UNIX - 40 * (int64_t)DAY_S + 100, &changed));
    TEST_ASSERT_TRUE(changed);
    TEST_ASSERT_EQUAL_INT64(T_UNIX - 10 * (int64_t)DAY_S + 100, t.s[idx].end_unix);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 100, web_sess_mark_now(&t, 200, 0));
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, a, 300));
    size_t len = web_sess_store_encode(&t, bind, web_sess_mark_now(&t, 200, 0), buf, sizeof(buf), &count);
    /* Forward again, past that end: it ends. */
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_clock(&t, 300, T_UNIX + 200, &changed));
    /* A boot from the record written in between takes nothing back, with or without the clock. */
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, bind, &mark, recs, &n));
    web_sess_init(&t);
    web_sess_mark(&t, mark, 5);
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, 0));
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, T_UNIX - 40 * (int64_t)DAY_S));

    /* Forward past the end, then back: the session ended, and a stale record of it stays dead under the mark. */
    web_sess_init(&t);
    idx = web_sess_create(&t, a, true, 100, T_UNIX);
    len = web_sess_store_encode(&t, bind, web_sess_mark_now(&t, 100, T_UNIX), buf, sizeof(buf), &count);
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_clock(&t, 200, T_UNIX + 31 * (int64_t)DAY_S, &changed));
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, 300, T_UNIX + DAY_S, &changed));
    int64_t late = web_sess_mark_now(&t, 300, T_UNIX + DAY_S);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 31 * (int64_t)DAY_S + 100, late);   /* the step back moved nothing */
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, bind, &mark, recs, &n));
    web_sess_init(&t);
    web_sess_mark(&t, late, 5);
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, 0));
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &recs[0], 5, T_UNIX + DAY_S));
}

TEST_CASE("without the clock at boot, the mark decides: from it to the end, none that ended before it", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    web_sess_mark(&t, T_UNIX + 20 * (int64_t)DAY_S, 5);   /* the stored mark */
    web_sess_rec_t r;
    key(r.key, 7);
    r.end_unix = T_UNIX + 10 * (int64_t)DAY_S;   /* ended before the mark (while the station was off, or earlier) */
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &r, 5, 0));
    r.end_unix = T_UNIX + 20 * (int64_t)DAY_S;
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &r, 5, 0));
    r.end_unix = T_UNIX + 25 * (int64_t)DAY_S;   /* five days past the mark: five days, not thirty */
    int idx = web_sess_restore(&t, &r, 5, 0);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_UINT32(5 * DAY_S, web_sess_left_s(&t, idx, 5));
    TEST_ASSERT_EQUAL_INT64(r.end_unix, t.s[idx].end_unix);
    /* Running on without a clock, the mark is carried forward (the station ran that long at least). */
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 20 * (int64_t)DAY_S + 3600, web_sess_mark_now(&t, 5 + 3600, 0));
    /* A trusted clock behind the mark gives nothing back. */
    web_sess_mark(&t, T_UNIX, 5 + 3600);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 20 * (int64_t)DAY_S + 3600, web_sess_mark_now(&t, 5 + 3600, T_UNIX));
}

TEST_CASE("the stored mark is refreshed at most once a day", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    web_sess_mark(&t, T_UNIX, 0);
    /* Three days without a clock, checked every minute: three writes. */
    int64_t stored = T_UNIX;
    int writes = 0;
    for (uint32_t s = 0; s <= 3 * DAY_S; s += 60) {
        int64_t now = web_sess_mark_now(&t, s, 0);
        if (web_sess_mark_due(stored, now)) {
            stored = now;
            writes++;
        }
    }
    TEST_ASSERT_EQUAL(3, writes);
    /* A clock far ahead: one write, then none for a day. */
    int64_t now = web_sess_mark_now(&t, 3 * DAY_S, T_UNIX + 40 * (int64_t)DAY_S);
    TEST_ASSERT_TRUE(web_sess_mark_due(stored, now));
    stored = now;
    web_sess_mark(&t, now, 3 * DAY_S);
    TEST_ASSERT_FALSE(web_sess_mark_due(stored, web_sess_mark_now(&t, 4 * DAY_S - 1, 0)));
    TEST_ASSERT_TRUE(web_sess_mark_due(stored, web_sess_mark_now(&t, 4 * DAY_S, 0)));
    TEST_ASSERT_FALSE(web_sess_mark_due(0, 0));   /* nothing known */
}

TEST_CASE("stored sessions: only remembered ones with a UTC end, bound to the password's salt", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    uint8_t k[4][WEB_SESSION_KEY_LEN];
    for (int i = 0; i < 4; i++) {
        key(k[i], (uint8_t)(40 * i));
    }
    (void)web_sess_create(&t, k[0], true, 10, T_UNIX);
    (void)web_sess_create(&t, k[1], false, 10, T_UNIX);   /* not remembered */
    (void)web_sess_create(&t, k[2], true, 10, 0);         /* no UTC end yet */
    (void)web_sess_create(&t, k[3], true, 20, T_UNIX + 10);
    uint8_t bind[WEB_SESS_BIND_LEN];
    memset(bind, 0xA5, sizeof(bind));
    uint8_t buf[WEB_SESS_STORE_MAX_LEN];
    size_t count = 99;
    size_t len = web_sess_store_encode(&t, bind, T_UNIX + 15, buf, sizeof(buf), &count);
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL(1 + WEB_SESS_BIND_LEN + 8 + 1 + 2 * (WEB_SESSION_KEY_LEN + 8), len);
    TEST_ASSERT_EQUAL_UINT8(WEB_SESS_STORE_VERSION, buf[0]);
    /* No token anywhere: only the keys (the tokens' digests), the ends and the mark. */
    uint8_t got_bind[WEB_SESS_BIND_LEN];
    web_sess_rec_t recs[WEB_SESSIONS_MAX];
    int64_t mark = 0;
    size_t n = 0;
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, got_bind, &mark, recs, &n));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 15, mark);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bind, got_bind, sizeof(bind));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k[0], recs[0].key, WEB_SESSION_KEY_LEN);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + WEB_SESSION_REMEMBER_S, recs[0].end_unix);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k[3], recs[1].key, WEB_SESSION_KEY_LEN);
    TEST_ASSERT_EQUAL_INT64(T_UNIX + 10 + WEB_SESSION_REMEMBER_S, recs[1].end_unix);

    /* Anything else is refused: a short or long record, another version (v1 had no mark), more sessions than the
     * table holds. */
    TEST_ASSERT_FALSE(web_sess_store_decode(buf, len - 1, got_bind, &mark, recs, &n));
    uint8_t longer[WEB_SESS_STORE_MAX_LEN + 1];
    memcpy(longer, buf, len);
    longer[len] = 0;
    TEST_ASSERT_FALSE(web_sess_store_decode(longer, len + 1, got_bind, &mark, recs, &n));
    buf[0] = 1;
    TEST_ASSERT_FALSE(web_sess_store_decode(buf, len, got_bind, &mark, recs, &n));
    buf[0] = WEB_SESS_STORE_VERSION;
    buf[1 + WEB_SESS_BIND_LEN + 8] = WEB_SESSIONS_MAX + 1;
    TEST_ASSERT_FALSE(web_sess_store_decode(buf, len, got_bind, &mark, recs, &n));
    TEST_ASSERT_EQUAL(0, web_sess_store_encode(&t, bind, T_UNIX, buf, 30, &count));   /* too small */

    /* A password change keeps the current session and ends every other one: one record left. */
    web_sess_end_all_except(&t, 0);
    len = web_sess_store_encode(&t, bind, T_UNIX + 15, buf, sizeof(buf), &count);
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_TRUE(web_sess_store_decode(buf, len, got_bind, &mark, recs, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k[0], recs[0].key, WEB_SESSION_KEY_LEN);
    /* Logout of that one too: nothing to store. */
    web_sess_end(&t, 0);
    (void)web_sess_store_encode(&t, bind, T_UNIX + 15, buf, sizeof(buf), &count);
    TEST_ASSERT_EQUAL(0, count);
}

TEST_CASE("restore: the time the UTC end leaves; without the clock from the mark until the clock ends it", "[web_core]")
{
    web_sessions_t t;
    web_sess_init(&t);
    web_sess_rec_t r;
    key(r.key, 9);
    uint32_t boot = 5;

    /* The clock is set at boot: 3 days left of the 30. */
    r.end_unix = T_UNIX + 3 * (int64_t)DAY_S;
    int idx = web_sess_restore(&t, &r, boot, T_UNIX);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_TRUE(t.s[idx].remember);
    TEST_ASSERT_EQUAL_UINT32(3 * DAY_S, web_sess_left_s(&t, idx, boot));
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, r.key, boot + 3 * DAY_S - 1));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, r.key, boot + 3 * DAY_S));

    /* Ended while the station was off: not taken back. No UTC end: not either. */
    web_sess_init(&t);
    r.end_unix = T_UNIX - 1;
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &r, boot, T_UNIX));
    r.end_unix = 0;
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &r, boot, T_UNIX));

    /* The clock not set yet: from the stored mark to its end; once the clock shows its end passed, it ends. */
    web_sess_init(&t);
    web_sess_mark(&t, T_UNIX, boot);
    r.end_unix = T_UNIX + 100;
    idx = web_sess_restore(&t, &r, boot, 0);
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_UINT32(100, web_sess_left_s(&t, idx, boot));
    TEST_ASSERT_EQUAL(idx, web_sess_find(&t, r.key, boot + 60));
    bool changed = false;
    TEST_ASSERT_EQUAL_UINT32(0, web_sess_clock(&t, boot + 60, T_UNIX + 70, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(30, web_sess_left_s(&t, idx, boot + 60));   /* the station was off 10 s more than the mark knew */
    TEST_ASSERT_EQUAL_UINT32(1u << idx, web_sess_clock(&t, boot + 120, T_UNIX + 130, &changed));
    TEST_ASSERT_EQUAL(-1, web_sess_find(&t, r.key, boot + 120));

    /* A full table takes nothing back (it ends no session for a stored one). */
    web_sess_init(&t);
    uint8_t k[WEB_SESSION_KEY_LEN];
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        key(k, (uint8_t)(100 + i));
        (void)web_sess_create(&t, k, false, boot, 0);
    }
    r.end_unix = T_UNIX + 100;
    TEST_ASSERT_EQUAL(-1, web_sess_restore(&t, &r, boot, T_UNIX));
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

TEST_CASE("caster host names", "[web_core]")
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
    TEST_ASSERT_EQUAL(WEB_ERR_COUNT, web_app_upload_verdict(WEB_IMAGE_OK, false, false, false, false));
    TEST_ASSERT_EQUAL(WEB_ERR_ROLLED_BACK_ACK_REQUIRED, web_app_upload_verdict(WEB_IMAGE_OK, true, false, false, false));
    TEST_ASSERT_EQUAL(WEB_ERR_COUNT, web_app_upload_verdict(WEB_IMAGE_OK, true, true, false, false));
    TEST_ASSERT_EQUAL(WEB_ERR_NOT_THIS_PROJECT, web_app_upload_verdict(WEB_IMAGE_OTHER_PROJECT, false, true, false, false));
    TEST_ASSERT_EQUAL(WEB_ERR_INVALID_IMAGE, web_app_upload_verdict(WEB_IMAGE_NOT_APP, true, true, false, false));
    TEST_ASSERT_EQUAL_STRING("rolled_back_ack_required", web_err_code(WEB_ERR_ROLLED_BACK_ACK_REQUIRED));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_ROLLED_BACK_ACK_REQUIRED));
}

TEST_CASE("app upload: the image already running is refused unless a reinstall was asked for", "[web_core]")
{
    TEST_ASSERT_EQUAL(WEB_ERR_SAME_IMAGE, web_app_upload_verdict(WEB_IMAGE_OK, false, false, true, false));
    TEST_ASSERT_EQUAL(WEB_ERR_COUNT, web_app_upload_verdict(WEB_IMAGE_OK, false, false, true, true));
    /* Another project or no app at all: those refusals come first. */
    TEST_ASSERT_EQUAL(WEB_ERR_NOT_THIS_PROJECT, web_app_upload_verdict(WEB_IMAGE_OTHER_PROJECT, false, false, true, false));
    TEST_ASSERT_EQUAL(WEB_ERR_INVALID_IMAGE, web_app_upload_verdict(WEB_IMAGE_NOT_APP, false, false, true, false));
    TEST_ASSERT_EQUAL_STRING("same_image", web_err_code(WEB_ERR_SAME_IMAGE));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_SAME_IMAGE));
    /* Noted as what it is (the Updates page's "Last job"), not as a state change. */
    TEST_ASSERT_EQUAL_STRING("same_image", web_app_refusal_result(WEB_ERR_SAME_IMAGE));
    TEST_ASSERT_EQUAL_STRING("rolled_back_ack_required", web_app_refusal_result(WEB_ERR_ROLLED_BACK_ACK_REQUIRED));
    TEST_ASSERT_EQUAL_STRING("invalid_image", web_app_refusal_result(WEB_ERR_NOT_THIS_PROJECT));
    TEST_ASSERT_EQUAL_STRING("invalid_image", web_app_refusal_result(WEB_ERR_INVALID_IMAGE));
    TEST_ASSERT_EQUAL_STRING("not_no_receiver", web_err_code(WEB_ERR_NOT_NO_RECEIVER));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_NOT_NO_RECEIVER));
}

TEST_CASE("app upload check reads the app's ELF SHA-256 from the description", "[web_core]")
{
    uint8_t img[256];
    web_image_info_t info;
    fake_image(img, sizeof(img), 9, "onocoydiy-esp-um982", "v0.4.0");
    for (int i = 0; i < 32; i++) {
        img[WEB_IMAGE_DESC_OFS + WEB_DESC_ELF_SHA_OFS + i] = (uint8_t)(0xA0 + i);
    }
    TEST_ASSERT_EQUAL(WEB_IMAGE_OK, web_image_check(img, sizeof(img), 9, "onocoydiy-esp-um982", &info));
    TEST_ASSERT_TRUE(info.has_elf_sha);
    TEST_ASSERT_EQUAL_HEX8(0xA0, info.elf_sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBF, info.elf_sha256[31]);
    /* A head that stops before the digest: the rest is still checked, the digest unknown. */
    TEST_ASSERT_EQUAL(WEB_IMAGE_OK, web_image_check(img, WEB_IMAGE_HEAD_SHA - 1, 9, "onocoydiy-esp-um982", &info));
    TEST_ASSERT_FALSE(info.has_elf_sha);
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

/* ---- client and local addresses ------------------------------------------------------- */

static web_addr_t v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    web_addr_t x = { .v6 = false, .b = { a, b, c, d } };
    return x;
}

static web_addr_t v6(const uint8_t bytes[16])
{
    web_addr_t x = { .v6 = true };
    memcpy(x.b, bytes, 16);
    return x;
}

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t q[4] = { a, b, c, d };
    uint32_t v;
    memcpy(&v, q, 4);   /* network order, as sin_addr.s_addr and esp_ip4_addr_t keep it */
    return v;
}

static const uint8_t k_mapped_ap[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 1 };
static const uint8_t k_ap_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x36, 0x85, 0x18, 0xFF, 0xFE, 0x0A, 0x2B, 0x3D };
static const uint8_t k_sta_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x36, 0x85, 0x18, 0xFF, 0xFE, 0x0A, 0x2B, 0x3C };
static const uint8_t k_phone_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x1C, 0x2A, 0x77, 0x01, 0x9E, 0x30, 0x4B, 0x11 };
static const uint8_t k_laptop_ll[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0x88, 0x10, 0x02, 0xAA, 0x51, 0x6C, 0x09, 0xE2 };
static const uint8_t k_global_a[16] = { 0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0, 0, 0, 0, 1 };
static const uint8_t k_global_b[16] = { 0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x78, 0xAB, 0xCD, 0, 0, 0, 0, 0, 2 };
static const uint8_t k_global_c[16] = { 0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x79, 0, 0, 0, 0, 0, 0, 0, 1 };

TEST_CASE("client key: IPv4 and v4-mapped alike, each link-local host apart, a global /64 together", "[web_core]")
{
    web_addr_t a = v4(192, 168, 4, 2);
    web_addr_t m = v6((const uint8_t[16]){ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 4, 2 });
    TEST_ASSERT_EQUAL_UINT64(ip4(192, 168, 4, 2), web_client_key(&a));
    TEST_ASSERT_EQUAL_UINT64(web_client_key(&a), web_client_key(&m));
    web_addr_t other = v4(192, 168, 4, 3);
    TEST_ASSERT_NOT_EQUAL(web_client_key(&a), web_client_key(&other));

    /* Two phones on the setup AP share fe80::/64: they must not share a throttle bucket. */
    web_addr_t p1 = v6(k_phone_ll);
    web_addr_t p2 = v6(k_laptop_ll);
    TEST_ASSERT_NOT_EQUAL(web_client_key(&p1), web_client_key(&p2));
    TEST_ASSERT_EQUAL_UINT64(web_client_key(&p1), web_client_key(&p1));
    /* fe80::/10 is link-local, not only fe80::/16 */
    uint8_t febf[16];
    memcpy(febf, k_phone_ll, 16);
    febf[1] = 0xBF;
    web_addr_t q1 = v6(febf);
    febf[15] ^= 0x01;
    web_addr_t q2 = v6(febf);
    TEST_ASSERT_NOT_EQUAL(web_client_key(&q1), web_client_key(&q2));

    /* A global address: the /64 prefix (one host may use any address in it). */
    web_addr_t g1 = v6(k_global_a);
    web_addr_t g2 = v6(k_global_b);
    web_addr_t g3 = v6(k_global_c);
    TEST_ASSERT_EQUAL_UINT64(web_client_key(&g1), web_client_key(&g2));
    TEST_ASSERT_NOT_EQUAL(web_client_key(&g1), web_client_key(&g3));
    TEST_ASSERT_EQUAL_UINT64(0, web_client_key(NULL));

    /* The throttle keeps link-local clients apart through the key. */
    web_throttles_t t;
    web_throttles_init(&t, 0);
    for (uint32_t i = 0; i < WEB_THROTTLE_FREE; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, web_client_key(&p1), true, 0));
        web_throttles_fail(&t, web_client_key(&p1), true, 0);
    }
    TEST_ASSERT_NOT_EQUAL(0, web_throttles_wait_s(&t, web_client_key(&p1), true, 0));
    TEST_ASSERT_EQUAL_UINT32(0, web_throttles_wait_s(&t, web_client_key(&p2), true, 0));
}

TEST_CASE("on the AP: its IPv4 address, v4-mapped too, and its link-local address", "[web_core]")
{
    const uint32_t ap = ip4(192, 168, 4, 1);
    web_addr_t a = v4(192, 168, 4, 1);
    web_addr_t sta = v4(192, 168, 1, 50);
    web_addr_t m = v6(k_mapped_ap);
    web_addr_t ll_ap = v6(k_ap_ll);
    web_addr_t ll_sta = v6(k_sta_ll);
    TEST_ASSERT_TRUE(web_addr_on_ap(&a, ap, k_ap_ll));
    TEST_ASSERT_TRUE(web_addr_on_ap(&m, ap, k_ap_ll));
    TEST_ASSERT_TRUE(web_addr_on_ap(&ll_ap, ap, k_ap_ll));
    TEST_ASSERT_FALSE(web_addr_on_ap(&ll_sta, ap, k_ap_ll));
    TEST_ASSERT_FALSE(web_addr_on_ap(&sta, ap, k_ap_ll));
    /* the AP without a link-local address yet, or without an address at all */
    TEST_ASSERT_FALSE(web_addr_on_ap(&ll_ap, ap, NULL));
    TEST_ASSERT_TRUE(web_addr_on_ap(&a, ap, NULL));
    TEST_ASSERT_FALSE(web_addr_on_ap(&a, 0, NULL));
    TEST_ASSERT_FALSE(web_addr_on_ap(NULL, ap, k_ap_ll));
    /* A literal IPv6 Host stays refused: only names and the IPv4 addresses pass. */
    web_host_names_t names = { .sta_ip = "192.168.1.50", .ap_ip = "192.168.4.1", .hostname = "onocoydiy",
                               .mdns_host = "onocoydiy" };
    TEST_ASSERT_FALSE(web_host_allowed("[fe80::3685:18ff:fe0a:2b3d]", &names));
    TEST_ASSERT_FALSE(web_host_allowed("[fe80::3685:18ff:fe0a:2b3d]:80", &names));
    TEST_ASSERT_FALSE(web_host_allowed("fe80::3685:18ff:fe0a:2b3d", &names));
    TEST_ASSERT_TRUE(web_host_allowed("onocoydiy.local", &names));
}

TEST_CASE("address text: IPv4, v4-mapped as IPv4, IPv6 compressed", "[web_core]")
{
    char buf[48];
    web_addr_t a = v4(10, 0, 2, 2);
    TEST_ASSERT_EQUAL(8, web_addr_text(&a, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("10.0.2.2", buf);
    web_addr_t m = v6(k_mapped_ap);
    web_addr_text(&m, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", buf);
    web_addr_t ll = v6(k_ap_ll);
    web_addr_text(&ll, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("fe80::3685:18ff:fe0a:2b3d", buf);
    web_addr_t g = v6(k_global_a);
    web_addr_text(&g, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("2001:db8:1234:5678::1", buf);
    static const struct {
        uint8_t b[16];
        const char *want;
    } cases[] = {
        { { 0 }, "::" },
        { { [15] = 1 }, "::1" },
        { { 0x20, 0x01, 0x0D, 0xB8 }, "2001:db8::" },
        /* a single zero group is not compressed */
        { { 0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 }, "2001:db8:0:1:1:1:1:1" },
        /* the longest run wins; of equal runs the first */
        { { 0x20, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 }, "2001:0:0:1::1" },
        { { 0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 }, "2001:db8::1:0:0:1" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        web_addr_t x = v6(cases[i].b);
        web_addr_text(&x, buf, sizeof(buf));
        TEST_ASSERT_EQUAL_STRING(cases[i].want, buf);
    }
    web_addr_text(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("?", buf);
    char small[6];
    web_addr_text(&a, small, sizeof(small));   /* cut, terminated */
    TEST_ASSERT_EQUAL_STRING("10.0.", small);
}

/* ---- requests no route took ------------------------------------------------------------ */

static bool api_known_for_test(const char *path)
{
    return strcmp(path, "/api/status") == 0 || strcmp(path, "/ws") == 0 || strcmp(path, "/api/session") == 0;
}

TEST_CASE("catch-all: the answer is classified before any body is read", "[web_core]")
{
    static const struct {
        const char *uri;
        bool options;
        bool get;
        bool asset;
        bool ap;
        web_fallback_t want;
    } rows[] = {
        { "/api/status", true, false, false, false, WEB_FB_METHOD },      /* OPTIONS: no CORS */
        { "/", true, false, true, true, WEB_FB_METHOD },
        { "/api/status", false, false, false, false, WEB_FB_METHOD },     /* POST to a GET route */
        { "/api/status?x=1", false, false, false, true, WEB_FB_METHOD },  /* the query does not count */
        { "/ws", false, false, false, false, WEB_FB_METHOD },
        { "/api/nope", false, false, false, false, WEB_FB_NOT_FOUND },
        { "/api/nope", false, false, false, true, WEB_FB_NOT_FOUND },     /* no redirect for /api, even on the AP */
        { "/api", false, true, false, false, WEB_FB_NOT_FOUND },
        { "/apix", false, true, false, true, WEB_FB_REDIRECT },           /* not /api */
        { "/wsx", false, true, false, false, WEB_FB_NOT_FOUND },
        { "/app.js", false, true, true, false, WEB_FB_ASSET },
        { "/app.js", false, true, true, true, WEB_FB_ASSET },
        { "/app.js", false, false, true, false, WEB_FB_METHOD },          /* POST to an asset */
        { "/nope", false, true, false, false, WEB_FB_NOT_FOUND },
        { "/nope", false, false, false, false, WEB_FB_NOT_FOUND },
        { "/generate_204", false, true, false, true, WEB_FB_REDIRECT },   /* the captive portal */
        { "/nope", false, false, false, true, WEB_FB_REDIRECT },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].want,
                                  web_fallback_classify(rows[i].uri, rows[i].options, rows[i].get, rows[i].asset,
                                                        rows[i].ap, api_known_for_test),
                                  rows[i].uri);
    }
    /* A path too long for the route table is unknown. */
    char longpath[80] = "/api/";
    memset(longpath + 5, 'a', 70);
    longpath[75] = '\0';
    TEST_ASSERT_EQUAL(WEB_FB_NOT_FOUND, web_fallback_classify(longpath, false, false, false, false, api_known_for_test));
    TEST_ASSERT_EQUAL(WEB_FB_NOT_FOUND, web_fallback_classify("/api/status", false, false, false, false, NULL));
}

TEST_CASE("Connection header tokens and when to close", "[web_core]")
{
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_CLOSE, web_connection_tokens("close"));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_CLOSE, web_connection_tokens("Close"));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_CLOSE, web_connection_tokens(" close "));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_KEEP_ALIVE, web_connection_tokens("Keep-Alive, Upgrade"));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_KEEP_ALIVE, web_connection_tokens("keep-alive"));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_CLOSE | WEB_CONN_TOK_KEEP_ALIVE, web_connection_tokens("keep-alive,\tCLOSE"));
    TEST_ASSERT_EQUAL_UINT(0, web_connection_tokens("Upgrade"));
    TEST_ASSERT_EQUAL_UINT(0, web_connection_tokens("closed, keep-alives"));
    TEST_ASSERT_EQUAL_UINT(0, web_connection_tokens(""));
    TEST_ASSERT_EQUAL_UINT(0, web_connection_tokens(NULL));
    TEST_ASSERT_EQUAL_UINT(WEB_CONN_TOK_CLOSE, web_connection_tokens(",,close,,"));

    /* HTTP/1.1: persistent unless "close". HTTP/1.0: always closed, also
     * with "keep-alive": the answer would have to echo the token for the
     * client to treat the connection as persistent (it reads to the end of
     * the connection otherwise), and a chunked answer cannot go to an
     * HTTP/1.0 client on a kept connection at all. */
    TEST_ASSERT_FALSE(web_conn_should_close(0, false));
    TEST_ASSERT_FALSE(web_conn_should_close(web_connection_tokens("Keep-Alive, Upgrade"), false));
    TEST_ASSERT_TRUE(web_conn_should_close(web_connection_tokens("close"), false));
    TEST_ASSERT_TRUE(web_conn_should_close(0, true));
    TEST_ASSERT_TRUE(web_conn_should_close(web_connection_tokens("keep-alive"), true));
    TEST_ASSERT_TRUE(web_conn_should_close(web_connection_tokens("Keep-Alive, Upgrade"), true));
    TEST_ASSERT_TRUE(web_conn_should_close(web_connection_tokens("keep-alive, close"), true));
}

TEST_CASE("a request's body: none, Content-Length, or a Transfer-Encoding nobody reads", "[web_core]")
{
    TEST_ASSERT_EQUAL(WEB_REQ_BODY_NONE, web_req_body(0, false));
    TEST_ASSERT_EQUAL(WEB_REQ_BODY_LENGTH, web_req_body(2, false));
    /* Chunked: the server reports a length of 0 and would parse the chunks
     * as the next request, so it counts as a body, and one with no length. */
    TEST_ASSERT_EQUAL(WEB_REQ_BODY_UNFRAMED, web_req_body(0, true));
    TEST_ASSERT_EQUAL(WEB_REQ_BODY_UNFRAMED, web_req_body(5, true));
    TEST_ASSERT_TRUE(web_req_body(0, true) != WEB_REQ_BODY_NONE);
}

/* ---- log lines about clients ----------------------------------------------------------------- */

TEST_CASE("client log lines: one a minute per client, the held-back ones counted", "[web_core]")
{
    web_lograte_t r;
    web_lograte_init(&r);
    const uint64_t client = 0x0202000A;
    const uint32_t t0 = 4294967200u;   /* across the clock wrap */
    uint32_t held = 99;
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 0, t0, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);
    for (uint32_t i = 1; i <= 4; i++) {   /* the 2nd to 5th within the minute */
        TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 0, t0 + i * 10, &held));
    }
    TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 0, t0 + WEB_LOGRATE_PERIOD_S - 1, NULL));
    /* After the minute: the next line says how many were held back (4 + 1 just now = 5). */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 0, t0 + WEB_LOGRATE_PERIOD_S, &held));
    TEST_ASSERT_EQUAL_UINT32(5, held);
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 0, t0 + 2 * WEB_LOGRATE_PERIOD_S, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);

    /* The acceptance case: 1st logs, 2nd-5th held, the next after 60 s carries +4. */
    web_lograte_init(&r);
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 7, 0, 100, &held));
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_FALSE(web_lograte_allow(&r, 7, 0, 101 + i, &held));
    }
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 7, 0, 161, &held));
    TEST_ASSERT_EQUAL_UINT32(4, held);

    /* Another client is never held back by the first. */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 8, 0, 162, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);

    /* A full table makes room by dropping the client whose last line is oldest. */
    web_lograte_init(&r);
    for (uint64_t k = 1; k <= WEB_LOGRATE_CLIENTS; k++) {
        TEST_ASSERT_TRUE(web_lograte_allow(&r, k, 0, (uint32_t)(1000 + k), NULL));
    }
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 100, 0, 1010, NULL));         /* replaces client 1 */
    TEST_ASSERT_FALSE(web_lograte_allow(&r, 2, 0, 1011, NULL));          /* still known: held */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 1, 0, 1012, NULL));           /* forgotten: a line again */
}

TEST_CASE("client log lines: one a minute per client and reason, held-back lines counted per pair", "[web_core]")
{
    web_lograte_t r;
    web_lograte_init(&r);
    const uint64_t client = 0x0202000A;
    uint32_t held = 99;
    /* Two reasons in one minute: two lines. */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 2, 100, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 4, 101, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);
    /* The same reason again within the minute: held, counted for its own pair. */
    TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 2, 102, &held));
    TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 2, 103, &held));
    TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 2, 104, &held));
    TEST_ASSERT_FALSE(web_lograte_allow(&r, client, 4, 105, &held));
    /* After the minute each pair says how many of its own were held back. */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 2, 100 + WEB_LOGRATE_PERIOD_S, &held));
    TEST_ASSERT_EQUAL_UINT32(3, held);
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client, 4, 101 + WEB_LOGRATE_PERIOD_S, &held));
    TEST_ASSERT_EQUAL_UINT32(1, held);
    /* Another client with the same reason is its own pair. */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, client + 1, 2, 170, &held));
    TEST_ASSERT_EQUAL_UINT32(0, held);

    /* A full table still makes room by dropping the pair whose last line is oldest. */
    web_lograte_init(&r);
    for (uint32_t k = 0; k < WEB_LOGRATE_CLIENTS; k++) {
        TEST_ASSERT_TRUE(web_lograte_allow(&r, 7, (uint8_t)k, 1000 + k, NULL));   /* one client, 8 reasons */
    }
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 7, 200, 1010, NULL));   /* a ninth pair replaces (7, 0) */
    TEST_ASSERT_FALSE(web_lograte_allow(&r, 7, 1, 1011, NULL));    /* still known: held */
    TEST_ASSERT_TRUE(web_lograte_allow(&r, 7, 0, 1012, NULL));     /* forgotten: a line again */
}

TEST_CASE("strict u32 query values (log and journal cursors) and level names", "[web_core]")
{
    uint32_t v = 7;
    TEST_ASSERT_TRUE(web_query_u32("", "cursor", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(web_query_u32("boot=prev", "cursor", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(web_query_u32("cursor=51234&boot=prev", "cursor", &v));
    TEST_ASSERT_EQUAL_UINT32(51234, v);
    TEST_ASSERT_TRUE(web_query_u32("cursor=4294967295", "cursor", &v));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v);
    TEST_ASSERT_TRUE(web_query_u32("cursor=0", "cursor", &v));
    TEST_ASSERT_EQUAL_UINT32(0, v);
    TEST_ASSERT_TRUE(web_query_u32("xcursor=abc", "cursor", &v));   /* another parameter */
    static const char *const bad[] = {
        "cursor=abc", "cursor=", "cursor", "cursor=-1", "cursor=+5", "cursor=%201", "cursor=1%zz", "cursor=1e3",
        "cursor=4294967296", "cursor=99999999999", "cursor=12a", "cursor= 5", "boot=prev&cursor=x",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(web_query_u32(bad[i], "cursor", &v), bad[i]);
    }
    TEST_ASSERT_EQUAL_STRING("none", web_log_level_text(0));
    TEST_ASSERT_EQUAL_STRING("info", web_log_level_text(3));
    TEST_ASSERT_EQUAL_STRING("verbose", web_log_level_text(5));
    TEST_ASSERT_EQUAL_STRING("?", web_log_level_text(6));
}

/* ---- streamed answers to HTTP/1.0 ------------------------------------------------------------ */

TEST_CASE("stream head: a download with its length, an API answer without one, never chunked", "[web_core]")
{
    char buf[512];
    /* A download whose size is known (the core dump). */
    size_t n = web_stream_head(buf, sizeof(buf), 200, "application/octet-stream",
                               "attachment; filename=\"coredump-1.2.3.elf\"", true, 65536);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), n);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "X-Content-Type-Options: nosniff\r\n"
                             "X-Frame-Options: DENY\r\n"
                             "Referrer-Policy: no-referrer\r\n"
                             "Cache-Control: no-store\r\n"
                             "Content-Disposition: attachment; filename=\"coredump-1.2.3.elf\"\r\n"
                             "Connection: close\r\n"
                             "Content-Length: 65536\r\n"
                             "\r\n",
                             buf);
    /* A JSON answer larger than the buffer: no length (the body ends with the connection), no disposition. */
    n = web_stream_head(buf, sizeof(buf), 409, "application/json", NULL, true, -1);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 409 Conflict\r\n"
                             "Content-Type: application/json\r\n"
                             "X-Content-Type-Options: nosniff\r\n"
                             "X-Frame-Options: DENY\r\n"
                             "Referrer-Policy: no-referrer\r\n"
                             "Cache-Control: no-store\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             buf);
    TEST_ASSERT_NULL(strstr(buf, "Transfer-Encoding"));
    TEST_ASSERT_NULL(strstr(buf, "Content-Length"));
    TEST_ASSERT_NULL(strstr(buf, "Content-Disposition"));
    /* Not an API answer: no Cache-Control; a zero length is a length. */
    n = web_stream_head(buf, sizeof(buf), 200, "text/plain; charset=utf-8", NULL, false, 0);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NULL(strstr(buf, "Cache-Control"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\r\nContent-Length: 0\r\n"));
    TEST_ASSERT_NULL(strstr(buf, "Transfer-Encoding"));
    /* Every head ends with the empty line, once. */
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n", buf + n - 4);
    TEST_ASSERT_EQUAL_PTR(buf + n - 4, strstr(buf, "\r\n\r\n"));
}

TEST_CASE("stream head: a buffer too small gives nothing, never half a head", "[web_core]")
{
    char buf[512];
    size_t full = web_stream_head(buf, sizeof(buf), 200, "text/plain; charset=utf-8",
                                  "attachment; filename=\"log-1.txt\"", true, -1);
    TEST_ASSERT_TRUE(full > 0);
    for (size_t cap = 0; cap <= full; cap++) {   /* the terminator needs one more byte */
        char small[512];
        memset(small, 'x', sizeof(small));
        TEST_ASSERT_EQUAL_size_t(0, web_stream_head(cap == 0 ? NULL : small, cap, 200, "text/plain; charset=utf-8",
                                                    "attachment; filename=\"log-1.txt\"", true, -1));
        if (cap > 0) {
            TEST_ASSERT_EQUAL_CHAR('\0', small[0]);
        }
    }
    char exact[512];
    TEST_ASSERT_EQUAL_size_t(full, web_stream_head(exact, full + 1, 200, "text/plain; charset=utf-8",
                                                   "attachment; filename=\"log-1.txt\"", true, -1));
    TEST_ASSERT_EQUAL_STRING(buf, exact);
}

TEST_CASE("common headers: one table for every answer, Cache-Control only on API answers", "[web_core]")
{
    static const char *const want[] = { "X-Content-Type-Options", "X-Frame-Options", "Referrer-Policy",
                                        "Cache-Control" };
    size_t i = 0;
    for (const web_hdr_t *h; (h = web_common_header(i, true)) != NULL; i++) {
        TEST_ASSERT_TRUE(i < 4);
        TEST_ASSERT_EQUAL_STRING(want[i], h->name);
    }
    TEST_ASSERT_EQUAL_size_t(4, i);
    TEST_ASSERT_EQUAL_STRING("no-store", web_common_header(3, true)->value);
    i = 0;
    while (web_common_header(i, false) != NULL) {
        TEST_ASSERT_TRUE(strcmp(web_common_header(i, false)->name, "Cache-Control") != 0);
        i++;
    }
    TEST_ASSERT_EQUAL_size_t(3, i);
}
