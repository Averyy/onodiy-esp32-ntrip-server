/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "ntrip_proto.h"
#include "unity.h"

/* ---- requests -------------------------------------------------------------------- */

TEST_CASE("ntrip v1 request bytes", "[ntrip]")
{
    char buf[NTRIP_REQUEST_MAX];
    char agent[NTRIP_AGENT_MAX];
    TEST_ASSERT_NOT_EQUAL(0, ntrip_build_agent("v0.3.0", agent, sizeof(agent)));
    size_t n = ntrip_build_v1_request(buf, sizeof(buf), "hunter2", "a1b2c3d4", agent);
    const char *want = "SOURCE hunter2 /a1b2c3d4\r\nSource-Agent: NTRIP onocoydiy-esp-um982/0.3.0\r\n\r\n";
    TEST_ASSERT_EQUAL_size_t(strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, buf);
    /* exactly three tokens, no version suffix, no Ntrip-Version header */
    TEST_ASSERT_NULL(strstr(buf, "HTTP"));
    TEST_ASSERT_NULL(strstr(buf, "Ntrip-Version"));

    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v1_request(buf, sizeof(buf), "two words", "m", agent));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v1_request(buf, sizeof(buf), "pw\r\nX", "m", agent));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v1_request(buf, sizeof(buf), "pw", "a/b", agent));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v1_request(buf, sizeof(buf), "", "m", agent));

    /* overflow: nothing usable, and the password does not linger in the buffer */
    char small[20];
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v1_request(small, sizeof(small), "hunter2", "a1b2c3d4", agent));
    for (size_t i = 0; i < sizeof(small); i++) {
        TEST_ASSERT_EQUAL_HEX8(0, small[i]);
    }
}

TEST_CASE("ntrip v2 request bytes with Basic auth and chunked body", "[ntrip]")
{
    char buf[NTRIP_REQUEST_MAX];
    const char *agent = "NTRIP onocoydiy-esp-um982/0.3.0";
    size_t n = ntrip_build_v2_request(buf, sizeof(buf), "caster.example.com", 2101, false, "MOUNT1", "alice",
                                      "s3cret", agent, false);
    const char *want = "POST /MOUNT1 HTTP/1.1\r\n"
                       "Host: caster.example.com:2101\r\n"
                       "Ntrip-Version: Ntrip/2.0\r\n"
                       "User-Agent: NTRIP onocoydiy-esp-um982/0.3.0\r\n"
                       "Authorization: Basic YWxpY2U6czNjcmV0\r\n"
                       "Connection: close\r\n"
                       "Transfer-Encoding: chunked\r\n"
                       "\r\n";
    TEST_ASSERT_EQUAL_size_t(strlen(want), n);
    TEST_ASSERT_EQUAL_STRING(want, buf);

    /* raw-body quirk: no Transfer-Encoding */
    n = ntrip_build_v2_request(buf, sizeof(buf), "caster.example.com", 2101, false, "MOUNT1", "alice", "s3cret",
                               agent, true);
    TEST_ASSERT_NOT_EQUAL(0, n);
    TEST_ASSERT_NULL(strstr(buf, "Transfer-Encoding"));

    /* Host carries the port unless it is the scheme default */
    ntrip_build_v2_request(buf, sizeof(buf), "h.example", 443, true, "M", "u", "p", agent, false);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\r\nHost: h.example\r\n"));
    ntrip_build_v2_request(buf, sizeof(buf), "h.example", 80, false, "M", "u", "p", agent, false);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\r\nHost: h.example\r\n"));
    ntrip_build_v2_request(buf, sizeof(buf), "h.example", 443, false, "M", "u", "p", agent, false);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\r\nHost: h.example:443\r\n"));
    ntrip_build_v2_request(buf, sizeof(buf), "2001:db8::1", 2101, false, "M", "u", "p", agent, false);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\r\nHost: [2001:db8::1]:2101\r\n"));

    /* header injection and invalid Basic user are refused */
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v2_request(buf, sizeof(buf), "h", 2101, false, "M", "a:b", "p", agent,
                                                       false));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v2_request(buf, sizeof(buf), "h", 2101, false, "M", "u", "p\r\nX: y",
                                                       agent, false));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_v2_request(buf, sizeof(buf), "h", 2101, false, "M N", "u", "p", agent,
                                                       false));
    /* maximum field lengths still fit */
    char host[64], mount[48], user[48], pass[64];
    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    memset(mount, 'm', sizeof(mount) - 1);
    mount[sizeof(mount) - 1] = '\0';
    memset(user, 'u', sizeof(user) - 1);
    user[sizeof(user) - 1] = '\0';
    memset(pass, 'p', sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';
    char long_agent[NTRIP_AGENT_MAX];
    memset(long_agent, 'a', sizeof(long_agent) - 1);
    long_agent[sizeof(long_agent) - 1] = '\0';
    TEST_ASSERT_NOT_EQUAL(0, ntrip_build_v2_request(buf, sizeof(buf), host, 65535, false, mount, user, pass,
                                                    long_agent, false));
}

TEST_CASE("ntrip base64 vectors", "[ntrip]")
{
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar", "Aladdin:open sesame" };
    static const char *const out[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy",
                                       "QWxhZGRpbjpvcGVuIHNlc2FtZQ==" };
    char b[64];
    for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        size_t n = ntrip_base64((const uint8_t *)in[i], strlen(in[i]), b, sizeof(b));
        TEST_ASSERT_EQUAL_size_t(strlen(out[i]), n);
        TEST_ASSERT_EQUAL_STRING(out[i], b);
    }
    TEST_ASSERT_EQUAL_size_t(0, ntrip_base64((const uint8_t *)"foo", 3, b, 4));   /* no room for the NUL */
}

TEST_CASE("ntrip agent string and Onocoy wire names", "[ntrip]")
{
    char a[NTRIP_AGENT_MAX];
    ntrip_build_agent("v0.3.0", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onocoydiy-esp-um982/0.3.0", a);
    ntrip_build_agent("v0.3.0-4-g47fc47b-dirty+dev", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onocoydiy-esp-um982/0.3.0-4-g47fc47b-dirty+dev", a);
    ntrip_build_agent("", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onocoydiy-esp-um982/unknown", a);
    ntrip_build_agent("a b\r", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onocoydiy-esp-um982/a_b_", a);
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_agent("1.0", a, 10));

    ntrip_wire_names_t w;
    ntrip_wire_names("servers.onocoy.com", false, "credUser", "", "label", &w);
    TEST_ASSERT_EQUAL_STRING("credUser", w.mount);
    ntrip_wire_names("servers.onocoy.com", true, "credUser", "", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("Roof1", w.mount);
    TEST_ASSERT_EQUAL_STRING("credUser", w.user);
    ntrip_wire_names("servers.onocoy.com", true, "credUser", "", "", &w);
    TEST_ASSERT_EQUAL_STRING("onocoydiy", w.mount);
    ntrip_wire_names("caster.example.com", true, "MOUNT", "bob", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("MOUNT", w.mount);
    TEST_ASSERT_EQUAL_STRING("bob", w.user);
}

/* ---- chunk encoder ---------------------------------------------------------------- */

TEST_CASE("ntrip chunk encoder", "[ntrip]")
{
    static uint8_t buf[NTRIP_CHUNK_HEAD_RESERVE + NTRIP_CHUNK_MAX_PAYLOAD + NTRIP_CHUNK_TAIL];
    size_t start = 99;

    memset(buf + NTRIP_CHUNK_HEAD_RESERVE, 0xAB, 25);
    size_t n = ntrip_chunk_wrap(buf, 25, &start);
    TEST_ASSERT_EQUAL_size_t(31, n);
    TEST_ASSERT_EQUAL_MEMORY("19\r\n", buf + start, 4);
    TEST_ASSERT_EQUAL_HEX8(0xAB, buf[start + 4]);
    TEST_ASSERT_EQUAL_MEMORY("\r\n", buf + start + 4 + 25, 2);
    TEST_ASSERT_EQUAL_size_t(NTRIP_CHUNK_HEAD_RESERVE, start + 4);

    n = ntrip_chunk_wrap(buf, 144, &start);
    TEST_ASSERT_EQUAL_size_t(4 + 144 + 2, n);
    TEST_ASSERT_EQUAL_MEMORY("90\r\n", buf + start, 4);

    n = ntrip_chunk_wrap(buf, 4096, &start);
    TEST_ASSERT_EQUAL_size_t(0, start);
    TEST_ASSERT_EQUAL_size_t(6 + 4096 + 2, n);
    TEST_ASSERT_EQUAL_MEMORY("1000\r\n", buf, 6);

    n = ntrip_chunk_wrap(buf, 1, &start);
    TEST_ASSERT_EQUAL_MEMORY("1\r\n", buf + start, 3);
    n = ntrip_chunk_wrap(buf, 0xAB, &start);
    TEST_ASSERT_EQUAL_MEMORY("ab\r\n", buf + start, 4);   /* lowercase, no leading zeros */

    /* never a zero-length chunk (that would end the body), never oversized */
    TEST_ASSERT_EQUAL_size_t(0, ntrip_chunk_wrap(buf, 0, &start));
    TEST_ASSERT_EQUAL_size_t(0, ntrip_chunk_wrap(buf, 4097, &start));
    TEST_ASSERT_EQUAL_STRING("0\r\n\r\n", NTRIP_CHUNK_TERMINATOR);
}

/* ---- frame counting --------------------------------------------------------------- */

static size_t make_frame(uint8_t *b, uint16_t type, bool mmb, size_t plen)
{
    memset(b, 0, 6 + plen);
    b[0] = 0xD3;
    b[1] = (uint8_t)((plen >> 8) & 0x03);
    b[2] = (uint8_t)plen;
    b[3] = (uint8_t)(type >> 4);
    b[4] = (uint8_t)((type & 0x0F) << 4);
    if (plen >= 7 && mmb) {
        b[3 + 6] = 0x02;
    }
    return 6 + plen;
}

TEST_CASE("ntrip counts frames and epoch ends in a hub read", "[ntrip]")
{
    uint8_t buf[512];
    size_t n = 0;
    n += make_frame(buf + n, 1005, false, 19);    /* station message: not an epoch end */
    n += make_frame(buf + n, 1077, true, 40);     /* GPS MSM7, more follow */
    n += make_frame(buf + n, 1127, true, 30);     /* BeiDou, split in two */
    n += make_frame(buf + n, 1127, false, 30);    /* last MSM of the epoch */
    n += make_frame(buf + n, 1104, false, 20);    /* receiver's own message number: not an MSM */
    uint32_t frames = 0, epochs = 0;
    ntrip_count_frames(buf, n, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(5, frames);
    TEST_ASSERT_EQUAL_UINT32(1, epochs);

    /* a truncated tail is not counted */
    ntrip_count_frames(buf, n - 3, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(4, frames);
    ntrip_count_frames(buf, 0, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(0, frames);
}

/* ---- reply classifier -------------------------------------------------------------- */

static ntrip_verdict_t run_reply(const char *bytes, bool v2, bool end_eof, ntrip_reply_t *r)
{
    ntrip_reply_init(r, v2);
    size_t used = 0;
    bool done = ntrip_reply_feed(r, (const uint8_t *)bytes, strlen(bytes), &used);
    if (!done) {
        ntrip_reply_end(r, end_eof);
    }
    TEST_ASSERT_EQUAL(NTRIP_RP_DONE, r->phase);
    return r->verdict;
}

static void expect(const char *bytes, bool success, ntrip_row_t row, ntrip_cls_t cls, uint8_t rule)
{
    static ntrip_reply_t r;
    ntrip_verdict_t v = run_reply(bytes, false, true, &r);
    char msg[160];
    snprintf(msg, sizeof(msg), "reply: %.100s", bytes);
    TEST_ASSERT_EQUAL_MESSAGE(success, v.success, msg);
    TEST_ASSERT_EQUAL_MESSAGE(row, v.row, msg);
    TEST_ASSERT_EQUAL_MESSAGE(cls, v.cls, msg);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(rule, v.rule, msg);
}

TEST_CASE("ntrip reply rows in order", "[ntrip]")
{
    expect("ICY 200 OK\r\n\r\n", true, NTRIP_ROW_NONE, NTRIP_CLS_NONE, 1);
    expect("OK\r\n", true, NTRIP_ROW_NONE, NTRIP_CLS_NONE, 2);
    expect("HTTP/1.1 200 OK\r\nServer: c\r\n\r\n", true, NTRIP_ROW_NONE, NTRIP_CLS_NONE, 3);
    expect("HTTP/1.0 200 OK\r\n\r\n", true, NTRIP_ROW_NONE, NTRIP_CLS_NONE, 3);
    expect("HTTP/1.1 401 Unauthorized\r\n\r\n", false, NTRIP_ROW_AUTH, NTRIP_CLS_AUTH, 4);
    expect("HTTP/1.1 409 Conflict\r\n\r\n", false, NTRIP_ROW_MOUNT_BUSY, NTRIP_CLS_MOUNT_BUSY, 4);
    expect("HTTP/1.1 500 Internal\r\n\r\n", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 4);
    expect("HTTP/1.1 503 Server Full\r\n\r\n", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 4);
    expect("HTTP/1.1 400 Bad Request\r\n\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("HTTP/1.1 403 Forbidden\r\n\r\nnot allowed", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("HTTP/1.1 404 Not Found\r\n\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("HTTP/1.1 431 Too Large\r\n\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("HTTP/1.1 501 Not Implemented\r\n\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("HTTP/1.1 302 Found\r\n\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 4);
    expect("SOURCETABLE 200 OK\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 5);
    expect("ERROR - Bad Password\r\n", false, NTRIP_ROW_AUTH, NTRIP_CLS_AUTH, 6);
    expect("ERROR - Mount Point Taken or Invalid\r\n", false, NTRIP_ROW_MOUNT_BUSY, NTRIP_CLS_MOUNT_BUSY, 7);
    expect("ERROR - Already Connected\r\n", false, NTRIP_ROW_MOUNT_BUSY, NTRIP_CLS_MOUNT_BUSY, 8);
    expect("ERROR - Bad Mountpoint\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 9);
    expect("ERROR - Missing Mountpoint\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 9);
    expect("ERROR - Too many sources\r\n", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 10);
    expect("ERROR - Something new\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 11);
    expect("Error 11 Unknown - epoll rd hup [caster v2]\r\n", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE,
           12);
    expect("Not authorized (no NTRIP source)\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 13);
    expect("<html><body>The NTRIP Caster has banned your IP</body></html>\r\n", false, NTRIP_ROW_BANNED,
           NTRIP_CLS_BANNED, 14);
    expect("<html>\r\n<table>caster table</table>\r\n</html>\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 14);
    expect("Your IP is BANNED for 3 hours\r\n", false, NTRIP_ROW_BANNED, NTRIP_CLS_BANNED, 15);
    expect("You are in the sandbox\r\n", false, NTRIP_ROW_BANNED, NTRIP_CLS_BANNED, 16);
    expect("ICY 200", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 17);   /* closed before the newline */
    expect("\x01\x02garbage\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 18);

    /* order and case: ERROR rows come before the ban words; matching is case-sensitive */
    expect("ERROR - you are banned\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 11);
    expect("HTTP/1.1 200 OK banned\r\n\r\n", true, NTRIP_ROW_NONE, NTRIP_CLS_NONE, 3);
    expect("icy 200 ok\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 18);
    expect("error - bad password\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 18);
    expect("Error x\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 18);
    expect("HTTP/1.1 2000 OK\r\n", false, NTRIP_ROW_CONFIG, NTRIP_CLS_CONFIG, 18);
}

TEST_CASE("ntrip reply details: 4xx body check, headers, timeouts, caps", "[ntrip]")
{
    static ntrip_reply_t r;
    ntrip_verdict_t v;

    /* a 4xx whose body says banned is a ban; the same code without it is not */
    v = run_reply("HTTP/1.1 403 Forbidden\r\nContent-Type: text/html\r\n\r\n<p>Your IP has been Banned</p>", false,
                  true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, v.row);
    TEST_ASSERT_EQUAL(NTRIP_HINT_BANNED, v.hint);
    v = run_reply("HTTP/1.1 403 Banned\r\n\r\n", false, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, v.row);
    /* 5xx: no body check */
    v = run_reply("HTTP/1.1 503 Full\r\n\r\nbanned", false, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, v.row);

    /* the body check finds the word across reads and stops after 2 KB */
    ntrip_reply_init(&r, false);
    size_t used;
    const char *head = "<html>\r\n";
    TEST_ASSERT_FALSE(ntrip_reply_feed(&r, (const uint8_t *)head, strlen(head), &used));
    TEST_ASSERT_EQUAL(NTRIP_RP_BODY, r.phase);
    TEST_ASSERT_FALSE(ntrip_reply_feed(&r, (const uint8_t *)"you are ban", 11, &used));
    TEST_ASSERT_FALSE(ntrip_reply_feed(&r, (const uint8_t *)"ned", 3, &used));
    ntrip_reply_end(&r, true);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, r.verdict.row);

    ntrip_reply_init(&r, false);
    ntrip_reply_feed(&r, (const uint8_t *)head, strlen(head), &used);
    static char filler[NTRIP_REPLY_BODY_MAX + 16];
    memset(filler, 'x', sizeof(filler));
    TEST_ASSERT_TRUE(ntrip_reply_feed(&r, (const uint8_t *)filler, sizeof(filler), &used));
    TEST_ASSERT_EQUAL_size_t(NTRIP_REPLY_BODY_MAX, used);
    TEST_ASSERT_EQUAL(NTRIP_ROW_CONFIG, r.verdict.row);

    /* Server: and Ntrip-Version: are recorded; v1-only hint without the header */
    v = run_reply("HTTP/1.1 401 Unauthorized\r\nServer: NTRIP Caster 2.0\r\nNtrip-Version: Ntrip/2.0\r\n\r\n", true,
                  true, &r);
    TEST_ASSERT_EQUAL_STRING("NTRIP Caster 2.0", r.server);
    TEST_ASSERT_TRUE(r.ntrip_v2_header);
    TEST_ASSERT_EQUAL(NTRIP_HINT_NONE, v.hint);
    v = run_reply("HTTP/1.1 401 Unauthorized\r\nServer: old\r\n\r\n", true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_HINT_V1_ONLY, v.hint);
    v = run_reply("ERROR - Bad Password\r\n", true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_HINT_V1_ONLY, v.hint);
    v = run_reply("ERROR - Bad Password\r\n", false, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_HINT_NONE, v.hint);

    /* a 200 whose headers never finish still streams after the header time */
    ntrip_reply_init(&r, true);
    const char *partial = "HTTP/1.1 200 OK\r\nServer: x\r\n";
    TEST_ASSERT_FALSE(ntrip_reply_feed(&r, (const uint8_t *)partial, strlen(partial), &used));
    TEST_ASSERT_EQUAL_UINT32(NTRIP_REPLY_HEADERS_MS, ntrip_reply_phase_ms(&r));
    ntrip_reply_end(&r, false);
    TEST_ASSERT_TRUE(r.verdict.success);

    /* the reply line is never padded out: a bare OK is enough, extra bytes are left alone */
    ntrip_reply_init(&r, false);
    const char *okplus = "OK\r\n\xD3\x00\x13";
    TEST_ASSERT_TRUE(ntrip_reply_feed(&r, (const uint8_t *)okplus, 7, &used));
    TEST_ASSERT_EQUAL_size_t(4, used);
    TEST_ASSERT_TRUE(r.verdict.success);

    /* byte-at-a-time feeding gives the same verdict */
    ntrip_reply_init(&r, false);
    const char *slow = "ICY 200 OK\r\n";
    bool done = false;
    for (size_t i = 0; i < strlen(slow); i++) {
        TEST_ASSERT_FALSE(done);
        done = ntrip_reply_feed(&r, (const uint8_t *)slow + i, 1, &used);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(r.verdict.success);

    /* nothing within the reply time: TRANSIENT / TIMEOUT */
    ntrip_reply_init(&r, false);
    TEST_ASSERT_EQUAL_UINT32(NTRIP_REPLY_LINE_MS, ntrip_reply_phase_ms(&r));
    ntrip_reply_end(&r, false);
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, r.verdict.row);
    TEST_ASSERT_EQUAL(NTRIP_CLS_TIMEOUT, r.verdict.cls);
    TEST_ASSERT_EQUAL_UINT8(17, r.verdict.rule);

    /* a first line past the cap is CONFIG */
    ntrip_reply_init(&r, false);
    static char longline[NTRIP_REPLY_LINE_MAX + 50];
    memset(longline, 'a', sizeof(longline));
    TEST_ASSERT_TRUE(ntrip_reply_feed(&r, (const uint8_t *)longline, sizeof(longline), &used));
    TEST_ASSERT_EQUAL(NTRIP_ROW_CONFIG, r.verdict.row);
    TEST_ASSERT_EQUAL_UINT8(0, r.verdict.rule);

    /* headers past 4 KB end the header phase */
    ntrip_reply_init(&r, false);
    ntrip_reply_feed(&r, (const uint8_t *)"HTTP/1.1 200 OK\r\n", 17, &used);
    static char hdrs[NTRIP_REPLY_HEADERS_MAX + 100];
    memset(hdrs, 'h', sizeof(hdrs));
    TEST_ASSERT_TRUE(ntrip_reply_feed(&r, (const uint8_t *)hdrs, sizeof(hdrs), &used));
    TEST_ASSERT_EQUAL_size_t(NTRIP_REPLY_HEADERS_MAX, used);
    TEST_ASSERT_TRUE(r.verdict.success);
}

TEST_CASE("ntrip sanitizes reply text and redacts secrets", "[ntrip]")
{
    const char *secrets[] = { "hunter2", NULL, "ab" };
    char out[64];
    const char *in = "bad password hunter2 for x\x01y";
    ntrip_sanitize(in, strlen(in), out, sizeof(out), secrets, 3);
    TEST_ASSERT_EQUAL_STRING("bad password ******* for x.y", out);
    /* a secret cut off at the end of the input is still hidden */
    in = "echo hunt";
    ntrip_sanitize(in, strlen(in), out, sizeof(out), secrets, 3);
    TEST_ASSERT_EQUAL_STRING("echo ****", out);
    /* short "secrets" are not matched (too many false hits) and cap is respected */
    in = "abcabc";
    ntrip_sanitize(in, strlen(in), out, 4, secrets, 3);
    TEST_ASSERT_EQUAL_STRING("abc", out);
}

/* ---- validation and policy --------------------------------------------------------- */

static ntrip_fields_t fields(const char *host, const char *mount, const char *user, const char *pass, bool v2)
{
    ntrip_fields_t f = { .host = host, .port = 2101, .mountpoint = mount, .username = user, .password = pass,
                         .label = "", .v2 = v2, .enabled = true };
    return f;
}

TEST_CASE("ntrip field validation", "[ntrip]")
{
    ntrip_fields_t f = fields("servers.onocoy.com", "a1b2c3d4", "", "hunter2", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f = fields("3.14.70.106", "MP_1.a-b", "", "pw", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f = fields("2001:db8::1", "M", "", "pw", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));

    static const char *const bad_hosts[] = { "bad_host.com", "-a.com", "a-.com", "a..b", ".a", "a b", "h\r\n",
                                             "1234567890123456789012345678901234567890123456789012345678901234" };
    for (size_t i = 0; i < sizeof(bad_hosts) / sizeof(bad_hosts[0]); i++) {
        f = fields(bad_hosts[i], "M", "", "pw", false);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_FIELDS_BAD_HOST, ntrip_validate_fields(&f), bad_hosts[i]);
    }
    f = fields("h.example", "two words", "", "pw", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_MOUNTPOINT, ntrip_validate_fields(&f));
    f = fields("h.example", "a/b", "", "pw", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_MOUNTPOINT, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "", "pass word", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_PASSWORD, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "u", "pass word", true);   /* Basic auth may carry spaces */
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "u", "pw\n", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_PASSWORD, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "a:b", "pw", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_USERNAME, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "u", "pw", true);
    f.label = "my-label";
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_LABEL, ntrip_validate_fields(&f));

    /* required fields only for an enabled slot */
    f = fields("h.example", "M", "", "", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_MISSING, ntrip_validate_fields(&f));
    f.enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "", "pw", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_MISSING, ntrip_validate_fields(&f));
    f = fields("servers.onocoy.com", "credUser", "", "pw", true);   /* Onocoy v2: user comes from the mountpoint */
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f = fields("", "M", "", "pw", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_MISSING, ntrip_validate_fields(&f));
    f = fields("h.example", "M", "", "pw", false);
    f.port = 0;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_PORT, ntrip_validate_fields(&f));
}

TEST_CASE("ntrip host domains and cross-slot policy", "[ntrip]")
{
    TEST_ASSERT_TRUE(ntrip_host_is_onocoy("servers.onocoy.com"));
    TEST_ASSERT_TRUE(ntrip_host_is_onocoy("ONOCOY.COM"));
    TEST_ASSERT_FALSE(ntrip_host_is_onocoy("notonocoy.com"));
    TEST_ASSERT_FALSE(ntrip_host_is_onocoy("onocoy.com.example.net"));
    TEST_ASSERT_TRUE(ntrip_host_is_open_caster("rtk2go.com"));
    TEST_ASSERT_TRUE(ntrip_host_is_open_caster("crtk.net"));
    TEST_ASSERT_FALSE(ntrip_host_is_open_caster("caster.emlid.com"));

    ntrip_policy_slot_t s[3] = {
        { true, "servers.onocoy.com", false },
        { true, "servers.onocoy.com", false },
        { false, "rtk2go.com", false },
    };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_ONOCOY, ntrip_check_policy(s, 3, 1));
    s[1].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 0));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));   /* disabled: no stream */

    s[1] = (ntrip_policy_slot_t){ true, "rtk2go.com", false };
    s[2] = (ntrip_policy_slot_t){ true, "rtk2go.com", true };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_RTK2GO, ntrip_check_policy(s, 3, 2));
    s[2].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 1));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 0));
    s[1].dup_ack = true;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));
    s[2] = (ntrip_policy_slot_t){ true, "caster.example.com", false };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 2));   /* uninvolved slot */
}

/* ---- backoff ------------------------------------------------------------------------- */

#define MID 200u   /* rnd giving a jitter factor of exactly 1.0 */
#define LO  0u     /* x 0.8 */
#define HI  400u   /* x 1.2 */

TEST_CASE("ntrip backoff rows per profile", "[ntrip]")
{
    /* TRANSIENT: floor x 2^n up to the cap */
    TEST_ASSERT_EQUAL_UINT32(1000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 0, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 1, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(8000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 3, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(15000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 4, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(15000,
                             ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 1000, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(30000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 10, true, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(60000,
                             ntrip_backoff_delay_ms(NTRIP_PROF_STANDARD, NTRIP_ROW_TRANSIENT, 20, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(10000,
                             ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_TRANSIENT, 0, false, 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(300000,
                             ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_TRANSIENT, 9, false, 30000, MID));

    /* table rows, last entry repeating */
    static const uint32_t fast_busy[] = { 5, 10, 15, 30, 30 };
    static const uint32_t std_busy[] = { 10, 20, 30, 60, 60 };
    static const uint32_t pol_busy[] = { 15, 30, 60, 120, 120 };
    static const uint32_t fast_auth[] = { 15, 30, 60, 120, 300, 300 };
    static const uint32_t std_auth[] = { 60, 300, 900, 1800, 1800 };
    static const uint32_t pol_auth[] = { 300, 900, 1800, 3600, 3600 };
    static const uint32_t banned[] = { 10800, 21600, 43200, 86400, 86400 };
    for (uint32_t n = 0; n < 5; n++) {
        TEST_ASSERT_EQUAL_UINT32(fast_busy[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_MOUNT_BUSY, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(std_busy[n] * 1000, ntrip_backoff_delay_ms(NTRIP_PROF_STANDARD, NTRIP_ROW_MOUNT_BUSY,
                                                                            n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(pol_busy[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_MOUNT_BUSY, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(std_auth[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_STANDARD, NTRIP_ROW_AUTH, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(pol_auth[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_CONFIG, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(banned[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_STANDARD, NTRIP_ROW_BANNED, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(banned[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_BANNED, n, false, 30000, MID));
    }
    for (uint32_t n = 0; n < 6; n++) {
        TEST_ASSERT_EQUAL_UINT32(fast_auth[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_AUTH, n, false, 30000, MID));
        /* fast: CONFIG and BANNED use the same row */
        TEST_ASSERT_EQUAL_UINT32(fast_auth[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_CONFIG, n, false, 30000, MID));
        TEST_ASSERT_EQUAL_UINT32(fast_auth[n] * 1000,
                                 ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_BANNED, n, false, 30000, MID));
    }
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_NONE, 0, false, 30000, MID));
}

TEST_CASE("ntrip backoff jitter bounds", "[ntrip]")
{
    /* +-20 %, never below the row's first value */
    TEST_ASSERT_EQUAL_UINT32(12000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 9, false, 30000, LO));
    TEST_ASSERT_EQUAL_UINT32(18000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 9, false, 30000, HI));
    TEST_ASSERT_EQUAL_UINT32(1000, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 0, false, 30000, LO));
    TEST_ASSERT_EQUAL_UINT32(1200, ntrip_backoff_delay_ms(NTRIP_PROF_FAST, NTRIP_ROW_TRANSIENT, 0, false, 30000, HI));
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_backoff_delay_ms(NTRIP_PROF_POLITE, NTRIP_ROW_AUTH, 0, false, 30000, LO));

    static const ntrip_row_t rows[] = { NTRIP_ROW_TRANSIENT, NTRIP_ROW_MOUNT_BUSY, NTRIP_ROW_AUTH, NTRIP_ROW_CONFIG,
                                        NTRIP_ROW_BANNED };
    for (int p = NTRIP_PROF_FAST; p <= NTRIP_PROF_POLITE; p++) {
        for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
            for (uint32_t n = 0; n < 40; n++) {
                uint32_t nominal = ntrip_backoff_delay_ms((ntrip_prof_t)p, rows[r], n, false, 30000, MID);
                uint32_t lo = ntrip_backoff_row_min_ms((ntrip_prof_t)p, rows[r]);
                for (uint32_t rnd = 0; rnd < 2000; rnd += 37) {
                    uint32_t d = ntrip_backoff_delay_ms((ntrip_prof_t)p, rows[r], n, false, 30000, rnd * 2654435761u);
                    TEST_ASSERT_TRUE(d >= lo);
                    TEST_ASSERT_TRUE(d >= nominal * 8 / 10);
                    TEST_ASSERT_TRUE(d <= nominal * 12 / 10);
                }
            }
        }
    }
}

/* ---- state machine ------------------------------------------------------------------- */

static const ntrip_sm_cfg_t k_fast = { NTRIP_PROF_FAST, true, 60000, 300000, 30000 };
static const ntrip_sm_cfg_t k_std = { NTRIP_PROF_STANDARD, false, 10000, 300000, 30000 };
static const ntrip_sm_cfg_t k_polite = { NTRIP_PROF_POLITE, false, 10000, 300000, 30000 };

static ntrip_sm_in_t ready(void)
{
    ntrip_sm_in_t in = { .enabled = true, .have_ip = true, .link_up = true, .paused = false, .fresh_data = true,
                         .session_open = false, .silent_ms = 0 };
    return in;
}

static ntrip_sm_act_t step(ntrip_sm_t *sm, const ntrip_sm_in_t *in, uint32_t now)
{
    ntrip_close_why_t why;
    return ntrip_sm_poll(sm, in, now, MID, &why);
}

static const ntrip_failure_t k_transient = { NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false, false };
static const ntrip_failure_t k_reached = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false };
static const ntrip_failure_t k_auth = { NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, true, false };
static const ntrip_failure_t k_busy = { NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, true, false };
static const ntrip_failure_t k_busy_ambiguous = { NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, true, true };

TEST_CASE("ntrip slot gate: enable, network, fresh data, stagger", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 1000;
    ntrip_sm_init(&sm, &k_std, 0x1234, t);
    ntrip_sm_in_t in = ready();
    ntrip_close_why_t why;

    in.enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t));
    TEST_ASSERT_EQUAL(NTRIP_SM_DISABLED, sm.state);
    in = ready();
    in.have_ip = false;
    in.link_up = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_NETWORK, sm.state);

    /* network arrives: a non-protected slot waits 0-10 s at random */
    in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, ntrip_sm_poll(&sm, &in, t, 5000, &why));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_DATA, sm.state);
    TEST_ASSERT_EQUAL_UINT32(5000, ntrip_sm_retry_in_ms(&sm, t));
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 4999));
    in.fresh_data = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 5000));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_DATA, sm.state);
    in.fresh_data = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 5001));

    /* the protected slot goes at once */
    ntrip_sm_init(&sm, &k_fast, 0x1234, t);
    in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, ntrip_sm_poll(&sm, &in, t, 5000, &why));
    ntrip_sm_connecting(&sm, t);
    TEST_ASSERT_EQUAL(NTRIP_SM_CONNECTING, sm.state);
}

TEST_CASE("ntrip slot session: stalls, pause, network loss are not failures", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_std, 1, t);
    sm.prev_link_up = true;                       /* network already up: no start-up stagger */
    ntrip_sm_in_t in = ready();
    ntrip_close_why_t why;
    ntrip_sm_failed(&sm, &k_transient, t, MID);   /* one earlier failure: attempt 1 */
    t += 5000;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t));
    ntrip_sm_connecting(&sm, t);
    ntrip_sm_connected(&sm, t);
    TEST_ASSERT_EQUAL(NTRIP_SM_STREAMING, sm.state);
    in.session_open = true;

    /* Wi-Fi blip with the IP still held: keep streaming */
    in.link_up = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 100));
    TEST_ASSERT_EQUAL(NTRIP_SM_STREAMING, sm.state);
    in.link_up = true;

    /* source stall: close after the slot's limit */
    in.silent_ms = 9999;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 200));
    in.silent_ms = 10000;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t + 300, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_SOURCE_STALL, why);
    ntrip_sm_closed(&sm, t + 300);
    in.session_open = false;
    in.silent_ms = 0;
    in.fresh_data = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 400));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_DATA, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1, sm.attempt);   /* not counted */

    /* pause: stay connected idle up to the limit, then close, stay PAUSED */
    in.fresh_data = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 500));
    ntrip_sm_connected(&sm, t + 500);
    in.session_open = true;
    in.paused = true;
    in.silent_ms = 3000;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 600));
    TEST_ASSERT_EQUAL(NTRIP_SM_PAUSED, sm.state);
    in.silent_ms = 10000;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t + 700, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_PAUSE_IDLE, why);
    ntrip_sm_closed(&sm, t + 700);
    in.session_open = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 800));
    TEST_ASSERT_EQUAL(NTRIP_SM_PAUSED, sm.state);
    in.paused = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 900));

    /* IP lost: close, WAIT_NETWORK, not a failure */
    ntrip_sm_connected(&sm, t + 900);
    in.session_open = true;
    in.silent_ms = 0;
    in.have_ip = false;
    in.link_up = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t + 1000, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_NO_IP, why);
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_NETWORK, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1, sm.attempt);

    /* disabled while connected: close */
    in = ready();
    in.session_open = true;
    in.enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t + 1100, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_DISABLED, why);
}

TEST_CASE("ntrip slot failures back off and a healthy session resets", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = UINT32_MAX - 20000;   /* cross the clock wrap during the test */
    ntrip_sm_init(&sm, &k_fast, 7, t);
    ntrip_sm_in_t in = ready();

    uint32_t d = ntrip_sm_failed(&sm, &k_transient, t, MID);
    TEST_ASSERT_EQUAL_UINT32(1000, d);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_CONNECT, sm.reason);
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 999));
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 1000));
    t += 1000;
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &k_transient, t, MID));
    t += 2000;
    TEST_ASSERT_EQUAL_UINT32(4000, ntrip_sm_failed(&sm, &k_transient, t, MID));

    /* a short session keeps the count growing */
    t += 4000;
    ntrip_sm_connected(&sm, t);
    ntrip_sm_sent(&sm, 100000, t + 30000);
    TEST_ASSERT_EQUAL_UINT32(8000, ntrip_sm_failed(&sm, &k_reached, t + 30000, MID));
    TEST_ASSERT_EQUAL_UINT32(4, sm.attempt);

    /* >= 60 s and >= 5 KB: back to the floor */
    t += 40000;
    ntrip_sm_connected(&sm, t);
    ntrip_sm_sent(&sm, 3000, t + 30000);
    ntrip_sm_sent(&sm, 3000, t + 61000);
    TEST_ASSERT_EQUAL_UINT32(0, sm.attempt);
    TEST_ASSERT_EQUAL_UINT32(1000, ntrip_sm_failed(&sm, &k_reached, t + 70000, MID));

    /* 60 s but too little data: not healthy */
    t += 80000;
    ntrip_sm_connected(&sm, t);
    ntrip_sm_sent(&sm, 1000, t + 90000);
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &k_reached, t + 90000, MID));
}

TEST_CASE("ntrip fast profile raises the transient cap after 10 minutes", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_fast, 7, t);
    uint32_t max_early = 0;
    uint32_t d = 0;
    while (t < NTRIP_FAST_LATE_MS) {
        d = ntrip_sm_failed(&sm, &k_transient, t, HI);
        if (t + d < NTRIP_FAST_LATE_MS && d > max_early) {
            max_early = d;
        }
        t += d;
    }
    TEST_ASSERT_EQUAL_UINT32(18000, max_early);    /* 15 s cap x 1.2 */
    d = ntrip_sm_failed(&sm, &k_transient, t, HI);
    TEST_ASSERT_EQUAL_UINT32(36000, d);            /* 30 s cap x 1.2 */
}

TEST_CASE("ntrip AUTH window and MOUNT_BUSY escalation", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_std, 42, t);

    /* never succeeded with these credentials: a password error is AUTH at once */
    uint32_t d = ntrip_sm_failed(&sm, &k_auth, t + 1000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    TEST_ASSERT_EQUAL_UINT32(60000, d);

    /* after a success, an AUTH reply within 300 s of the disconnect follows the MOUNT_BUSY row */
    ntrip_sm_user_edit(&sm, &k_std, 42, t);
    ntrip_sm_connected(&sm, t);
    ntrip_sm_closed(&sm, t + 1000);
    d = ntrip_sm_failed(&sm, &k_auth, t + 11000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_AUTH, sm.reason);
    TEST_ASSERT_EQUAL(NTRIP_ROW_MOUNT_BUSY, sm.row);
    TEST_ASSERT_EQUAL_UINT32(10000, d);
    d = ntrip_sm_failed(&sm, &k_auth, t + 302000, MID);   /* past the window */
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, sm.row);

    /* other credentials than the ones that worked: no window */
    ntrip_sm_user_edit(&sm, &k_std, 43, t + 400000);
    ntrip_sm_closed(&sm, t + 400000);
    ntrip_sm_failed(&sm, &k_auth, t + 401000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);

    /* 409 for 10 minutes: AUTH row (state stays a backoff), hint after 3 minutes */
    ntrip_sm_user_edit(&sm, &k_std, 42, t);
    t = 1000000;
    uint32_t start = t;
    bool hinted_early = false;
    while (t - start < NTRIP_BUSY_ESCALATE_MS) {
        d = ntrip_sm_failed(&sm, &k_busy, t, MID);
        TEST_ASSERT_EQUAL(NTRIP_ROW_MOUNT_BUSY, sm.row);
        if (t - start < NTRIP_BUSY_HINT_MS && sm.hint != NTRIP_HINT_NONE) {
            hinted_early = true;
        }
        t += d;
    }
    TEST_ASSERT_FALSE(hinted_early);
    TEST_ASSERT_EQUAL(NTRIP_HINT_MOUNT_HELD, sm.hint);
    d = ntrip_sm_failed(&sm, &k_busy, t, MID);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, sm.row);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1800000, d);
    /* any other failure ends the busy run */
    ntrip_sm_failed(&sm, &k_transient, t + d, MID);
    TEST_ASSERT_FALSE(sm.busy_active);

    /* the ambiguous text ("taken or invalid") becomes CONFIG */
    ntrip_sm_user_edit(&sm, &k_std, 42, t);
    start = t;
    while (t - start < NTRIP_BUSY_ESCALATE_MS) {
        t += ntrip_sm_failed(&sm, &k_busy_ambiguous, t, MID);
    }
    ntrip_sm_failed(&sm, &k_busy_ambiguous, t, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_CONFIG_ERROR, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_CONFIG, sm.reason);
}

TEST_CASE("ntrip polite hourly limit after 20 caster-reaching failures", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_polite, 9, t);
    /* failures that never reached the caster do not count */
    for (int i = 0; i < 30; i++) {
        t += ntrip_sm_failed(&sm, &k_transient, t, MID);
    }
    TEST_ASSERT_FALSE(sm.hourly);
    for (int i = 0; i < 19; i++) {
        t += ntrip_sm_failed(&sm, &k_reached, t, MID);
        TEST_ASSERT_FALSE(sm.hourly);
    }
    uint32_t d = ntrip_sm_failed(&sm, &k_reached, t, LO);
    TEST_ASSERT_TRUE(sm.hourly);
    TEST_ASSERT_EQUAL_UINT32(NTRIP_HOURLY_MS, d);
    d = ntrip_sm_failed(&sm, &k_transient, t + d, LO);   /* every attempt now, DNS included */
    TEST_ASSERT_EQUAL_UINT32(NTRIP_HOURLY_MS, d);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));

    /* a user edit lifts it and retries at once */
    ntrip_sm_user_edit(&sm, &k_polite, 9, t);
    TEST_ASSERT_FALSE(sm.hourly);
    TEST_ASSERT_EQUAL_UINT32(0, sm.attempt);
    ntrip_sm_in_t in = ready();
    in.link_up = true;
    sm.prev_link_up = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 1));
}

TEST_CASE("ntrip retry survives network loss and pauses", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_fast, 9, t);
    ntrip_sm_in_t in = ready();
    step(&sm, &in, t);
    uint32_t d = ntrip_sm_failed(&sm, &k_auth, t, MID);
    TEST_ASSERT_EQUAL_UINT32(15000, d);
    in.have_ip = false;
    in.link_up = false;
    step(&sm, &in, t + 1000);
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_NETWORK, sm.state);
    in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 2000));
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    in.paused = true;
    step(&sm, &in, t + 3000);
    TEST_ASSERT_EQUAL(NTRIP_SM_PAUSED, sm.state);
    in.paused = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 14999));
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 15000));
}

TEST_CASE("ntrip long backoff persistence", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 5000;
    ntrip_sm_init(&sm, &k_std, 77, t);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));

    /* short backoffs are never written */
    ntrip_sm_failed(&sm, &k_transient, t, MID);
    ntrip_sm_failed(&sm, &k_busy, t, MID);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));

    /* entering AUTH is */
    uint32_t d = ntrip_sm_failed(&sm, &k_auth, t, MID);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_backoff_rec_t rec;
    ntrip_sm_persist_get(&sm, t + 1000, &rec);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, rec.row);
    TEST_ASSERT_EQUAL(NTRIP_CLS_AUTH, rec.cls);
    TEST_ASSERT_EQUAL_UINT32(3, rec.attempt);
    TEST_ASSERT_EQUAL_UINT32((d - 1000 + 999) / 1000, rec.remaining_s);
    ntrip_sm_persist_done(&sm);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));
    ntrip_sm_failed(&sm, &k_auth, t + d, MID);       /* same class: no write */
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));

    /* codec round trip, strict version, trailing bytes from a newer writer ignored */
    uint8_t buf[NTRIP_BACKOFF_REC_MAX + 4];
    size_t n = ntrip_backoff_rec_encode(&rec, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, n);
    TEST_ASSERT_TRUE(n <= NTRIP_BACKOFF_REC_MAX);
    ntrip_backoff_rec_t back;
    buf[n] = 0xEE;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n + 1, &back));
    TEST_ASSERT_EQUAL(rec.row, back.row);
    TEST_ASSERT_EQUAL(rec.cls, back.cls);
    TEST_ASSERT_EQUAL_UINT32(rec.attempt, back.attempt);
    TEST_ASSERT_EQUAL_UINT32(rec.remaining_s, back.remaining_s);
    TEST_ASSERT_EQUAL(rec.hourly, back.hourly);
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n - 1, &back));
    buf[0] = NTRIP_BACKOFF_REC_VERSION + 1;
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n, &back));
    buf[0] = NTRIP_BACKOFF_REC_VERSION;
    buf[2] = (uint8_t)NTRIP_ROW_TRANSIENT;              /* not a long row */
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n, &back));

    /* a reboot waits out the remaining delay before the first attempt */
    ntrip_sm_t boot;
    uint32_t b = 123;
    ntrip_sm_init(&boot, &k_std, 77, b);
    ntrip_sm_restore(&boot, &rec, b);
    ntrip_sm_in_t in = ready();
    boot.prev_link_up = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&boot, &in, b + rec.remaining_s * 1000 - 1));
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, boot.state);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&boot, &in, b + rec.remaining_s * 1000));
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&boot));

    /* success clears the record and stores the credentials that worked */
    ntrip_sm_connected(&boot, b + rec.remaining_s * 1000);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&boot));
    ntrip_sm_persist_get(&boot, b, &rec);
    TEST_ASSERT_EQUAL(NTRIP_ROW_NONE, rec.row);
    TEST_ASSERT_EQUAL_UINT32(0, rec.remaining_s);
    TEST_ASSERT_TRUE(rec.good_valid);
    TEST_ASSERT_EQUAL_HEX32(77, rec.good_hash);
    ntrip_sm_persist_done(&boot);

    /* after a reboot the AUTH window knows these credentials worked */
    ntrip_sm_t again;
    ntrip_sm_init(&again, &k_std, 77, 0);
    ntrip_sm_restore(&again, &rec, 0);
    ntrip_sm_failed(&again, &k_auth, 20000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, again.state);
    TEST_ASSERT_EQUAL(NTRIP_ROW_MOUNT_BUSY, again.row);

    /* a user edit clears a stored long backoff */
    ntrip_sm_failed(&again, &k_auth, 400000, MID);
    ntrip_sm_persist_done(&again);
    ntrip_sm_user_edit(&again, &k_std, 78, 400001);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&again));
}

TEST_CASE("ntrip credential hash and Onocoy offline budget", "[ntrip]")
{
    uint32_t a = ntrip_cred_hash("servers.onocoy.com", 2101, false, "u", "", "pw", "");
    TEST_ASSERT_EQUAL_HEX32(a, ntrip_cred_hash("SERVERS.onocoy.com", 2101, false, "u", "", "pw", ""));
    TEST_ASSERT_NOT_EQUAL(a, ntrip_cred_hash("servers.onocoy.com", 2101, false, "u", "", "pw2", ""));
    TEST_ASSERT_NOT_EQUAL(a, ntrip_cred_hash("servers.onocoy.com", 2121, false, "u", "", "pw", ""));
    TEST_ASSERT_NOT_EQUAL(a, ntrip_cred_hash("servers.onocoy.com", 2101, true, "u", "", "pw", ""));
    TEST_ASSERT_NOT_EQUAL(ntrip_cred_hash("h", 1, true, "ab", "c", "p", ""),
                          ntrip_cred_hash("h", 1, true, "a", "bc", "p", ""));

    TEST_ASSERT_EQUAL_UINT32(473, ntrip_offline_budget_s(1000));
    TEST_ASSERT_EQUAL_UINT32(473, ntrip_offline_budget_s(1500));
    TEST_ASSERT_EQUAL_UINT32(465, ntrip_offline_budget_s(999));
    TEST_ASSERT_EQUAL_UINT32(430, ntrip_offline_budget_s(995));
    TEST_ASSERT_EQUAL_UINT32(387, ntrip_offline_budget_s(990));
    TEST_ASSERT_EQUAL_UINT32(300, ntrip_offline_budget_s(980));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_offline_budget_s(979));
}
