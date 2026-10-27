/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Local NTRIP caster, pure core (components/local_caster/lcast_core.c):
 * addresses, the request parser, decisions, replies byte for byte, the STR
 * record, chunks, credentials, the throttle, the journal gate, the stored
 * record and its rules, the rate estimate, and a deterministic fuzz loop
 * over the parser.
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "lcast_core.h"
#include "local_caster.h"
#include "settings_codec.h"
#include "unity.h"

/* ---- helpers ------------------------------------------------------------------------------ */

static lcast_addr_t a4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    lcast_addr_t x;
    memset(&x, 0, sizeof(x));
    x.b[0] = a;
    x.b[1] = b;
    x.b[2] = c;
    x.b[3] = d;
    return x;
}

/* The same address as an IPv4-mapped IPv6 one (::ffff:a.b.c.d). */
static lcast_addr_t a4m(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    lcast_addr_t x;
    memset(&x, 0, sizeof(x));
    x.v6 = true;
    x.b[10] = 0xFF;
    x.b[11] = 0xFF;
    x.b[12] = a;
    x.b[13] = b;
    x.b[14] = c;
    x.b[15] = d;
    return x;
}

static lcast_addr_t a6(const char *text)
{
    lcast_addr_t x;
    memset(&x, 0, sizeof(x));
    x.v6 = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, inet_pton(AF_INET6, text, x.b), text);
    return x;
}

/* An IPv4 address as a uint32_t in network order, as esp_netif keeps it. */
static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t q[4] = { a, b, c, d };
    uint32_t v;
    memcpy(&v, q, 4);
    return v;
}

static lcast_req_result_t feed_str(lcast_req_t *r, const char *s)
{
    return lcast_req_feed(r, (const uint8_t *)s, strlen(s));
}

static lcast_req_result_t parse_str(lcast_req_t *r, const char *s)
{
    lcast_req_reset(r);
    return feed_str(r, s);
}

static uint32_t s_rng = 0x2545F491u;

static uint32_t rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static lcast_req_t s_req;   /* 1.2 KB: off the test stack */

#define REQ_V1 "GET /ROVER HTTP/1.0\r\nUser-Agent: NTRIP RTKLIB/2.4.3\r\nAccept: */*\r\nConnection: close\r\n" \
               "Authorization: Basic cm92ZXI6c2VjcmV0cHc=\r\n\r\n"
#define REQ_V2 "GET /ROVER HTTP/1.1\r\nHost: onodiy.local:2101\r\nNtrip-Version: Ntrip/2.0\r\n"            \
               "User-Agent: NTRIP onodiy-ntrip-client/1.0\r\nAuthorization: Basic cm92ZXI6c2VjcmV0cHc=\r\n" \
               "Connection: close\r\n\r\n"

/* ---- 1. address verdicts -------------------------------------------------------------------- */

TEST_CASE("lcast addresses: every edge of the IPv4 ranges, plain and v4-mapped", "[lcast]")
{
    static const struct {
        uint8_t q[4];
        bool ok;
    } t[] = {
        { { 9, 255, 255, 255 }, false },   { { 10, 0, 0, 0 }, true },       { { 10, 255, 255, 255 }, true },
        { { 11, 0, 0, 0 }, false },        { { 172, 15, 255, 255 }, false }, { { 172, 16, 0, 0 }, true },
        { { 172, 31, 255, 255 }, true },   { { 172, 32, 0, 0 }, false },     { { 192, 167, 255, 255 }, false },
        { { 192, 168, 0, 0 }, true },      { { 192, 168, 255, 255 }, true }, { { 192, 169, 0, 0 }, false },
        { { 169, 254, 0, 1 }, true },      { { 169, 255, 0, 1 }, false },    { { 169, 253, 255, 255 }, false },
        { { 127, 0, 0, 1 }, true },        { { 100, 64, 0, 1 }, false },     { { 100, 127, 255, 255 }, false },
        { { 100, 128, 0, 1 }, false },     { { 0, 0, 0, 0 }, false },        { { 224, 0, 0, 1 }, false },
        { { 255, 255, 255, 255 }, false }, { { 8, 8, 8, 8 }, false },        { { 203, 0, 113, 9 }, false },
    };
    lcast_addr_t local = a4(192, 168, 2, 10);
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "%u.%u.%u.%u", t[i].q[0], t[i].q[1], t[i].q[2], t[i].q[3]);
        lcast_addr_verdict_t want = t[i].ok ? LCAST_ADDR_OK : LCAST_ADDR_NOT_PRIVATE;
        lcast_addr_t p = a4(t[i].q[0], t[i].q[1], t[i].q[2], t[i].q[3]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, lcast_addr_verdict(&p, &local, 0, 0, 0, NULL), msg);
        lcast_addr_t m = a4m(t[i].q[0], t[i].q[1], t[i].q[2], t[i].q[3]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, lcast_addr_verdict(&m, &local, 0, 0, 0, NULL), msg);
    }
}

TEST_CASE("lcast addresses: IPv6 ranges, the gateway and the setup hotspot", "[lcast]")
{
    static const struct {
        const char *a;
        bool ok;
    } t[] = {
        { "fe80::1", true },        { "febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff", true },
        { "fec0::1", false },       { "fc00::1", true },
        { "fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", true },
        { "fbff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", false },
        { "::1", true },            { "::", false },
        { "2001:db8::1", false },   { "64:ff9b::a00:1", false },
        { "::ffff:0:a00:1", false }, { "::a00:1", false },
        { "2002:a00:1::", false },  { "ff02::1", false },
        { "::2", false },
    };
    lcast_addr_t local6 = a6("fe80::1234");
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        lcast_addr_t p = a6(t[i].a);
        TEST_ASSERT_EQUAL_INT_MESSAGE(t[i].ok ? LCAST_ADDR_OK : LCAST_ADDR_NOT_PRIVATE,
                                      lcast_addr_verdict(&p, &local6, 0, 0, 0, NULL), t[i].a);
    }

    /* The gateway: refused when equal (also v4-mapped), not a rule when unknown. */
    lcast_addr_t local = a4(192, 168, 2, 10);
    uint32_t gw = ip4(192, 168, 2, 1);
    lcast_addr_t router = a4(192, 168, 2, 1);
    lcast_addr_t router_m = a4m(192, 168, 2, 1);
    lcast_addr_t phone = a4(192, 168, 2, 50);
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_GATEWAY, lcast_addr_verdict(&router, &local, gw, 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_GATEWAY, lcast_addr_verdict(&router_m, &local, gw, 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&phone, &local, gw, 0, 0, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&router, &local, 0, 0, 0, NULL));
    /* a public gateway address is refused by the ranges first */
    lcast_addr_t pub = a4(203, 0, 113, 1);
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_NOT_PRIVATE, lcast_addr_verdict(&pub, &local, ip4(203, 0, 113, 1), 0, 0, NULL));

    /* The hotspot: the local address is the AP's IPv4 (also v4-mapped) or its link-local address. */
    uint32_t ap4 = ip4(192, 168, 4, 1);
    lcast_addr_t ap_local = a4(192, 168, 4, 1);
    lcast_addr_t ap_local_m = a4m(192, 168, 4, 1);
    lcast_addr_t setup_phone = a4(192, 168, 4, 2);
    lcast_addr_t ll = a6("fe80::aa:bbff:fecc:ddee");
    lcast_addr_t ll_peer = a6("fe80::5");
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&setup_phone, &ap_local, gw, ap4, 0, ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&setup_phone, &ap_local_m, gw, ap4, 0, ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&ll_peer, &ll, gw, ap4, 0, ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&ll_peer, &local6, gw, ap4, 0, ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&phone, &local, gw, ap4, 0, ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&setup_phone, &ap_local, gw, 0, 0, NULL));   /* no AP */
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_NOT_PRIVATE, lcast_addr_verdict(NULL, &local, 0, 0, 0, NULL));

    /* unmapping */
    lcast_addr_t m = a4m(10, 0, 2, 2);
    lcast_addr_unmap(&m);
    TEST_ASSERT_FALSE(m.v6);
    TEST_ASSERT_EQUAL_UINT8(10, m.b[0]);
    TEST_ASSERT_EQUAL_UINT8(2, m.b[3]);
    lcast_addr_t t6 = a6("::ffff:0:a00:1");
    lcast_addr_unmap(&t6);
    TEST_ASSERT_TRUE(t6.v6);
}

TEST_CASE("lcast addresses: a hotspot client that reaches the home-network address is refused while the AP is up",
          "[lcast]")
{
    /* lwIP takes an IPv4 packet for any of the station's addresses on any interface: a phone on the
     * hotspot (192.168.4.2) can connect to the station's home address (192.168.2.10). */
    const uint32_t ap4 = ip4(192, 168, 4, 1);
    const uint32_t mask = ip4(255, 255, 255, 0);
    const uint32_t gw = ip4(192, 168, 2, 1);
    lcast_addr_t sta_local = a4(192, 168, 2, 10);
    lcast_addr_t setup_phone = a4(192, 168, 4, 2);
    lcast_addr_t setup_phone_m = a4m(192, 168, 4, 200);
    lcast_addr_t lan_phone = a4(192, 168, 2, 50);
    lcast_addr_t next_subnet = a4(192, 168, 5, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&setup_phone, &sta_local, gw, ap4, mask, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&setup_phone_m, &sta_local, gw, ap4, mask, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&lan_phone, &sta_local, gw, ap4, mask, NULL));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&next_subnet, &sta_local, gw, ap4, mask, NULL));
    /* the AP down (mask 0): its subnet is nobody's, the home network may even use it */
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&setup_phone, &sta_local, gw, ap4, 0, NULL));
    /* a home network numbered like the hotspot counts as the hotspot while the AP is up (documented) */
    lcast_addr_t same_local = a4(192, 168, 4, 20);
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&setup_phone, &same_local, 0, ap4, mask, NULL));
    /* IPv6 is decided by the local address (link-local addresses only, taken on their own interface) */
    lcast_addr_t ap_ll = a6("fe80::aa:bbff:fecc:ddee");
    lcast_addr_t sta_ll = a6("fe80::1234");
    lcast_addr_t peer_ll = a6("fe80::5");
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&peer_ll, &ap_ll, gw, ap4, mask, ap_ll.b));
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_OK, lcast_addr_verdict(&peer_ll, &sta_ll, gw, ap4, mask, ap_ll.b));
    /* the order: the hotspot before the ranges and the gateway */
    lcast_addr_t ap_gw = a4(192, 168, 4, 1);
    TEST_ASSERT_EQUAL_INT(LCAST_ADDR_HOTSPOT, lcast_addr_verdict(&ap_gw, &sta_local, ip4(192, 168, 4, 1), ap4, mask,
                                                                 NULL));
}

/* ---- connection slots ------------------------------------------------------------------------ */

static lcast_slot_view_t sv(lcast_slot_kind_t kind, uint64_t key, uint32_t since, bool bytes)
{
    return (lcast_slot_view_t) { .kind = kind, .key = key, .since_ms = since, .got_bytes = bytes };
}

TEST_CASE("lcast slots: idle connections from the LAN never keep a rover out", "[lcast]")
{
    const uint64_t A = 0x0A00020A, B = 0x0A00020B, R = 0x0A000232;
    lcast_slot_view_t s[4];
    lcast_slot_pick_t p;
    /* a free slot: taken, nothing closed; a client may hold several while slots are free */
    s[0] = sv(LCAST_SLOT_REQ, A, 100, false);
    s[1] = sv(LCAST_SLOT_FREE, 0, 0, false);
    s[2] = sv(LCAST_SLOT_REQ, A, 150, true);
    s[3] = sv(LCAST_SLOT_BUSY, R, 50, false);
    p = lcast_slot_pick(s, 4, A, 1000);
    TEST_ASSERT_EQUAL_INT(1, p.slot);
    TEST_ASSERT_FALSE(p.evict);

    /* all taken by one client's idle connections: the oldest silent one gives way to the rover */
    for (int i = 0; i < 4; i++) {
        s[i] = sv(LCAST_SLOT_REQ, A, (uint32_t)(100 + 10 * i), false);
    }
    p = lcast_slot_pick(s, 4, R, 1000);
    TEST_ASSERT_EQUAL_INT(0, p.slot);
    TEST_ASSERT_TRUE(p.evict);
    /* ... also when they each sent a byte (a request in the works): one client, several slots */
    for (int i = 0; i < 4; i++) {
        s[i] = sv(LCAST_SLOT_REQ, A, (uint32_t)(100 + 10 * i), true);
    }
    s[0].since_ms = 400;
    p = lcast_slot_pick(s, 4, R, 1000);
    TEST_ASSERT_EQUAL_INT(1, p.slot);   /* the oldest */
    TEST_ASSERT_TRUE(p.evict);

    /* a lingering connection (its answer is out) goes first, before a request */
    s[0] = sv(LCAST_SLOT_REQ, A, 100, false);
    s[1] = sv(LCAST_SLOT_LINGER, B, 900, false);
    s[2] = sv(LCAST_SLOT_BUSY, R, 50, false);
    s[3] = sv(LCAST_SLOT_LINGER, A, 800, false);
    p = lcast_slot_pick(s, 4, R, 1000);
    TEST_ASSERT_EQUAL_INT(3, p.slot);

    /* the same client's earlier request is replaced by its newer one */
    s[0] = sv(LCAST_SLOT_REQ, A, 100, true);
    s[1] = sv(LCAST_SLOT_REQ, B, 50, true);
    s[2] = sv(LCAST_SLOT_BUSY, R, 50, false);
    s[3] = sv(LCAST_SLOT_BUSY, R + 1, 50, false);
    p = lcast_slot_pick(s, 4, A, 1000);
    TEST_ASSERT_EQUAL_INT(0, p.slot);
    TEST_ASSERT_TRUE(p.evict);
    /* a stranger with requests from two clients, each holding one slot: refused (no rover's slot is taken) */
    p = lcast_slot_pick(s, 4, R + 2, 1000);
    TEST_ASSERT_EQUAL_INT(-1, p.slot);
    TEST_ASSERT_FALSE(p.evict);
    /* a client that holds a rover and a request: its request may go */
    s[1].key = R;
    p = lcast_slot_pick(s, 4, R + 2, 1000);
    TEST_ASSERT_EQUAL_INT(1, p.slot);

    /* streaming rovers and answers being written are never taken */
    for (int i = 0; i < 4; i++) {
        s[i] = sv(LCAST_SLOT_BUSY, A, 0, false);
    }
    p = lcast_slot_pick(s, 4, A, 1000);
    TEST_ASSERT_EQUAL_INT(-1, p.slot);

    /* age across the clock wrap */
    s[0] = sv(LCAST_SLOT_REQ, A, UINT32_MAX - 100u, false);
    s[1] = sv(LCAST_SLOT_REQ, B, 50, false);
    p = lcast_slot_pick(s, 2, R, 100);
    TEST_ASSERT_EQUAL_INT(0, p.slot);
}

/* ---- 2. address text and client key -------------------------------------------------------- */

TEST_CASE("lcast address text (RFC 5952) and client keys", "[lcast]")
{
    static const struct {
        const char *in;
        const char *out;
    } t[] = {
        { "fe80::1", "fe80::1" },
        { "2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1" },   /* the first of equal runs */
        { "2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1" }, /* a single zero group stays */
        { "::", "::" },
        { "::1", "::1" },
        { "fd00:abcd::", "fd00:abcd::" },
        { "2001:DB8::A:0:0:0", "2001:db8:0:0:a::" },
        { "::ffff:10.0.2.2", "10.0.2.2" },
    };
    char buf[LOCAL_CASTER_ADDR_LEN];
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        lcast_addr_t a = a6(t[i].in);
        TEST_ASSERT_EQUAL_size_t(strlen(t[i].out), lcast_addr_text(&a, buf, sizeof(buf)));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(t[i].out, buf, t[i].in);
    }
    lcast_addr_t v = a4(192, 168, 2, 50);
    lcast_addr_text(&v, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("192.168.2.50", buf);
    lcast_addr_text(NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("?", buf);
    char small[6];
    lcast_addr_text(&v, small, sizeof(small));
    TEST_ASSERT_EQUAL_STRING("192.1", small);   /* cut, terminated */
    lcast_addr_t longest = a6("1111:2222:3333:4444:5555:6666:7777:8888");
    TEST_ASSERT_EQUAL_size_t(39, lcast_addr_text(&longest, buf, sizeof(buf)));

    /* keys: IPv4 as itself (also mapped), link-local folded, others by /64 */
    lcast_addr_t m = a4m(192, 168, 2, 50);
    TEST_ASSERT_EQUAL_UINT64(lcast_client_key(&v), lcast_client_key(&m));
    TEST_ASSERT_EQUAL_UINT64(ip4(192, 168, 2, 50), lcast_client_key(&v));
    lcast_addr_t g1 = a6("2001:db8:1:2::10");
    lcast_addr_t g2 = a6("2001:db8:1:2::20");
    lcast_addr_t g3 = a6("2001:db8:1:3::10");
    TEST_ASSERT_EQUAL_UINT64(lcast_client_key(&g1), lcast_client_key(&g2));
    TEST_ASSERT_NOT_EQUAL(lcast_client_key(&g1), lcast_client_key(&g3));
    lcast_addr_t l1 = a6("fe80::10");
    lcast_addr_t l2 = a6("fe80::20");
    TEST_ASSERT_NOT_EQUAL(lcast_client_key(&l1), lcast_client_key(&l2));
    TEST_ASSERT_EQUAL_UINT64(0, lcast_client_key(NULL));

    /* the journal's 4 bytes */
    uint8_t q[4];
    lcast_addr_word(&m, q);
    TEST_ASSERT_EQUAL_UINT8(192, q[0]);
    TEST_ASSERT_EQUAL_UINT8(50, q[3]);
    lcast_addr_t w6 = a6("fd00::1:aabb:ccdd");
    lcast_addr_word(&w6, q);
    TEST_ASSERT_EQUAL_UINT8(0xAA, q[0]);
    TEST_ASSERT_EQUAL_UINT8(0xDD, q[3]);
}

/* ---- 3. base64 ------------------------------------------------------------------------------ */

TEST_CASE("lcast base64: RFC 4648 vectors, optional padding, bad input, the length cap", "[lcast]")
{
    static const struct {
        const char *in;
        const char *out;
    } v[] = {
        { "", "" },           { "Zg==", "f" },        { "Zm8=", "fo" },         { "Zm9v", "foo" },
        { "Zm9vYg==", "foob" }, { "Zm9vYmE=", "fooba" }, { "Zm9vYmFy", "foobar" },
        { "Zg", "f" },        { "Zm8", "fo" },        { "Zm9vYg", "foob" },     /* padding left out */
    };
    uint8_t out[96];
    size_t n;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(lcast_b64_decode(v[i].in, strlen(v[i].in), out, sizeof(out), &n), v[i].in);
        TEST_ASSERT_EQUAL_size_t(strlen(v[i].out), n);
        if (n > 0) {
            TEST_ASSERT_EQUAL_MEMORY(v[i].out, out, n);
        }
    }
    static const char *const bad[] = {
        "Z", "Zm9vY", "Zg=", "Z===", "Zm9v====", "Zm=v", "Zm9v!A==", "Zm 9v", "Zm-v", "Zm_v", "=Zm9", "Zg==Zg==",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(lcast_b64_decode(bad[i], strlen(bad[i]), out, sizeof(out), &n), bad[i]);
    }
    /* 95 decoded bytes fit the credential buffer, 96 do not */
    const char *b95 = "YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYTpiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJ"
                      "iYmJiYmJiYmJiYmJiYmJiYmJiYmI=";
    const char *b96 = "YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYTpiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJ"
                      "iYmJiYmJiYmJiYmJiYmJiYmJiYmJi";
    TEST_ASSERT_TRUE(lcast_b64_decode(b95, strlen(b95), out, 95, &n));
    TEST_ASSERT_EQUAL_size_t(95, n);
    TEST_ASSERT_FALSE(lcast_b64_decode(b96, strlen(b96), out, 95, &n));
}

/* ---- 4. credentials -------------------------------------------------------------------------- */

TEST_CASE("lcast credentials: exact match only, in constant time over 96 bytes", "[lcast]")
{
    lcast_cred_t c;
    TEST_ASSERT_TRUE(lcast_cred_set(&c, "rover", "secretpw"));
    TEST_ASSERT_EQUAL_size_t(14, c.len);
    TEST_ASSERT_EQUAL_STRING("rover:secretpw", c.v);
    TEST_ASSERT_TRUE(lcast_cred_match(&c, "rover:secretpw", 14));
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rovex:secretpw", 14));    /* wrong user */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:secretpx", 14));    /* wrong password */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:secret", 12));      /* a prefix */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:secretpw1", 15));   /* longer */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "", 0));                   /* empty */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "Rover:secretpw", 14));    /* case */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:SECRETPW", 14));
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:secretpw\0", 15));  /* a NUL more is longer */
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "rover:secretpw", 96));    /* over the window */

    /* the longest pair fits; one byte more does not (and leaves nothing that matches) */
    char user[32];
    char pass[65];
    memset(user, 'u', 31);
    user[31] = '\0';
    memset(pass, 'p', 63);
    pass[63] = '\0';
    TEST_ASSERT_TRUE(lcast_cred_set(&c, user, pass));
    TEST_ASSERT_EQUAL_size_t(95, c.len);
    pass[63] = 'p';
    pass[64] = '\0';
    TEST_ASSERT_FALSE(lcast_cred_set(&c, user, pass));
    TEST_ASSERT_EQUAL_size_t(0, c.len);
    TEST_ASSERT_FALSE(lcast_cred_match(&c, "", 0));   /* none set: nothing matches, not even empty */

    char z[16] = "secret";
    lcast_secure_zero(z, sizeof(z));
    for (size_t i = 0; i < sizeof(z); i++) {
        TEST_ASSERT_EQUAL_CHAR(0, z[i]);
    }
}

/* ---- 5. parser ----------------------------------------------------------------------------- */

TEST_CASE("lcast parser: an NTRIP 1.0 request as RTKLIB sends it", "[lcast]")
{
    lcast_req_t *r = &s_req;
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, REQ_V1));
    TEST_ASSERT_EQUAL_STRING("ROVER", r->mount);
    TEST_ASSERT_FALSE(r->mount_long);
    TEST_ASSERT_EQUAL_UINT8(0, r->http_minor);
    TEST_ASSERT_FALSE(r->ntrip2);
    TEST_ASSERT_TRUE(r->auth_present);
    TEST_ASSERT_FALSE(r->auth_bad);
    TEST_ASSERT_EQUAL_STRING("rover:secretpw", r->auth);
    TEST_ASSERT_EQUAL_size_t(14, r->auth_len);
    TEST_ASSERT_FALSE(r->browser);
    TEST_ASSERT_EQUAL_STRING("NTRIP RTKLIB/2.4.3", r->agent);
    /* wiped: nothing of the request or the credentials stays */
    lcast_req_wipe(r);
    static const uint8_t zero[sizeof(lcast_req_t)];
    TEST_ASSERT_EQUAL_MEMORY(zero, r, sizeof(*r));
}

TEST_CASE("lcast parser: NTRIP 2.0, LF only, any split, header case, versions", "[lcast]")
{
    lcast_req_t *r = &s_req;
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, REQ_V2));
    TEST_ASSERT_EQUAL_UINT8(1, r->http_minor);
    TEST_ASSERT_TRUE(r->ntrip2);
    TEST_ASSERT_EQUAL_STRING("NTRIP onodiy-ntrip-client/1.0", r->agent);

    /* LF only */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE,
                          parse_str(r, "GET /ROVER HTTP/1.1\nNtrip-Version: Ntrip/2.0\nAuthorization: Basic "
                                       "cm92ZXI6c2VjcmV0cHc=\n\n"));
    TEST_ASSERT_TRUE(r->ntrip2);
    TEST_ASSERT_EQUAL_STRING("rover:secretpw", r->auth);

    /* byte by byte: NEED_MORE until the very last byte */
    const char *req = REQ_V2;
    size_t n = strlen(req);
    lcast_req_reset(r);
    for (size_t i = 0; i < n; i++) {
        lcast_req_result_t res = lcast_req_feed(r, (const uint8_t *)req + i, 1);
        TEST_ASSERT_EQUAL_INT(i + 1 == n ? LCAST_REQ_DONE : LCAST_REQ_NEED_MORE, res);
    }
    TEST_ASSERT_EQUAL_STRING("rover:secretpw", r->auth);
    /* random splits, the GGA a client sends at once after it included */
    char with_gga[600];
    snprintf(with_gga, sizeof(with_gga), "%s$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n",
             REQ_V1);
    for (int k = 0; k < 200; k++) {
        lcast_req_reset(r);
        size_t len = strlen(with_gga);
        size_t at = 0;
        lcast_req_result_t res = LCAST_REQ_NEED_MORE;
        while (at < len) {
            size_t step = 1u + rnd() % 40u;
            if (step > len - at) {
                step = len - at;
            }
            res = lcast_req_feed(r, (const uint8_t *)with_gga + at, step);
            at += step;
        }
        TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, res);
        TEST_ASSERT_EQUAL_STRING("ROVER", r->mount);
        TEST_ASSERT_EQUAL_STRING("rover:secretpw", r->auth);
        TEST_ASSERT_EQUAL_STRING("NTRIP RTKLIB/2.4.3", r->agent);
    }

    /* header names in any case, values trimmed */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE,
                          parse_str(r, "GET /ROVER HTTP/1.1\r\nNTRIP-VERSION:   ntrip/2.0  \r\nuser-agent:x\r\n"
                                       "AUTHORIZATION: bAsIc   cm92ZXI6c2VjcmV0cHc  \r\n\r\n"));
    TEST_ASSERT_TRUE(r->ntrip2);
    TEST_ASSERT_EQUAL_STRING("x", r->agent);
    TEST_ASSERT_FALSE(r->auth_bad);
    TEST_ASSERT_EQUAL_STRING("rover:secretpw", r->auth);
    /* another Ntrip-Version value is 1.0 */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.1\r\nNtrip-Version: Ntrip/1.0\r\n\r\n"));
    TEST_ASSERT_FALSE(r->ntrip2);

    /* no version: HTTP/1.0, NTRIP 1.0 */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER\r\n\r\n"));
    TEST_ASSERT_EQUAL_UINT8(0, r->http_minor);
    TEST_ASSERT_FALSE(r->auth_present);
    TEST_ASSERT_EQUAL_STRING("", r->agent);
    /* more spaces between the parts */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET   /ROVER    HTTP/1.1 \r\n\r\n"));
    TEST_ASSERT_EQUAL_UINT8(1, r->http_minor);
    /* HTTP/1.0 with Ntrip-Version 2.0 */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.0\r\nNtrip-Version: Ntrip/2.0\r\n\r\n"));
    TEST_ASSERT_TRUE(r->ntrip2);
    TEST_ASSERT_EQUAL_UINT8(0, r->http_minor);
}

TEST_CASE("lcast parser: targets, refused methods and versions, limits, NUL, Authorization, browsers, agents",
          "[lcast]")
{
    lcast_req_t *r = &s_req;
    /* absolute form, query dropped, no decoding */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET http://onodiy.local:2101/ROVER?x=1 HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("ROVER", r->mount);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET HTTP://host/ HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("", r->mount);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET http://host HTTP/1.1\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("", r->mount);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /RO%56ER?pw=secret HTTP/1.0\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("RO%56ER", r->mount);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET / HTTP/1.0\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("", r->mount);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER/ HTTP/1.0\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("ROVER/", r->mount);
    /* a mount longer than the buffer is kept cut and marked */
    char longreq[200];
    char m70[71];
    memset(m70, 'M', 70);
    m70[70] = '\0';
    snprintf(longreq, sizeof(longreq), "GET /%s HTTP/1.0\r\n\r\n", m70);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, longreq));
    TEST_ASSERT_TRUE(r->mount_long);
    TEST_ASSERT_EQUAL_size_t(63, strlen(r->mount));

    static const char *const bad[] = {
        "SOURCE secret /ROVER\r\n\r\n",
        "POST /ROVER HTTP/1.1\r\n\r\n",
        "HEAD /ROVER HTTP/1.1\r\n\r\n",
        "get /ROVER HTTP/1.1\r\n\r\n",
        "GET /ROVER RTSP/1.0\r\n\r\n",
        "GET /ROVER HTTP/2.0\r\n\r\n",
        "GET /ROVER http/1.1\r\n\r\n",
        "GET ROVER HTTP/1.1\r\n\r\n",
        "GET * HTTP/1.1\r\n\r\n",
        "GET\r\n\r\n",
        "GET /ROVER HTTP/1.1 extra\r\n\r\n",
        "GET /RO\tVER HTTP/1.1\r\n\r\n",
        "\r\nGET /ROVER HTTP/1.1\r\n\r\n",
        "GET /RO\x80VER HTTP/1.1\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(LCAST_REQ_BAD, parse_str(r, bad[i]), bad[i]);
        TEST_ASSERT_EQUAL_STRING("", r->auth);
    }

    /* 1024 bytes and more without the end of the header */
    uint8_t big[1100];
    memcpy(big, "GET /ROVER HTTP/1.1\r\nX-Pad: ", 28);
    memset(big + 28, 'a', sizeof(big) - 28);
    lcast_req_reset(r);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_NEED_MORE, lcast_req_feed(r, big, 1023));
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_TOO_LARGE, lcast_req_feed(r, big + 1023, 2));
    lcast_req_reset(r);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_TOO_LARGE, lcast_req_feed(r, big, 1025));
    size_t room;
    lcast_req_room(r, &room);
    TEST_ASSERT_EQUAL_size_t(0, room);
    /* exactly 1024 bytes ending the header is fine */
    lcast_req_reset(r);
    memcpy(big + 1020, "\r\n\r\n", 4);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, lcast_req_feed(r, big, 1024));

    /* a NUL anywhere in the header */
    static const char nul_req[] = "GET /ROVER HTTP/1.1\r\nUser-Agent: a\0b\r\n\r\n";
    lcast_req_reset(r);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_BAD, lcast_req_feed(r, (const uint8_t *)nul_req, sizeof(nul_req) - 1));
    /* ... but not in what follows it */
    static const char nul_after[] = "GET /ROVER HTTP/1.1\r\n\r\n\0\0\0";
    lcast_req_reset(r);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, lcast_req_feed(r, (const uint8_t *)nul_after, sizeof(nul_after) - 1));

    /* Authorization that is not usable Basic: present and bad */
    static const char *const bad_auth[] = {
        "Digest username=\"rover\"", "Bearer abc", "Basic", "Basic ", "Basic !!!!", "Basic bm9jb2xvbg==",
        "Basicx cm92ZXI6c2VjcmV0cHc=",
        "Basic YWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYWFhYTpiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYm"
        "JiYmJiYmJiYmJiYmJiYmJiYmJiYmJiYmJi",
        "Basic cm92ZXI6c2Vj cmV0cHc=",
    };
    for (size_t i = 0; i < sizeof(bad_auth) / sizeof(bad_auth[0]); i++) {
        char q[300];
        snprintf(q, sizeof(q), "GET /ROVER HTTP/1.0\r\nAuthorization: %s\r\n\r\n", bad_auth[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(LCAST_REQ_DONE, parse_str(r, q), bad_auth[i]);
        TEST_ASSERT_TRUE_MESSAGE(r->auth_present, bad_auth[i]);
        TEST_ASSERT_TRUE_MESSAGE(r->auth_bad, bad_auth[i]);
        TEST_ASSERT_EQUAL_size_t(0, r->auth_len);
        TEST_ASSERT_EQUAL_STRING("", r->auth);
    }
    /* a password with colons splits at the first one (the whole string is compared) */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.0\r\nAuthorization: Basic "
                                                       "cm92ZXI6c2VjcmV0cHc6d2l0aDpjb2xvbnM=\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("rover:secretpw:with:colons", r->auth);
    /* two Authorization headers */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_BAD, parse_str(r, "GET /ROVER HTTP/1.0\r\nAuthorization: Basic "
                                                      "cm92ZXI6c2VjcmV0cHc=\r\nauthorization: Basic Zm9v\r\n\r\n"));
    TEST_ASSERT_EQUAL_STRING("", r->auth);

    /* browsers */
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.1\r\nOrigin: http://x\r\n\r\n"));
    TEST_ASSERT_TRUE(r->browser);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.1\r\nsec-fetch-site: cross-site\r\n\r\n"));
    TEST_ASSERT_TRUE(r->browser);
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, "GET /ROVER HTTP/1.1\r\nSec-Fetch: no\r\n\r\n"));
    TEST_ASSERT_FALSE(r->browser);

    /* agent: printable ASCII only, cut to 47, the first one kept */
    char q[400];
    snprintf(q, sizeof(q), "GET /ROVER HTTP/1.0\r\nUser-Agent: NTRIP\x01 caf\xc3\xa9 %s\r\nUser-Agent: second\r\n\r\n",
             "0123456789012345678901234567890123456789012345678901234567890123456789");
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, q));
    TEST_ASSERT_EQUAL_size_t(LOCAL_CASTER_AGENT_LEN - 1u, strlen(r->agent));
    TEST_ASSERT_EQUAL_STRING_LEN("NTRIP caf 0123", r->agent, 14);
    /* lines without ':' are ignored, header lines past 32 too */
    char many[1024];
    size_t k = (size_t)snprintf(many, sizeof(many), "GET /ROVER HTTP/1.0\r\nno colon here\r\n");
    for (int i = 0; i < 31; i++) {
        k += (size_t)snprintf(many + k, sizeof(many) - k, "X-%d: y\r\n", i);
    }
    snprintf(many + k, sizeof(many) - k, "Origin: late\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(LCAST_REQ_DONE, parse_str(r, many));
    TEST_ASSERT_FALSE(r->browser);   /* the 33rd header line */
}

/* ---- 6. decisions -------------------------------------------------------------------------- */

TEST_CASE("lcast decisions in their documented order", "[lcast]")
{
    lcast_req_t *r = &s_req;
    lcast_cred_t cred;
    lcast_cred_set(&cred, "rover", "secretpw");
    lcast_decision_t d;

    /* bad, too long, late -> 400 (1.0 form) */
    d = lcast_decide(r, LCAST_REQ_BAD, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E400, d.action);
    TEST_ASSERT_EQUAL_INT(LCAST_FLAV_V1, d.flavour);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E400, lcast_decide(r, LCAST_REQ_TOO_LARGE, "ROVER", &cred, 0, 0, 2).action);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E400, lcast_decide(r, LCAST_REQ_NEED_MORE, "ROVER", &cred, 0, 0, 2).action);
    /* a browser -> 403, before anything else */
    parse_str(r, "GET /ROVER HTTP/1.1\r\nNtrip-Version: Ntrip/2.0\r\nOrigin: http://x\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 300, 2, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E403, d.action);
    TEST_ASSERT_EQUAL_INT(LCAST_FLAV_V2_CHUNKED, d.flavour);
    /* GET /, another mount, a trailing slash, an over-long one -> the table, even throttled and full */
    static const char *const table[] = {
        "GET / HTTP/1.0\r\n\r\n", "GET /NOPE HTTP/1.0\r\n\r\n", "GET /ROVER/ HTTP/1.0\r\n\r\n",
        "GET /ROVERX HTTP/1.0\r\n\r\n", "GET /ROVE HTTP/1.0\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        parse_str(r, table[i]);
        d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 300, 2, 2);
        TEST_ASSERT_EQUAL_INT_MESSAGE(LCAST_ACT_SOURCETABLE, d.action, table[i]);
        TEST_ASSERT_FALSE(d.count_fail);
    }
    /* the mount in another case streams */
    parse_str(r, "GET /rOvEr HTTP/1.0\r\nAuthorization: Basic cm92ZXI6c2VjcmV0cHc=\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_STREAM, d.action);
    TEST_ASSERT_EQUAL_INT(LCAST_FLAV_V1, d.flavour);
    TEST_ASSERT_TRUE(d.login_ok);
    /* no credentials -> 401, not counted */
    parse_str(r, "GET /ROVER HTTP/1.1\r\nNtrip-Version: Ntrip/2.0\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E401, d.action);
    TEST_ASSERT_FALSE(d.count_fail);
    /* wrong -> 401, counted; malformed too */
    parse_str(r, "GET /ROVER HTTP/1.0\r\nAuthorization: Basic bm9jb2xvbjp4\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E401, d.action);
    TEST_ASSERT_TRUE(d.count_fail);
    TEST_ASSERT_FALSE(d.login_ok);
    parse_str(r, "GET /ROVER HTTP/1.0\r\nAuthorization: Digest x\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E401, d.action);
    TEST_ASSERT_TRUE(d.count_fail);
    /* throttled with the right credentials -> 429, not checked */
    parse_str(r, REQ_V1);
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 16, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E429, d.action);
    TEST_ASSERT_FALSE(d.login_ok);
    TEST_ASSERT_FALSE(d.count_fail);
    /* full -> 503 only after a good login (a wrong one stays 401: no occupancy leak) */
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 2, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E503, d.action);
    TEST_ASSERT_TRUE(d.login_ok);
    parse_str(r, "GET /ROVER HTTP/1.0\r\nAuthorization: Basic bm9jb2xvbjp4\r\n\r\n");
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E401, lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 2, 2).action);
    /* flavours */
    parse_str(r, REQ_V2);
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 1, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_STREAM, d.action);
    TEST_ASSERT_EQUAL_INT(LCAST_FLAV_V2_CHUNKED, d.flavour);
    parse_str(r, "GET /ROVER HTTP/1.0\r\nNtrip-Version: Ntrip/2.0\r\nAuthorization: Basic cm92ZXI6c2VjcmV0cHc=\r\n\r\n");
    d = lcast_decide(r, LCAST_REQ_DONE, "ROVER", &cred, 0, 0, 2);
    TEST_ASSERT_EQUAL_INT(LCAST_FLAV_V2_RAW, d.flavour);
    /* no credential set (cannot happen while on): nobody logs in */
    lcast_cred_t none;
    memset(&none, 0, sizeof(none));
    parse_str(r, REQ_V1);
    TEST_ASSERT_EQUAL_INT(LCAST_ACT_E401, lcast_decide(r, LCAST_REQ_DONE, "ROVER", &none, 0, 0, 2).action);
}

/* ---- 7. replies ------------------------------------------------------------------------------ */

#define DATE_UNIX 1792384200   /* Mon, 19 Oct 2026 04:30:00 GMT */

TEST_CASE("lcast replies: stream success, every flavour, with and without Date", "[lcast]")
{
    char b[1024];
    TEST_ASSERT_EQUAL_size_t(14, lcast_reply_stream(b, sizeof(b), LCAST_FLAV_V1, "1.2.3", DATE_UNIX));
    TEST_ASSERT_EQUAL_STRING("ICY 200 OK\r\n\r\n", b);
    const char *v2 = "HTTP/1.1 200 OK\r\n"
                     "Ntrip-Version: Ntrip/2.0\r\n"
                     "Server: NTRIP onodiy/1.2.3\r\n"
                     "Cache-Control: no-store, no-cache, max-age=0\r\n"
                     "Pragma: no-cache\r\n"
                     "Connection: close\r\n"
                     "Content-Type: gnss/data\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n";
    TEST_ASSERT_EQUAL_size_t(strlen(v2), lcast_reply_stream(b, sizeof(b), LCAST_FLAV_V2_CHUNKED, "1.2.3", 0));
    TEST_ASSERT_EQUAL_STRING(v2, b);
    /* an untrusted clock sends no Date */
    lcast_reply_stream(b, sizeof(b), LCAST_FLAV_V2_CHUNKED, "1.2.3", LCAST_CLOCK_VALID_UNIX - 1);
    TEST_ASSERT_EQUAL_STRING(v2, b);
    const char *v2d = "HTTP/1.1 200 OK\r\n"
                      "Ntrip-Version: Ntrip/2.0\r\n"
                      "Server: NTRIP onodiy/1.2.3\r\n"
                      "Date: Mon, 19 Oct 2026 04:30:00 GMT\r\n"
                      "Cache-Control: no-store, no-cache, max-age=0\r\n"
                      "Pragma: no-cache\r\n"
                      "Connection: close\r\n"
                      "Content-Type: gnss/data\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n";
    lcast_reply_stream(b, sizeof(b), LCAST_FLAV_V2_CHUNKED, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING(v2d, b);
    const char *raw = "HTTP/1.0 200 OK\r\n"
                      "Ntrip-Version: Ntrip/2.0\r\n"
                      "Server: NTRIP onodiy/1.2.3\r\n"
                      "Date: Mon, 19 Oct 2026 04:30:00 GMT\r\n"
                      "Cache-Control: no-store, no-cache, max-age=0\r\n"
                      "Pragma: no-cache\r\n"
                      "Connection: close\r\n"
                      "Content-Type: gnss/data\r\n"
                      "\r\n";
    lcast_reply_stream(b, sizeof(b), LCAST_FLAV_V2_RAW, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING(raw, b);
    /* too small: 0 */
    TEST_ASSERT_EQUAL_size_t(0, lcast_reply_stream(b, 14, LCAST_FLAV_V1, "1.2.3", 0));
    TEST_ASSERT_EQUAL_size_t(0, lcast_reply_stream(b, strlen(v2), LCAST_FLAV_V2_CHUNKED, "1.2.3", 0));
    TEST_ASSERT_EQUAL_size_t(strlen(v2), lcast_reply_stream(b, strlen(v2) + 1, LCAST_FLAV_V2_CHUNKED, "1.2.3", 0));

    /* IMF dates */
    char d[40];
    TEST_ASSERT_EQUAL_size_t(29, lcast_imf_date(LCAST_CLOCK_VALID_UNIX - 1, d, sizeof(d)));
    TEST_ASSERT_EQUAL_STRING("Sun, 31 Dec 2023 23:59:59 GMT", d);
    lcast_imf_date(951782400, d, sizeof(d));
    TEST_ASSERT_EQUAL_STRING("Tue, 29 Feb 2000 00:00:00 GMT", d);
    lcast_imf_date(4102444799LL, d, sizeof(d));
    TEST_ASSERT_EQUAL_STRING("Thu, 31 Dec 2099 23:59:59 GMT", d);
    TEST_ASSERT_EQUAL_size_t(0, lcast_imf_date(DATE_UNIX, d, 29));
}

/* Content-Length equals the body after the blank line. */
static void check_length(const char *reply)
{
    const char *cl = strstr(reply, "Content-Length: ");
    TEST_ASSERT_NOT_NULL(cl);
    unsigned long n = strtoul(cl + 16, NULL, 10);
    const char *body = strstr(reply, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_size_t(n, strlen(body + 4));
}

TEST_CASE("lcast replies: errors and the source table, byte for byte", "[lcast]")
{
    char b[1024];
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E401, LCAST_FLAV_V1, "ROVER", 0, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.0 401 Unauthorized\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "WWW-Authenticate: Basic realm=\"/ROVER\"\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 39\r\n"
                             "\r\n"
                             "Wrong or missing username or password\r\n",
                             b);
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E401, LCAST_FLAV_V2_CHUNKED, "ROVER", 0, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 401 Unauthorized\r\n"
                             "Ntrip-Version: Ntrip/2.0\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Date: Mon, 19 Oct 2026 04:30:00 GMT\r\n"
                             "WWW-Authenticate: Basic realm=\"/ROVER\"\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 39\r\n"
                             "\r\n"
                             "Wrong or missing username or password\r\n",
                             b);
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E429, LCAST_FLAV_V2_RAW, "ROVER", 16, "1.2.3", 0);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.0 429 Too Many Requests\r\n"
                             "Ntrip-Version: Ntrip/2.0\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Retry-After: 16\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 42\r\n"
                             "\r\n"
                             "Too many wrong logins, try again in 16 s\r\n",
                             b);
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E400, LCAST_FLAV_V1, "ROVER", 0, "1.2.3", 0);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.0 400 Bad Request\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 13\r\n"
                             "\r\n"
                             "Bad request\r\n",
                             b);
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E403, LCAST_FLAV_V2_CHUNKED, "ROVER", 0, "1.2.3", 0);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 403 Forbidden\r\n"
                             "Ntrip-Version: Ntrip/2.0\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 22\r\n"
                             "\r\n"
                             "Not for web browsers\r\n",
                             b);
    lcast_reply_error(b, sizeof(b), LCAST_ACT_E503, LCAST_FLAV_V1, "ROVER", 0, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.0 503 Service Unavailable\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Connection: close\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 28\r\n"
                             "\r\n"
                             "At most 4 rovers at a time\r\n",
                             b);
    /* every code and flavour: a body as long as it says, nothing secret */
    static const lcast_action_t codes[] = { LCAST_ACT_E400, LCAST_ACT_E401, LCAST_ACT_E403, LCAST_ACT_E429,
                                            LCAST_ACT_E503 };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        for (int f = LCAST_FLAV_V1; f <= LCAST_FLAV_V2_RAW; f++) {
            for (int dated = 0; dated < 2; dated++) {
                size_t n = lcast_reply_error(b, sizeof(b), codes[i], (lcast_flavour_t)f, "ROVER", 300, "1.2.3",
                                             dated ? DATE_UNIX : 0);
                TEST_ASSERT_TRUE(n > 0);
                TEST_ASSERT_EQUAL_size_t(n, strlen(b));
                check_length(b);
                TEST_ASSERT_NULL(strstr(b, "secretpw"));
                TEST_ASSERT_NULL(strstr(b, "rover:"));
                TEST_ASSERT_EQUAL(f != LCAST_FLAV_V1 && dated, strstr(b, "Date: ") != NULL);
                TEST_ASSERT_EQUAL(f != LCAST_FLAV_V1, strstr(b, "Ntrip-Version: Ntrip/2.0\r\n") != NULL);
                TEST_ASSERT_EQUAL_size_t(0, lcast_reply_error(b, n, codes[i], (lcast_flavour_t)f, "ROVER", 300,
                                                              "1.2.3", dated ? DATE_UNIX : 0));
            }
        }
    }
    TEST_ASSERT_EQUAL_size_t(0, lcast_reply_error(b, sizeof(b), LCAST_ACT_STREAM, LCAST_FLAV_V1, "ROVER", 0, "1", 0));

    const char *str = "STR;ROVER;onodiy;RTCM 3.3;1005(1);2;GPS;onodiy;;0.00;0.00;0;0;UM982;none;B;N;0;";
    size_t n = lcast_reply_sourcetable(b, sizeof(b), LCAST_FLAV_V1, str, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING("SOURCETABLE 200 OK\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 97\r\n"
                             "\r\n"
                             "STR;ROVER;onodiy;RTCM 3.3;1005(1);2;GPS;onodiy;;0.00;0.00;0;0;UM982;none;B;N;0;\r\n"
                             "ENDSOURCETABLE\r\n",
                             b);
    TEST_ASSERT_EQUAL_size_t(strlen(b), n);
    check_length(b);
    lcast_reply_sourcetable(b, sizeof(b), LCAST_FLAV_V2_CHUNKED, str, "1.2.3", DATE_UNIX);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.1 200 OK\r\n"
                             "Ntrip-Version: Ntrip/2.0\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Date: Mon, 19 Oct 2026 04:30:00 GMT\r\n"
                             "Connection: close\r\n"
                             "Content-Type: gnss/sourcetable\r\n"
                             "Content-Length: 97\r\n"
                             "\r\n"
                             "STR;ROVER;onodiy;RTCM 3.3;1005(1);2;GPS;onodiy;;0.00;0.00;0;0;UM982;none;B;N;0;\r\n"
                             "ENDSOURCETABLE\r\n",
                             b);
    lcast_reply_sourcetable(b, sizeof(b), LCAST_FLAV_V2_RAW, str, "1.2.3", 0);
    TEST_ASSERT_EQUAL_STRING("HTTP/1.0 200 OK\r\n"
                             "Ntrip-Version: Ntrip/2.0\r\n"
                             "Server: NTRIP onodiy/1.2.3\r\n"
                             "Connection: close\r\n"
                             "Content-Type: gnss/sourcetable\r\n"
                             "Content-Length: 97\r\n"
                             "\r\n"
                             "STR;ROVER;onodiy;RTCM 3.3;1005(1);2;GPS;onodiy;;0.00;0.00;0;0;UM982;none;B;N;0;\r\n"
                             "ENDSOURCETABLE\r\n",
                             b);
    check_length(b);
    TEST_ASSERT_EQUAL_size_t(0, lcast_reply_sourcetable(b, 100, LCAST_FLAV_V1, str, "1.2.3", 0));
}

/* ---- 8. STR record ----------------------------------------------------------------------------- */

static size_t count_char(const char *s, char c)
{
    size_t n = 0;
    for (; *s != '\0'; s++) {
        n += *s == c;
    }
    return n;
}

TEST_CASE("lcast STR: the documented example, 19 fields, sanitised", "[lcast]")
{
    lcast_str_in_t in;
    memset(&in, 0, sizeof(in));
    static const uint16_t msgs[] = { 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137, 1230 };
    for (size_t i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        in.msgs[i] = (lcast_str_msg_t) { msgs[i], 1 };
    }
    in.n = sizeof(msgs) / sizeof(msgs[0]);
    in.mount = "onodiy";
    in.identifier = "onodiy";
    in.generator = "UM982";
    in.carrier = 2;
    in.bitrate = 13600;
    char s[LCAST_STR_MAX];
    size_t n = lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("STR;onodiy;onodiy;RTCM 3.3;1005(1),1033(1),1077(1),1087(1),1097(1),1117(1),1127(1),"
                             "1137(1),1230(1);2;GPS+GLO+GAL+BDS+QZS+IRN;onodiy;;0.00;0.00;0;0;UM982;none;B;N;13600;",
                             s);
    TEST_ASSERT_EQUAL_size_t(strlen(s), n);
    TEST_ASSERT_EQUAL_size_t(18, count_char(s, ';'));
    TEST_ASSERT_NOT_NULL(strstr(s, ";0.00;0.00;"));

    /* ';' and control bytes in any field; no generator */
    in.mount = "a;b";
    in.identifier = "host\x01name";
    in.generator = "UM;982\n";
    in.n = 1;
    in.msgs[0] = (lcast_str_msg_t) { 1074, 5 };
    in.carrier = 7;
    in.bitrate = 0;
    lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("STR;a_b;hostname;RTCM 3.3;1074(5);2;GPS;onodiy;;0.00;0.00;0;0;UM_982;none;B;N;0;", s);
    in.generator = "";
    in.n = 0;
    in.carrier = 0;
    lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("STR;a_b;hostname;RTCM 3.3;;0;;onodiy;;0.00;0.00;0;0;onodiy;none;B;N;0;", s);
    TEST_ASSERT_EQUAL_size_t(18, count_char(s, ';'));
    in.generator = NULL;
    lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, ";onodiy;none;"));
    TEST_ASSERT_EQUAL_size_t(0, lcast_str_build(&in, s, 20));

    /* the table body ends with ENDSOURCETABLE */
    char b[1024];
    lcast_reply_sourcetable(b, sizeof(b), LCAST_FLAV_V1, s, "1", 0);
    size_t bl = strlen(b);
    TEST_ASSERT_EQUAL_STRING("\r\nENDSOURCETABLE\r\n", b + bl - 18);
}

TEST_CASE("lcast STR: messages from the observations, periods, the profile, limits, nav and carrier", "[lcast]")
{
    lcast_obs_msg_t o[40];
    lcast_str_msg_t m[LCAST_STR_MSGS_MAX];
    memset(o, 0, sizeof(o));
    /* seen in the last 60 s, unsorted, with 1101-1107 and an old one */
    o[0] = (lcast_obs_msg_t) { 1230, 5, 10, 4999, 100 };
    o[1] = (lcast_obs_msg_t) { 1005, 10, 10, 999, 100 };          /* 999 -> 1 */
    o[2] = (lcast_obs_msg_t) { 1077, 1, 10, 1500, 100 };          /* 1500 -> 2 */
    o[3] = (lcast_obs_msg_t) { 1104, 1, 10, 1000, 100 };          /* SBAS MSM numbers: never */
    o[4] = (lcast_obs_msg_t) { 1033, 10, 1, UINT32_MAX, 100 };     /* once: the profile's 10 */
    o[5] = (lcast_obs_msg_t) { 1019, 0, 1, UINT32_MAX, 100 };      /* once, no profile: 1 */
    o[6] = (lcast_obs_msg_t) { 1087, 1, 10, 1000, 60001 };        /* too old */
    o[7] = (lcast_obs_msg_t) { 1097, 1, 0, UINT32_MAX, UINT32_MAX }; /* profile only, never seen */
    o[8] = (lcast_obs_msg_t) { 1042, 0, 3, 400, 60000 };          /* 400 -> at least 1; 60 s is in */
    o[9] = (lcast_obs_msg_t) { 1117, 1, 8, 2000, 100 };           /* 8 sightings: still the profile's 1 */
    o[10] = (lcast_obs_msg_t) { 1127, 1, 9, 2000, 100 };          /* 9: the measured 2 */
    size_t n = lcast_str_msgs_from_obs(o, 11, m, LCAST_STR_MSGS_MAX);
    TEST_ASSERT_EQUAL_size_t(8, n);
    static const lcast_str_msg_t want[] = { { 1005, 1 }, { 1019, 1 }, { 1033, 10 }, { 1042, 1 }, { 1077, 2 },
                                            { 1117, 1 }, { 1127, 2 }, { 1230, 5 } };
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(want[i].msg, m[i].msg);
        TEST_ASSERT_EQUAL_UINT16(want[i].period_s, m[i].period_s);
    }
    /* nothing seen: the profile's entries and periods */
    memset(o, 0, sizeof(o));
    o[0] = (lcast_obs_msg_t) { 1097, 1, 0, UINT32_MAX, UINT32_MAX };
    o[1] = (lcast_obs_msg_t) { 1005, 10, 0, UINT32_MAX, UINT32_MAX };
    o[2] = (lcast_obs_msg_t) { 1107, 1, 0, UINT32_MAX, UINT32_MAX };
    o[3] = (lcast_obs_msg_t) { 1013, 0, 0, UINT32_MAX, UINT32_MAX };
    o[4] = (lcast_obs_msg_t) { 1077, 1, 5, 1000, 70000 };          /* seen long ago: not "seen" */
    n = lcast_str_msgs_from_obs(o, 5, m, LCAST_STR_MSGS_MAX);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT16(1005, m[0].msg);
    TEST_ASSERT_EQUAL_UINT16(10, m[0].period_s);
    TEST_ASSERT_EQUAL_UINT16(1077, m[1].msg);
    TEST_ASSERT_EQUAL_UINT16(1097, m[2].msg);
    TEST_ASSERT_EQUAL_size_t(0, lcast_str_msgs_from_obs(o, 0, m, LCAST_STR_MSGS_MAX));

    /* at most 24 entries */
    memset(o, 0, sizeof(o));
    for (size_t i = 0; i < 40; i++) {
        o[i] = (lcast_obs_msg_t) { (uint16_t)(1001 + i), 1, LCAST_STR_PERIOD_SEEN, 1000, 0 };
    }
    n = lcast_str_msgs_from_obs(o, 40, m, LCAST_STR_MSGS_MAX);
    TEST_ASSERT_EQUAL_size_t(24, n);
    TEST_ASSERT_EQUAL_UINT16(1024, m[23].msg);
    /* and at most 255 characters: long periods make each entry 12 characters */
    for (size_t i = 0; i < 40; i++) {
        o[i].period_ms = 65535000u;
    }
    n = lcast_str_msgs_from_obs(o, 40, m, LCAST_STR_MSGS_MAX);
    lcast_str_in_t in;
    memset(&in, 0, sizeof(in));
    memcpy(in.msgs, m, n * sizeof(m[0]));
    in.n = n;
    in.mount = "M";
    in.identifier = "h";
    char s[LCAST_STR_MAX];
    lcast_str_build(&in, s, sizeof(s));
    const char *details = strstr(s, "RTCM 3.3;") + 9;
    size_t dlen = (size_t)(strchr(details, ';') - details);
    TEST_ASSERT_TRUE(dlen <= LCAST_STR_DETAILS_MAX);
    TEST_ASSERT_EQUAL_size_t(21, n);   /* 21 x 12 - 1 = 251; one more would be 263 */
    TEST_ASSERT_EQUAL_size_t(251, dlen);

    /* nav: the fixed order, only MSM numbers */
    memset(&in, 0, sizeof(in));
    static const uint16_t nav_msgs[] = { 1005, 1137, 1117, 1127, 1097, 1087, 1074, 1071 };
    for (size_t i = 0; i < sizeof(nav_msgs) / sizeof(nav_msgs[0]); i++) {
        in.msgs[i] = (lcast_str_msg_t) { nav_msgs[i], 1 };
    }
    in.n = sizeof(nav_msgs) / sizeof(nav_msgs[0]);
    in.mount = "M";
    in.identifier = "h";
    lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, ";GPS+GLO+GAL+BDS+QZS+IRN;"));
    in.msgs[0] = (lcast_str_msg_t) { 1070, 1 };   /* not an MSM */
    in.msgs[1] = (lcast_str_msg_t) { 1078, 1 };
    in.n = 2;
    lcast_str_build(&in, s, sizeof(s));
    TEST_ASSERT_NOT_NULL(strstr(s, ";0;;onodiy;"));

    /* carrier */
    lcast_str_msg_t msm[] = { { 1077, 1 } };
    lcast_str_msg_t plain[] = { { 1005, 1 } };
    TEST_ASSERT_EQUAL_UINT8(2, lcast_str_carrier(4, plain, 1));
    TEST_ASSERT_EQUAL_UINT8(2, lcast_str_carrier(2, NULL, 0));
    TEST_ASSERT_EQUAL_UINT8(1, lcast_str_carrier(1, msm, 1));
    TEST_ASSERT_EQUAL_UINT8(2, lcast_str_carrier(0, msm, 1));
    TEST_ASSERT_EQUAL_UINT8(0, lcast_str_carrier(0, plain, 1));
    TEST_ASSERT_EQUAL_UINT8(0, lcast_str_carrier(0, NULL, 0));

    /* sanitising */
    char t[8];
    TEST_ASSERT_EQUAL_size_t(7, lcast_sanitize("a;b\x7f\tcdefghij", t, sizeof(t), true));
    TEST_ASSERT_EQUAL_STRING("a_bcdef", t);
    lcast_sanitize("a;b", t, sizeof(t), false);
    TEST_ASSERT_EQUAL_STRING("a;b", t);
    lcast_sanitize(NULL, t, sizeof(t), true);
    TEST_ASSERT_EQUAL_STRING("", t);
}

/* ---- 9. chunks ----------------------------------------------------------------------------- */

TEST_CASE("lcast chunk framing: size lines in lowercase hex, payload untouched", "[lcast]")
{
    static uint8_t buf[LCAST_CHUNK_HEAD + LCAST_CHUNK_DATA + LCAST_CHUNK_TAIL];
    static const struct {
        size_t len;
        const char *head;
    } t[] = { { 1, "1" }, { 15, "f" }, { 16, "10" }, { 255, "ff" }, { 256, "100" }, { 4095, "fff" },
              { 4096, "1000" } };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        memset(buf, 0xEE, sizeof(buf));
        for (size_t k = 0; k < t[i].len; k++) {
            buf[LCAST_CHUNK_HEAD + k] = (uint8_t)(k * 7u);
        }
        size_t start = 99;
        size_t n = lcast_chunk_wrap(buf, t[i].len, &start);
        size_t hl = strlen(t[i].head) + 2u;
        TEST_ASSERT_EQUAL_size_t(LCAST_CHUNK_HEAD - hl, start);
        TEST_ASSERT_EQUAL_size_t(hl + t[i].len + 2u, n);
        TEST_ASSERT_EQUAL_MEMORY(t[i].head, buf + start, hl - 2u);
        TEST_ASSERT_EQUAL_MEMORY("\r\n", buf + start + hl - 2u, 2);
        for (size_t k = 0; k < t[i].len; k++) {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(k * 7u), buf[LCAST_CHUNK_HEAD + k]);
        }
        TEST_ASSERT_EQUAL_MEMORY("\r\n", buf + LCAST_CHUNK_HEAD + t[i].len, 2);
        for (size_t k = 0; k < start; k++) {
            TEST_ASSERT_EQUAL_UINT8(0xEE, buf[k]);   /* nothing before the size line is touched */
        }
        if (LCAST_CHUNK_HEAD + t[i].len + 2u < sizeof(buf)) {
            TEST_ASSERT_EQUAL_UINT8(0xEE, buf[LCAST_CHUNK_HEAD + t[i].len + 2u]);
        }
    }
    size_t start;
    TEST_ASSERT_EQUAL_size_t(0, lcast_chunk_wrap(buf, 0, &start));      /* never an empty chunk */
    TEST_ASSERT_EQUAL_size_t(0, lcast_chunk_wrap(buf, 4097, &start));
    TEST_ASSERT_EQUAL_STRING("0\r\n\r\n", LCAST_CHUNK_END);
}

/* ---- 10. throttle -------------------------------------------------------------------------- */

TEST_CASE("lcast throttle: 5 free, then 2, 4, 8 ... 300 s; per address; LRU; global bucket; ok resets", "[lcast]")
{
    static const uint32_t delays[] = { 0, 0, 0, 0, 0, 2, 4, 8, 16, 32, 64, 128, 256, 300, 300 };
    for (uint32_t n = 0; n < sizeof(delays) / sizeof(delays[0]); n++) {
        TEST_ASSERT_EQUAL_UINT32(delays[n], lcast_throttle_delay_s(n));
    }
    TEST_ASSERT_EQUAL_UINT32(300, lcast_throttle_delay_s(UINT32_MAX));

    lcast_throttles_t t;
    uint32_t now = 1000;
    lcast_throttles_init(&t, now);
    uint64_t a = 0x0A00020Au;
    uint64_t b = 0x0A00020Bu;
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, a, now));
        lcast_throttles_fail(&t, a, now);
    }
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, a, now));
    TEST_ASSERT_EQUAL_UINT32(5, lcast_throttles_fail(&t, a, now));
    TEST_ASSERT_EQUAL_UINT32(2, lcast_throttles_wait_s(&t, a, now));
    TEST_ASSERT_EQUAL_UINT32(1, lcast_throttles_wait_s(&t, a, now + 1));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, a, now + 2));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, b, now));   /* another address is free */
    now += 2;
    lcast_throttles_fail(&t, a, now);
    TEST_ASSERT_EQUAL_UINT32(4, lcast_throttles_wait_s(&t, a, now));
    now += 4;
    lcast_throttles_fail(&t, a, now);
    TEST_ASSERT_EQUAL_UINT32(8, lcast_throttles_wait_s(&t, a, now));
    lcast_throttles_ok(&t, a);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, a, now));
    TEST_ASSERT_EQUAL_UINT32(1, lcast_throttles_fail(&t, a, now));

    /* 16 addresses tracked: a 17th replaces the one seen longest ago */
    lcast_throttles_init(&t, 0);
    for (uint32_t k = 0; k < LCAST_THROTTLE_CLIENTS; k++) {
        for (int i = 0; i < 1; i++) {
            lcast_throttles_fail(&t, 100 + k, k);
        }
    }
    (void)lcast_throttles_wait_s(&t, 100, 20);   /* seen again: now the newest */
    lcast_throttles_fail(&t, 999, 21);
    TEST_ASSERT_EQUAL_UINT32(2, lcast_throttles_fail(&t, 100, 22));    /* kept */
    TEST_ASSERT_EQUAL_UINT32(1, lcast_throttles_fail(&t, 101, 23));    /* replaced: starts over */

    /* the global bucket: 30 failures, then one per 120 s */
    lcast_throttles_init(&t, 5000);
    for (uint32_t k = 0; k < 30; k++) {
        TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, 7000 + k, 5000));
        lcast_throttles_fail(&t, 7000 + k, 5000);
    }
    TEST_ASSERT_EQUAL_UINT32(120, lcast_throttles_wait_s(&t, 1, 5000));
    TEST_ASSERT_EQUAL_UINT32(20, lcast_throttles_wait_s(&t, 1, 5100));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, 1, 5120));
    lcast_throttles_fail(&t, 1, 5120);
    TEST_ASSERT_EQUAL_UINT32(120, lcast_throttles_wait_s(&t, 2, 5120));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, 2, 5120 + 30u * 120u));
}

TEST_CASE("lcast throttle: the latest logins' addresses skip the global bucket, never their own delay", "[lcast]")
{
    lcast_throttles_t t;
    lcast_throttles_init(&t, 1000);
    const uint64_t owner = 0x0A000232, rover2 = 0x0A000233, third = 0x0A000234, fourth = 0x0A000235, fifth = 0x0A000236;
    lcast_throttles_ok(&t, owner);
    /* someone else empties the bucket and keeps it empty with one failure per 120 s */
    for (uint32_t k = 0; k < LCAST_THROTTLE_BURST; k++) {
        lcast_throttles_fail(&t, 9000 + k, 1000);
    }
    TEST_ASSERT_EQUAL_UINT32(120, lcast_throttles_wait_s(&t, rover2, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, owner, 1000));   /* the owner's rover still logs in */
    lcast_throttles_fail(&t, 9999, 1120);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, owner, 1121));
    /* its own failures still delay it */
    for (int i = 0; i < 5; i++) {
        lcast_throttles_fail(&t, owner, 1200);
    }
    TEST_ASSERT_EQUAL_UINT32(2, lcast_throttles_wait_s(&t, owner, 1200));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, owner, 1202));
    /* four are trusted, newest first: a fifth login pushes the oldest out; a repeat moves up */
    TEST_ASSERT_EQUAL_UINT32(4, LCAST_THROTTLE_TRUSTED);
    lcast_throttles_ok(&t, rover2);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, rover2, 1300));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, owner, 1300));
    lcast_throttles_ok(&t, owner);   /* owner newest, rover2 second */
    lcast_throttles_ok(&t, third);
    lcast_throttles_ok(&t, fourth);
    TEST_ASSERT_EQUAL_UINT32(4, t.trusted_n);
    lcast_throttles_ok(&t, fifth);   /* rover2 goes */
    TEST_ASSERT_EQUAL_UINT32(4, t.trusted_n);
    TEST_ASSERT_EQUAL_UINT64(fifth, t.trusted[0]);
    TEST_ASSERT_EQUAL_UINT64(fourth, t.trusted[1]);
    TEST_ASSERT_EQUAL_UINT64(third, t.trusted[2]);
    TEST_ASSERT_EQUAL_UINT64(owner, t.trusted[3]);
    lcast_throttles_fail(&t, 9998, 1300);   /* the token that came back meanwhile: used again */
    TEST_ASSERT_NOT_EQUAL(0, lcast_throttles_wait_s(&t, rover2, 1300));
    TEST_ASSERT_EQUAL_UINT32(0, lcast_throttles_wait_s(&t, owner, 1300));
    /* a new table trusts nobody */
    lcast_throttles_init(&t, 0);
    TEST_ASSERT_EQUAL_UINT32(0, t.trusted_n);
}

/* ---- 11. journal gate --------------------------------------------------------------------------- */

TEST_CASE("lcast journal gate: listed at 10 s with a token, 6 an hour, unlisted counted, records packed", "[lcast]")
{
    lcast_jgate_t g;
    lcast_jgate_init(&g, 100);
    uint32_t un = 99;
    TEST_ASSERT_FALSE(lcast_jgate_try_list(&g, 9, 100, &un));
    TEST_ASSERT_EQUAL_UINT32(6, g.tokens);   /* none taken before 10 s */
    lcast_jgate_unlisted_end(&g);            /* a session under 10 s */
    lcast_jgate_unlisted_end(&g);
    TEST_ASSERT_TRUE(lcast_jgate_try_list(&g, 10, 100, &un));
    TEST_ASSERT_EQUAL_UINT32(2, un);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(lcast_jgate_try_list(&g, 12, 101, &un));
        TEST_ASSERT_EQUAL_UINT32(0, un);
    }
    TEST_ASSERT_FALSE(lcast_jgate_try_list(&g, 30, 102, &un));   /* the bucket is empty */
    lcast_jgate_unlisted_end(&g);                                 /* that one never got listed */
    TEST_ASSERT_FALSE(lcast_jgate_try_list(&g, 600, 100 + 599, &un));
    TEST_ASSERT_TRUE(lcast_jgate_try_list(&g, 600, 100 + 600, &un));   /* one token per 600 s */
    TEST_ASSERT_EQUAL_UINT32(1, un);
    TEST_ASSERT_FALSE(lcast_jgate_try_list(&g, 600, 100 + 601, &un));
    TEST_ASSERT_TRUE(lcast_jgate_try_list(&g, 20, 100 + 600 + 6u * 3600u, &un));   /* refilled, not beyond 6 */
    TEST_ASSERT_EQUAL_UINT32(5, g.tokens);

    /* DIAG_EV_ROVER_UP: conn, version, IPv6 bit, age, unlisted; b = the address word */
    int32_t a;
    int32_t b;
    lcast_addr_t q = a4(10, 0, 2, 2);
    lcast_jrec_up(1, 2, &q, 10, 3, &a, &b);
    TEST_ASSERT_EQUAL_INT32(1 | (2 << 4) | (10 << 8) | (3 << 16), a);
    TEST_ASSERT_EQUAL_INT32(167772674, b);   /* the QEMU check value */
    lcast_addr_t m = a4m(192, 168, 2, 50);   /* top bit set: b is negative, read back as b >>> 0 */
    lcast_jrec_up(0, 1, &m, 300, 40000, &a, &b);
    TEST_ASSERT_EQUAL_INT32(0 | (1 << 4) | (255 << 8) | (32767 << 16), a);   /* saturated */
    TEST_ASSERT_TRUE(a >= 0);
    TEST_ASSERT_EQUAL_UINT32(0xC0A80232u, (uint32_t)b);
    lcast_addr_t v6 = a6("fe80::1:aabb:ccdd");
    lcast_jrec_up(3, 2, &v6, 11, 0, &a, &b);
    TEST_ASSERT_EQUAL_INT32(3 | (2 << 4) | (1 << 6) | (11 << 8), a);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDDu, (uint32_t)b);
    /* DIAG_EV_ROVER_DOWN: conn and reason; b = seconds */
    lcast_jrec_down(2, LOCAL_CASTER_END_STALLED, 3725, &a, &b);
    TEST_ASSERT_EQUAL_INT32(2 | (2 << 8), a);
    TEST_ASSERT_EQUAL_INT32(3725, b);
    lcast_jrec_down(1, LOCAL_CASTER_END_RESTART, UINT32_MAX, &a, &b);
    TEST_ASSERT_EQUAL_INT32(1 | (6 << 8), a);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, b);

    /* the queue: 4 entries, the oldest dropped when full, 60 s at most */
    lcast_jq_t jq;
    memset(&jq, 0, sizeof(jq));
    lcast_jq_entry_t e;
    uint32_t expired = 0;
    TEST_ASSERT_FALSE(lcast_jq_head(&jq, 0, &e, &expired));
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_FALSE(lcast_jq_push(&jq, DIAG_EV_ROVER_UP, i, 0, (uint32_t)(10 + i)));
    }
    TEST_ASSERT_TRUE(lcast_jq_push(&jq, DIAG_EV_ROVER_DOWN, 4, 0, 20));
    TEST_ASSERT_TRUE(lcast_jq_head(&jq, 20, &e, &expired));
    TEST_ASSERT_EQUAL_INT32(1, e.a);   /* entry 0 went for room */
    lcast_jq_pop(&jq);
    TEST_ASSERT_TRUE(lcast_jq_head(&jq, 72, &e, &expired));   /* 60 s old: still in */
    TEST_ASSERT_EQUAL_UINT32(0, expired);
    TEST_ASSERT_EQUAL_INT32(2, e.a);
    TEST_ASSERT_TRUE(lcast_jq_head(&jq, 74, &e, &expired));   /* 12 and 13 are over 60 s old */
    TEST_ASSERT_EQUAL_UINT32(2, expired);
    TEST_ASSERT_EQUAL_INT32(4, e.a);
    TEST_ASSERT_EQUAL_UINT16(DIAG_EV_ROVER_DOWN, e.kind);
    lcast_jq_pop(&jq);
    TEST_ASSERT_FALSE(lcast_jq_head(&jq, 74, &e, &expired));
    lcast_jq_pop(&jq);   /* empty: nothing */
    TEST_ASSERT_EQUAL_UINT32(0, jq.n);

    TEST_ASSERT_EQUAL_STRING("closed", lcast_end_name(LOCAL_CASTER_END_CLOSED));
    TEST_ASSERT_EQUAL_STRING("restart", lcast_end_name(LOCAL_CASTER_END_RESTART));
    TEST_ASSERT_EQUAL_STRING("none", lcast_end_name((local_caster_end_t)99));
}

/* ---- 12. config record and rules ------------------------------------------------------------- */

static local_caster_cfg_t cfg_of(bool on, const char *mount, const char *user, const char *pass)
{
    local_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = on;
    strncpy(c.mountpoint, mount, sizeof(c.mountpoint) - 1);
    strncpy(c.username, user, sizeof(c.username) - 1);
    strncpy(c.password, pass, sizeof(c.password) - 1);
    return c;
}

TEST_CASE("lcast config record: round trip, exact v1 bytes, a later version, damaged records", "[lcast]")
{
    local_caster_cfg_t c = cfg_of(true, "ROVER", "rover", "secretpw");
    uint8_t buf[LCAST_CFG_BLOB_MAX + 8];
    size_t n = lcast_cfg_encode(&c, buf, sizeof(buf));
    static const uint8_t v1[] = { 1, 1, 5, 'R', 'O', 'V', 'E', 'R', 5, 'r', 'o', 'v', 'e', 'r',
                                  8, 's', 'e', 'c', 'r', 'e', 't', 'p', 'w' };
    TEST_ASSERT_EQUAL_size_t(sizeof(v1), n);
    TEST_ASSERT_EQUAL_MEMORY(v1, buf, n);
    local_caster_cfg_t d;
    TEST_ASSERT_TRUE(lcast_cfg_decode(buf, n, &d));
    TEST_ASSERT_TRUE(d.enabled);
    TEST_ASSERT_EQUAL_STRING("ROVER", d.mountpoint);
    TEST_ASSERT_EQUAL_STRING("rover", d.username);
    TEST_ASSERT_EQUAL_STRING("secretpw", d.password);
    TEST_ASSERT_TRUE(d.password_set);

    /* the largest record is 146 bytes */
    char m47[48];
    char u31[32];
    char p63[64];
    memset(m47, 'm', 47);
    m47[47] = '\0';
    memset(u31, 'u', 31);
    u31[31] = '\0';
    memset(p63, '~', 63);
    p63[63] = '\0';
    c = cfg_of(true, m47, u31, p63);
    memset(buf, 0x77, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, lcast_cfg_encode(&c, buf, LCAST_CFG_BLOB_MAX - 1));
    for (size_t i = 0; i < LCAST_CFG_BLOB_MAX - 1; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, buf[i]);   /* a record that did not fit leaves no password behind */
    }
    TEST_ASSERT_EQUAL_size_t(LCAST_CFG_BLOB_MAX, lcast_cfg_encode(&c, buf, sizeof(buf)));
    TEST_ASSERT_TRUE(lcast_cfg_decode(buf, LCAST_CFG_BLOB_MAX, &d));
    TEST_ASSERT_EQUAL_STRING(p63, d.password);

    /* off, no password */
    c = cfg_of(false, "onodiy", "rover", "");
    n = lcast_cfg_encode(&c, buf, sizeof(buf));
    TEST_ASSERT_TRUE(lcast_cfg_decode(buf, n, &d));
    TEST_ASSERT_FALSE(d.enabled);
    TEST_ASSERT_FALSE(d.password_set);

    /* a later version appends fields: decoded, the rest ignored */
    c = cfg_of(true, "ROVER", "rover", "secretpw");
    n = lcast_cfg_encode(&c, buf, sizeof(buf));
    buf[0] = 2;
    buf[n] = 0x55;
    buf[n + 1] = 0xAA;
    TEST_ASSERT_TRUE(lcast_cfg_decode(buf, n + 2, &d));
    TEST_ASSERT_EQUAL_STRING("secretpw", d.password);
    buf[0] = 1;
    TEST_ASSERT_TRUE(lcast_cfg_decode(buf, n + 2, &d));
    /* version 0, truncated, empty */
    buf[0] = 0;
    TEST_ASSERT_FALSE(lcast_cfg_decode(buf, n, &d));
    buf[0] = 1;
    for (size_t cut = 0; cut < n; cut++) {
        TEST_ASSERT_FALSE(lcast_cfg_decode(buf, cut, &d));
        TEST_ASSERT_FALSE(d.enabled);
        TEST_ASSERT_EQUAL_STRING("", d.password);
    }
    /* over-long strings, and values the rules refuse */
    uint8_t raw[300];
    blob_writer_t w;
    char m48[49];
    memset(m48, 'm', 48);
    m48[48] = '\0';
    blob_writer_init(&w, raw, sizeof(raw));
    blob_put_u8(&w, 1);
    blob_put_bool(&w, false);
    blob_put_str(&w, m48);
    blob_put_str(&w, "rover");
    blob_put_str(&w, "");
    TEST_ASSERT_FALSE(lcast_cfg_decode(raw, w.len, &d));
    static const char *const refused[][3] = {
        { "", "rover", "" }, { "RO VER", "rover", "" }, { "ROVER", "", "" }, { "ROVER", "ro:ver", "" },
        { "ROVER", "rover", "short" }, { "ROVER", "rover", "with space" },
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        blob_writer_init(&w, raw, sizeof(raw));
        blob_put_u8(&w, 1);
        blob_put_bool(&w, false);
        blob_put_str(&w, refused[i][0]);
        blob_put_str(&w, refused[i][1]);
        blob_put_str(&w, refused[i][2]);
        TEST_ASSERT_FALSE(lcast_cfg_decode(raw, w.len, &d));
    }
    /* on without a password cannot be stored */
    blob_writer_init(&w, raw, sizeof(raw));
    blob_put_u8(&w, 1);
    blob_put_bool(&w, true);
    blob_put_str(&w, "ROVER");
    blob_put_str(&w, "rover");
    blob_put_str(&w, "");
    TEST_ASSERT_FALSE(lcast_cfg_decode(raw, w.len, &d));
}

TEST_CASE("lcast config rules: every boundary and its words", "[lcast]")
{
    const char *field = NULL;
    const char *rule = NULL;
    const char *name_rule = "1 to 47 letters, digits, dots, hyphens or underscores";
    const char *user_rule = "1 to 31 letters, digits, dots, hyphens or underscores";
    const char *pass_rule = "8 to 63 characters, letters, digits and symbols, no spaces";
    char m47[48];
    char u31[32];
    char p63[64];
    memset(m47, 'a', 47);
    m47[47] = '\0';
    memset(u31, 'b', 31);
    u31[31] = '\0';
    memset(p63, '!', 63);
    p63[63] = '\0';

    local_caster_cfg_t c = cfg_of(true, "On.o-d_iy9", "rover", "12345678");
    TEST_ASSERT_TRUE(lcast_cfg_check(&c, false, &field, &rule));
    c = cfg_of(true, m47, u31, p63);
    TEST_ASSERT_TRUE(lcast_cfg_check(&c, false, &field, &rule));
    c = cfg_of(false, "ROVER", "rover", "");
    TEST_ASSERT_TRUE(lcast_cfg_check(&c, false, NULL, NULL));
    c = cfg_of(true, "ROVER", "rover", "");
    TEST_ASSERT_TRUE(lcast_cfg_check(&c, true, NULL, NULL));   /* the stored one is kept */

    /* mountpoint */
    c = cfg_of(false, "", "rover", "");
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("mountpoint", field);
    TEST_ASSERT_EQUAL_STRING(name_rule, rule);
    static const char *const bad_names[] = { "RO VER", "RO/VER", "RO;VER", "RÖVER", "RO:VER", "RO%VER" };
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        c = cfg_of(false, bad_names[i], "rover", "");
        TEST_ASSERT_FALSE_MESSAGE(lcast_cfg_check(&c, false, &field, &rule), bad_names[i]);
        TEST_ASSERT_EQUAL_STRING("mountpoint", field);
    }
    c = cfg_of(false, "ROVER", "rover", "");
    memset(c.mountpoint, 'a', sizeof(c.mountpoint));   /* not terminated: 48 */
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("mountpoint", field);
    /* username */
    c = cfg_of(false, "ROVER", "", "");
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("username", field);
    TEST_ASSERT_EQUAL_STRING(user_rule, rule);
    c = cfg_of(false, "ROVER", "ro:ver", "");
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("username", field);
    c = cfg_of(false, "ROVER", "rover", "");
    memset(c.username, 'b', sizeof(c.username));
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("username", field);
    /* password: 7 and 64 refused, 8 and 63 fine, no space or control, ':' allowed */
    c = cfg_of(false, "ROVER", "rover", "1234567");
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, true, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("password", field);
    TEST_ASSERT_EQUAL_STRING(pass_rule, rule);
    c = cfg_of(false, "ROVER", "rover", "12345678");
    memset(c.password, 'p', sizeof(c.password));
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, true, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("password", field);
    static const char *const bad_pass[] = { "1234 5678", "1234\t5678", "1234\x7f" "5678", "caf\xc3\xa9" "1234" };
    for (size_t i = 0; i < sizeof(bad_pass) / sizeof(bad_pass[0]); i++) {
        c = cfg_of(false, "ROVER", "rover", bad_pass[i]);
        TEST_ASSERT_FALSE(lcast_cfg_check(&c, true, &field, &rule));
        TEST_ASSERT_EQUAL_STRING(pass_rule, rule);
    }
    c = cfg_of(false, "ROVER", "rover", "pa:ss:wo:rd");
    TEST_ASSERT_TRUE(lcast_cfg_check(&c, false, NULL, NULL));
    /* turning on without a password given or stored */
    c = cfg_of(true, "ROVER", "rover", "");
    field = NULL;
    rule = NULL;
    TEST_ASSERT_FALSE(lcast_cfg_check(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("password", field);
    TEST_ASSERT_EQUAL_STRING("needed to turn this on", rule);
    TEST_ASSERT_FALSE(lcast_cfg_check(NULL, false, &field, &rule));
    TEST_ASSERT_NULL(field);
    /* the public check is the same rule set */
    c = cfg_of(true, "ROVER", "rover", "");
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, local_caster_check_config(&c, false, &field, &rule));
    TEST_ASSERT_EQUAL_STRING("needed to turn this on", rule);
    TEST_ASSERT_EQUAL_INT(ESP_OK, local_caster_check_config(&c, true, NULL, NULL));
}

/* ---- 13. rate estimate -------------------------------------------------------------------------- */

TEST_CASE("lcast bit rate: from the oldest of 60 samples 1 s apart to now, from when the input began", "[lcast]")
{
    lcast_rate_t r;
    lcast_rate_init(&r);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, 1000, 5000));   /* no sample */
    lcast_rate_sample(&r, 300, 0);                                 /* at init: nothing in yet */
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, 1000, 1000));   /* 0.7 s: too short to tell */
    TEST_ASSERT_EQUAL_UINT32(8000, lcast_rate_bps(&r, 1300, 1000));
    lcast_rate_sample(&r, 1000, 5000);                              /* too soon: not taken */
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    /* 12 s with nothing in (the receiver not streaming yet), then 1700 bytes/s from 12.8 s on */
    uint32_t t = 300;
    uint64_t bytes = 0;
    while (t < 16300u) {
        t += LCAST_RATE_STEP_MS;
        bytes = t > 12800u ? (t - 12800u) * 17u / 10u : 0;
        lcast_rate_sample(&r, t, bytes);
    }
    TEST_ASSERT_EQUAL_UINT32(17, r.n);
    TEST_ASSERT_EQUAL_UINT32(5950, (uint32_t)bytes);
    /* 4 s in: from the last still sample (12.3 s), not from boot (which would say 3296) */
    TEST_ASSERT_EQUAL_UINT32(6800u * 8000u / 4500u, lcast_rate_bps(&r, 16800, 6800));
    /* the input began after the newest sample: too short to tell yet */
    lcast_rate_t still;
    lcast_rate_init(&still);
    for (uint32_t i = 0; i < 5; i++) {
        lcast_rate_sample(&still, 300u + i * LCAST_RATE_STEP_MS, 100);
    }
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&still, 4700, 400));
    TEST_ASSERT_EQUAL_UINT32(2400, lcast_rate_bps(&still, 5300, 400));
    /* the ring keeps 60 samples: the oldest is 59 s back */
    for (int i = 0; i < 70; i++) {
        t += LCAST_RATE_STEP_MS;
        bytes += 1700u;
        lcast_rate_sample(&r, t, bytes);
    }
    TEST_ASSERT_EQUAL_UINT32(60, r.n);
    TEST_ASSERT_EQUAL_UINT32(13600, lcast_rate_bps(&r, t, bytes));
    TEST_ASSERT_EQUAL_UINT32(13600, lcast_rate_bps(&r, t + 500u, bytes + 850u));
    /* a pause inside the minute counts: 20 s with nothing in */
    for (int i = 0; i < 20; i++) {
        t += LCAST_RATE_STEP_MS;
        lcast_rate_sample(&r, t, bytes);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(1700ull * 39u * 8000u / 59000u), lcast_rate_bps(&r, t, bytes));
    /* no sample for a long time (the timer stopped): no rate rather than an old one */
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, t + 100000u, bytes + 17000u));
    /* samples missed, or a counter going back, start over */
    lcast_rate_sample(&r, t + 30000u, bytes + 1000u);
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, t + 30500u, bytes + 2000u));
    lcast_rate_sample(&r, t + 31000u, 10);
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, t + 32500u, 5));
    /* across the wrap of the millisecond clock */
    lcast_rate_init(&r);
    lcast_rate_sample(&r, UINT32_MAX - 4999u, 0);
    TEST_ASSERT_EQUAL_UINT32(2000, lcast_rate_bps(&r, 5000, 2500));
    /* the timer's sample came after the caller read the count and the clock: that sample is the latest reading */
    lcast_rate_init(&r);
    lcast_rate_sample(&r, 1000, 0);
    lcast_rate_sample(&r, 2000, 0);
    lcast_rate_sample(&r, 3000, 1700);
    TEST_ASSERT_EQUAL_UINT32(13600, lcast_rate_bps(&r, 2990, 1690));
}

TEST_CASE("lcast bit rate: callback jitter neither drops a nominal sample nor resets continuous input", "[lcast]")
{
    lcast_rate_t r;
    lcast_rate_init(&r);
    lcast_rate_sample(&r, 0, 0);
    const uint32_t times[] = { 1001, 2000, 3002, 4001, 5000 };
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        /* 1000 bytes/s: actual arrival timestamps, including jitter, must
         * be used in the denominator. 2000 is only 999 ms after 1001;
         * discarding it would make 3002 falsely look like a 2001 ms gap. */
        lcast_rate_sample(&r, times[i], times[i]);
        TEST_ASSERT_EQUAL_UINT32(i + 2, r.n);
        TEST_ASSERT_EQUAL_UINT32(8000, lcast_rate_bps(&r, times[i], times[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(6000, r.next_ms);
}

TEST_CASE("lcast bit rate: sustained jitter keeps the bounded minute window across clock wrap", "[lcast]")
{
    lcast_rate_t r;
    lcast_rate_init(&r);
    uint32_t start = UINT32_MAX - 1500u;
    lcast_rate_sample(&r, start, 100);
    const uint32_t late[] = { 1, 0, 2, 0, 200, 0, 999, 0 };
    uint32_t at = start;
    uint64_t bytes = 100;
    for (uint32_t i = 1; i <= 3u * LCAST_RATE_SAMPLES; i++) {
        uint32_t elapsed = i * LCAST_RATE_STEP_MS + late[(i - 1u) % (sizeof(late) / sizeof(late[0]))];
        at = start + elapsed;
        bytes = 100u + (uint64_t)elapsed * 2u;
        lcast_rate_sample(&r, at, bytes);
        TEST_ASSERT_EQUAL_UINT32(i + 1u < LCAST_RATE_SAMPLES ? i + 1u : LCAST_RATE_SAMPLES, r.n);
        TEST_ASSERT_EQUAL_UINT32(16000, lcast_rate_bps(&r, at, bytes));
        TEST_ASSERT_EQUAL_UINT32(16000, lcast_rate_bps(&r, at + 500u, bytes + 1000u));
        TEST_ASSERT_EQUAL_UINT32(start + (i + 1u) * LCAST_RATE_STEP_MS, r.next_ms);
    }
    /* The timer really stops: a stale history still cannot advertise a rate. */
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, at + 100000u, bytes + 200000u));
}

TEST_CASE("lcast bit rate: overdue callbacks coalesce while true gaps and early counter resets start over", "[lcast]")
{
    lcast_rate_t r;
    lcast_rate_init(&r);
    lcast_rate_sample(&r, 100, 0);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, 1099, 999));
    lcast_rate_sample(&r, 1100, 1000);
    /* At most two periods is still accepted, with one actual sample. */
    lcast_rate_sample(&r, 3100, 3000);
    TEST_ASSERT_EQUAL_UINT32(3, r.n);
    TEST_ASSERT_EQUAL_UINT32(4100, r.next_ms);
    lcast_rate_sample(&r, 3100, 3000);   /* timer catch-up in the same millisecond */
    lcast_rate_sample(&r, 3101, 3001);
    TEST_ASSERT_EQUAL_UINT32(3, r.n);
    lcast_rate_sample(&r, 4100, 4000);
    TEST_ASSERT_EQUAL_UINT32(8000, lcast_rate_bps(&r, 4100, 4000));

    lcast_rate_sample(&r, 6201, 6101);   /* a genuine gap over two periods resets the history and phase */
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    TEST_ASSERT_EQUAL_UINT32(7201, r.next_ms);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, 6201, 6101));
    lcast_rate_sample(&r, 7200, 7100);
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    lcast_rate_sample(&r, 7201, 7101);
    TEST_ASSERT_EQUAL_UINT32(2, r.n);
    TEST_ASSERT_EQUAL_UINT32(8000, lcast_rate_bps(&r, 7201, 7101));

    /* A reset is meaningful even before the next nominal deadline. */
    lcast_rate_sample(&r, 7300, 10);
    TEST_ASSERT_EQUAL_UINT32(1, r.n);
    TEST_ASSERT_EQUAL_UINT32(8300, r.next_ms);
    TEST_ASSERT_EQUAL_UINT32(0, lcast_rate_bps(&r, 7300, 10));
    lcast_rate_sample(&r, 8300, 1010);
    TEST_ASSERT_EQUAL_UINT32(2, r.n);
    TEST_ASSERT_EQUAL_UINT32(8000, lcast_rate_bps(&r, 8300, 1010));
}

TEST_CASE("lcast end reasons: the API's names and the console's words", "[lcast]")
{
    static const char *const words[] = { "ended", "the rover closed the connection", "the rover took no data for 10 s",
                                         "the connection was lost", "turned off or its settings changed",
                                         "the station lost its network address", "the station restarted" };
    for (int i = 0; i <= LOCAL_CASTER_END_RESTART; i++) {
        TEST_ASSERT_EQUAL_STRING(words[i], lcast_end_words((local_caster_end_t)i));
    }
    TEST_ASSERT_EQUAL_STRING("ended", lcast_end_words((local_caster_end_t)42));
}

/* ---- 14. fuzz --------------------------------------------------------------------------------- */

static void check_fields(const lcast_req_t *r)
{
    TEST_ASSERT_TRUE(r->len <= sizeof(r->buf));
    TEST_ASSERT_TRUE(strnlen(r->mount, sizeof(r->mount)) < sizeof(r->mount));
    TEST_ASSERT_TRUE(strnlen(r->agent, sizeof(r->agent)) < sizeof(r->agent));
    TEST_ASSERT_TRUE(strnlen(r->auth, sizeof(r->auth)) < sizeof(r->auth));
    TEST_ASSERT_TRUE(r->auth_len < sizeof(r->auth));
    TEST_ASSERT_TRUE(r->http_minor <= 1);
    if (r->result == LCAST_REQ_DONE && r->auth_present && !r->auth_bad) {
        TEST_ASSERT_EQUAL_size_t(r->auth_len, strlen(r->auth));
        TEST_ASSERT_NOT_NULL(memchr(r->auth, ':', r->auth_len));
    } else {
        TEST_ASSERT_EQUAL_size_t(0, r->auth_len);
    }
    for (size_t i = 0; r->agent[i] != '\0'; i++) {
        TEST_ASSERT_TRUE(r->agent[i] >= 0x20 && r->agent[i] <= 0x7E);
    }
}

TEST_CASE("lcast parser fuzz: 200 000 cases end in a defined result with every field in bounds", "[lcast]")
{
    static const char *const seeds[] = {
        REQ_V1,
        REQ_V2,
        "GET / HTTP/1.0\r\n\r\n",
        "GET http://h:1/ROVER?q HTTP/1.1\nOrigin: x\nUser-Agent: abc\n\n",
        "GET /ROVER HTTP/1.0\r\nAuthorization: Basic cm92ZXI6c2VjcmV0cHc6d2l0aDpjb2xvbnM=\r\n\r\n",
    };
    static const char alphabet[] = "GET /:\r\n\r\nHTTP/1.01Basic=+Authorization Ntrip-Version Sec-Fetch-\x00\x80\xff";
    static uint8_t buf[1400];
    static lcast_cred_t cred;
    lcast_cred_set(&cred, "rover", "secretpw");
    static char out[1024];
    lcast_req_t *r = &s_req;
    uint32_t done = 0;
    uint32_t bad = 0;
    uint32_t large = 0;
    s_rng = 0x9E3779B9u;
    for (uint32_t k = 0; k < 200000u; k++) {
        size_t n;
        uint32_t mode = rnd() % 4u;
        if (mode == 0) {
            /* random bytes */
            n = rnd() % sizeof(buf);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (uint8_t)rnd();
            }
        } else if (mode == 1) {
            /* the request alphabet */
            n = rnd() % sizeof(buf);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (uint8_t)alphabet[rnd() % (sizeof(alphabet) - 1u)];
            }
        } else {
            /* a valid request with mutations: flips, insertions, deletions */
            const char *s = seeds[rnd() % (sizeof(seeds) / sizeof(seeds[0]))];
            n = strlen(s);
            memcpy(buf, s, n);
            uint32_t edits = 1u + rnd() % 6u;
            for (uint32_t e = 0; e < edits && n > 0; e++) {
                size_t at = rnd() % n;
                switch (rnd() % 3u) {
                case 0:
                    buf[at] = (uint8_t)(rnd() % 3u == 0 ? alphabet[rnd() % (sizeof(alphabet) - 1u)] : rnd());
                    break;
                case 1:
                    if (n < sizeof(buf)) {
                        memmove(buf + at + 1, buf + at, n - at);
                        buf[at] = (uint8_t)alphabet[rnd() % (sizeof(alphabet) - 1u)];
                        n++;
                    }
                    break;
                default:
                    memmove(buf + at, buf + at + 1, n - at - 1);
                    n--;
                    break;
                }
            }
        }
        lcast_req_reset(r);
        size_t at = 0;
        lcast_req_result_t res = LCAST_REQ_NEED_MORE;
        while (at < n) {
            size_t step = 1u + rnd() % 300u;
            if (step > n - at) {
                step = n - at;
            }
            res = lcast_req_feed(r, buf + at, step);
            at += step;
            check_fields(r);
        }
        TEST_ASSERT_TRUE(res == LCAST_REQ_NEED_MORE || res == LCAST_REQ_DONE || res == LCAST_REQ_TOO_LARGE ||
                         res == LCAST_REQ_BAD);
        TEST_ASSERT_EQUAL_INT(res, r->result);
        done += res == LCAST_REQ_DONE;
        bad += res == LCAST_REQ_BAD;
        large += res == LCAST_REQ_TOO_LARGE;
        lcast_decision_t d = lcast_decide(r, res, "ROVER", &cred, rnd() % 2u, rnd() % 3u, 2);
        TEST_ASSERT_TRUE(d.action <= LCAST_ACT_E503);
        TEST_ASSERT_TRUE(d.flavour <= LCAST_FLAV_V2_RAW);
        if (d.action >= LCAST_ACT_E400) {
            TEST_ASSERT_TRUE(lcast_reply_error(out, sizeof(out), d.action, d.flavour, "ROVER", 5, "1.0", 0) > 0);
        }
    }
    /* every outcome was reached */
    TEST_ASSERT_TRUE(done > 1000);
    TEST_ASSERT_TRUE(bad > 1000);
    TEST_ASSERT_TRUE(large > 100);
}
