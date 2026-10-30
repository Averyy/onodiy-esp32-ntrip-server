/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "ntrip_proto.h"
#include "unity.h"

/* ---- requests -------------------------------------------------------------------- */

TEST_CASE("ntrip v1 request bytes", "[ntrip]")
{
    char buf[NTRIP_REQUEST_MAX];
    char agent[NTRIP_AGENT_MAX];
    TEST_ASSERT_NOT_EQUAL(0, ntrip_build_agent("v0.3.0", agent, sizeof(agent)));
    size_t n = ntrip_build_v1_request(buf, sizeof(buf), "hunter2", "a1b2c3d4", agent);
    const char *want = "SOURCE hunter2 /a1b2c3d4\r\nSource-Agent: NTRIP onodiy/0.3.0\r\n\r\n";
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
    const char *agent = "NTRIP onodiy/0.3.0";
    size_t n = ntrip_build_v2_request(buf, sizeof(buf), "caster.example.com", 2101, false, "MOUNT1", "alice",
                                      "s3cret", agent, false);
    const char *want = "POST /MOUNT1 HTTP/1.1\r\n"
                       "Host: caster.example.com:2101\r\n"
                       "Ntrip-Version: Ntrip/2.0\r\n"
                       "User-Agent: NTRIP onodiy/0.3.0\r\n"
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
    TEST_ASSERT_EQUAL_STRING("NTRIP onodiy/0.3.0", a);
    ntrip_build_agent("v0.3.0-4-g47fc47b-dirty+dev", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onodiy/0.3.0-4-g47fc47b-dirty+dev", a);
    ntrip_build_agent("", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onodiy/unknown", a);
    ntrip_build_agent("a b\r", a, sizeof(a));
    TEST_ASSERT_EQUAL_STRING("NTRIP onodiy/a_b_", a);
    TEST_ASSERT_EQUAL_size_t(0, ntrip_build_agent("1.0", a, 10));

    ntrip_wire_names_t w;
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), false, "credUser", "", "label", &w);
    TEST_ASSERT_EQUAL_STRING("credUser", w.mount);
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "credUser", "", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("Roof1", w.mount);
    TEST_ASSERT_EQUAL_STRING("credUser", w.user);
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "credUser", "", "", &w);
    TEST_ASSERT_EQUAL_STRING("onodiy", w.mount);
    ntrip_wire_names(ntrip_host_is_onocoy("caster.example.com"), true, "MOUNT", "bob", "Roof1", &w);
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
    ntrip_epoch_track_t tr;
    ntrip_epoch_track_init(&tr);
    ntrip_count_frames(buf, n, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(5, frames);
    TEST_ASSERT_EQUAL_UINT32(1, epochs);

    /* a truncated tail is not counted */
    ntrip_epoch_track_init(&tr);
    ntrip_count_frames(buf, n - 3, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(4, frames);
    ntrip_count_frames(buf, 0, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(0, frames);
}

/* MSM frame with an epoch time (payload bits 24..53) and the multiple-message bit. */
static size_t make_msm(uint8_t *b, uint16_t type, uint32_t tow, bool mmb)
{
    size_t n = make_frame(b, type, mmb, 20);
    b[3 + 3] = (uint8_t)(tow >> 22);
    b[3 + 4] = (uint8_t)(tow >> 14);
    b[3 + 5] = (uint8_t)(tow >> 6);
    b[3 + 6] = (uint8_t)(((tow & 0x3Fu) << 2) | (mmb ? 0x02u : 0x00u));
    return n;
}

TEST_CASE("ntrip epoch count: a lost closing MSM is ended by the next epoch time, across reads", "[ntrip]")
{
    uint8_t buf[512];
    uint32_t frames = 0, epochs = 0, total = 0;
    ntrip_epoch_track_t tr;
    ntrip_epoch_track_init(&tr);

    /* Epoch 1000: GPS and GLONASS, the closing BeiDou MSM is lost. */
    size_t n = 0;
    n += make_msm(buf + n, 1077, 1000, true);
    n += make_msm(buf + n, 1087, 7000, true);
    ntrip_count_frames(buf, n, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(2, frames);
    TEST_ASSERT_EQUAL_UINT32(0, epochs);
    /* Next read: epoch 2000 complete; its GPS MSM ends epoch 1000. */
    n = 0;
    n += make_msm(buf + n, 1005, 0, false);        /* not an MSM: nothing to do with epochs */
    n += make_msm(buf + n, 1077, 2000, true);
    n += make_msm(buf + n, 1127, 2000 - 14000 + 604800000, true);
    n += make_msm(buf + n, 1127, 2000 - 14000 + 604800000, true);   /* same type, same epoch: split */
    n += make_msm(buf + n, 1087, 8000, false);
    ntrip_count_frames(buf, n, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(5, frames);
    TEST_ASSERT_EQUAL_UINT32(2, epochs);
    total += epochs;
    /* A new constellation alone does not end an open epoch; 110x never counts. */
    n = 0;
    n += make_msm(buf + n, 1097, 3000, true);
    n += make_msm(buf + n, 1107, 3000, false);
    n += make_msm(buf + n, 1077, 3000, true);
    ntrip_count_frames(buf, n, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(0, epochs);
    /* A resync starts from a clean state: the open epoch is forgotten. */
    ntrip_epoch_track_init(&tr);
    n = make_msm(buf, 1077, 4000, true);
    ntrip_count_frames(buf, n, &tr, &frames, &epochs);
    TEST_ASSERT_EQUAL_UINT32(0, epochs);
    TEST_ASSERT_EQUAL_UINT32(2, total);
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

    /* A chunked 4xx body is read through its chunk framing: a word cut by a
     * chunk boundary is still found, and a size line never reads as text. */
    v = run_reply("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n"
                  "e\r\n<p>Your IP is \r\n"
                  "3;ext=1\r\nban\r\n"
                  "A\r\nned.</p>\r\n\r\n"
                  "0\r\n\r\n",
                  true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, v.row);
    TEST_ASSERT_EQUAL(NTRIP_HINT_BANNED, v.hint);
    v = run_reply("HTTP/1.1 403 Forbidden\r\ntransfer-encoding:  Chunked \r\n\r\n"
                  "2\r\nba\r\n4\r\nnned\r\n0\r\n\r\n",
                  true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, v.row);
    /* "ba" "nned" across the size line, but not chunked: no word there */
    v = run_reply("HTTP/1.1 403 Forbidden\r\n\r\nba\r\n4\r\nnned", true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_CONFIG, v.row);
    /* the terminating chunk ends the body check at once */
    ntrip_reply_init(&r, true);
    const char *chunked = "HTTP/1.1 401 Unauthorized\r\nTransfer-Encoding: chunked\r\n\r\n"
                          "5\r\nhello\r\n0\r\n\r\n";
    TEST_ASSERT_TRUE(ntrip_reply_feed(&r, (const uint8_t *)chunked, strlen(chunked), &used));
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, r.verdict.row);
    /* a malformed size line: the rest is searched as it comes */
    v = run_reply("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nbanned", true, true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_BANNED, v.row);

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
    TEST_ASSERT_TRUE(ntrip_host_in_domain("servers.onocoy.com", "onocoy.com"));
    TEST_ASSERT_TRUE(ntrip_host_in_domain("ONOCOY.COM", "onocoy.com"));
    TEST_ASSERT_FALSE(ntrip_host_in_domain("notonocoy.com", "onocoy.com"));
    TEST_ASSERT_FALSE(ntrip_host_in_domain("onocoy.com.evil", "onocoy.com"));
    TEST_ASSERT_FALSE(ntrip_host_in_domain("onocoy.com", ""));
    TEST_ASSERT_FALSE(ntrip_host_in_domain(NULL, "onocoy.com"));
    TEST_ASSERT_TRUE(ntrip_host_is_rtk2go("RTK2GO.com"));
    TEST_ASSERT_FALSE(ntrip_host_is_rtk2go("rtk2go.com.example.net"));
    TEST_ASSERT_TRUE(ntrip_host_is_onocoy("servers.onocoy.com"));
    TEST_ASSERT_TRUE(ntrip_host_is_onocoy("ONOCOY.COM"));
    TEST_ASSERT_FALSE(ntrip_host_is_onocoy("notonocoy.com"));
    TEST_ASSERT_FALSE(ntrip_host_is_onocoy("onocoy.com.example.net"));
    TEST_ASSERT_TRUE(ntrip_host_is_open_caster("rtk2go.com"));
    TEST_ASSERT_TRUE(ntrip_host_is_open_caster("crtk.net"));
    TEST_ASSERT_FALSE(ntrip_host_is_open_caster("caster.emlid.com"));

    ntrip_policy_slot_t s[3] = {
        { true, "servers.onocoy.com", false, false },
        { true, "servers.onocoy.com", false, false },
        { false, "rtk2go.com", false, false },
    };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_ONOCOY, ntrip_check_policy(s, 3, 1));
    s[1].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 0));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));   /* disabled: no stream */

    s[1] = (ntrip_policy_slot_t){ true, "rtk2go.com", false, false };
    s[2] = (ntrip_policy_slot_t){ true, "rtk2go.com", true, false };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_RTK2GO, ntrip_check_policy(s, 3, 2));
    s[2].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 1));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 0));
    s[1].dup_ack = true;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));
    s[2] = (ntrip_policy_slot_t){ true, "caster.example.com", false, false };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 2));   /* uninvolved slot */
}

/* The cases the web API names in 409 caster_policy (they used to be decided a
 * second time in the web component; ntrip_server is the only owner now). */
TEST_CASE("ntrip cross-slot policy: hosts match by domain, any case; the edited slot's ack counts", "[ntrip]")
{
    ntrip_policy_slot_t s[3] = {
        { .enabled = true, .host = "servers.onocoy.com" },
        { .enabled = true, .host = "rtk2go.com" },
        { .enabled = false, .host = "" },
    };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 1));
    s[1].dup_ack = true;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 0));   /* the ack is per saved slot */
    s[2] = (ntrip_policy_slot_t){ .enabled = true, .host = "Servers.Onocoy.com" };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_ONOCOY, ntrip_check_policy(s, 3, 2));
    s[2] = (ntrip_policy_slot_t){ .enabled = true, .host = "RTK2GO.com", .dup_ack = true };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_RTK2GO, ntrip_check_policy(s, 3, 2));
    s[2] = (ntrip_policy_slot_t){ .enabled = true, .host = "caster.centipede.fr" };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 2));
    s[2] = (ntrip_policy_slot_t){ .enabled = true, .host = "crtk.net" };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(s, 3, 2));
    s[2].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 2));            /* disabled: streams nowhere */
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 3));            /* no such slot */
    s[0].host = NULL;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(s, 3, 1));            /* no host: nothing to pair */
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

/* Without the Onocoy login guard (it has its own tests). */
static const ntrip_sm_cfg_t k_fast = { NTRIP_PROF_FAST, true, 60000, 300000, 30000, false };
static const ntrip_sm_cfg_t k_std = { NTRIP_PROF_STANDARD, false, 10000, 300000, 30000, false };
static const ntrip_sm_cfg_t k_polite = { NTRIP_PROF_POLITE, false, 10000, 300000, 30000, false };

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

static const ntrip_failure_t k_transient = { NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false, false, false, false, 0, 0 };
static const ntrip_failure_t k_reached = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false, false, false, 0,
                                           0 };
static const ntrip_failure_t k_auth = { NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, true, false, false, true, 0, 0 };
static const ntrip_failure_t k_busy = { NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, true, false, false, true, 0, 0 };
static const ntrip_failure_t k_busy_ambiguous = { NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, true, true, false, true, 0,
                                                  0 };
/* A refused connect reached the caster's host: it follows the TRANSIENT row on every slot. */
static const ntrip_failure_t k_refused = { NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false, false, true, false, 0, 0 };
static const ntrip_failure_t k_dns = { NTRIP_CLS_DNS, NTRIP_ROW_TRANSIENT, false, false, false, false, 0, 0 };

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

    /* A Wi-Fi drop clears the address at once (and lwIP aborts the session):
     * close as "no network", not a failure. Re-association alone brings no
     * address: still waiting. The address back: the protected slot would go
     * at once, this one waits its random stagger from that moment. */
    in.have_ip = false;
    in.link_up = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t + 100, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_NO_IP, why);
    ntrip_sm_closed(&sm, t + 100);
    in.session_open = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 2000));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_NETWORK, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1, sm.attempt);
    in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, ntrip_sm_poll(&sm, &in, t + 3000, 2500, &why));
    TEST_ASSERT_EQUAL_UINT32(2500, ntrip_sm_retry_in_ms(&sm, t + 3000));
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 5500));
    ntrip_sm_connecting(&sm, t + 5500);
    ntrip_sm_connected(&sm, t + 5500);
    in.session_open = true;
    t += 6000;

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

    uint32_t d = ntrip_sm_failed(&sm, &k_refused, t, MID);
    TEST_ASSERT_EQUAL_UINT32(1000, d);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_CONNECT, sm.reason);
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + 999));
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 1000));
    t += 1000;
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &k_refused, t, MID));
    t += 2000;
    TEST_ASSERT_EQUAL_UINT32(4000, ntrip_sm_failed(&sm, &k_refused, t, MID));

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
        d = ntrip_sm_failed(&sm, &k_refused, t, HI);
        if (t + d < NTRIP_FAST_LATE_MS && d > max_early) {
            max_early = d;
        }
        t += d;
    }
    TEST_ASSERT_EQUAL_UINT32(18000, max_early);    /* 15 s cap x 1.2 */
    d = ntrip_sm_failed(&sm, &k_refused, t, HI);
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
    /* The next step of the same row is written too (else a reboot would put
     * the slot back a step), until the row's last entry repeats. */
    uint32_t tt = t + d;
    tt += ntrip_sm_failed(&sm, &k_auth, tt, MID);    /* attempt 4 */
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_sm_persist_done(&sm);
    tt += ntrip_sm_failed(&sm, &k_auth, tt, MID);    /* attempt 5: the last entry */
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_sm_persist_done(&sm);
    tt += ntrip_sm_failed(&sm, &k_auth, tt, MID);    /* the last entry repeats: no write */
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
    TEST_ASSERT_EQUAL_UINT8(rec.reached, back.reached);
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n - 1, &back));
    /* a version 1 record (no failure count) is still read */
    buf[0] = 1;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n - 1, &back));
    TEST_ASSERT_EQUAL_UINT8(0, back.reached);
    TEST_ASSERT_EQUAL_UINT32(rec.attempt, back.attempt);
    buf[0] = NTRIP_BACKOFF_REC_VERSION + 1;
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n, &back));
    buf[0] = NTRIP_BACKOFF_REC_VERSION;
    buf[2] = (uint8_t)NTRIP_ROW_TRANSIENT;              /* not a long row */
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n, &back));

    /* a reboot waits the stored step's full delay (AUTH row step 2: 900 s,
     * not the 899 s left when the record was written) before the first attempt */
    ntrip_sm_t boot;
    uint32_t b = 123;
    ntrip_sm_init(&boot, &k_std, 77, b);
    ntrip_sm_restore(&boot, &rec, b);
    ntrip_sm_in_t in = ready();
    boot.prev_link_up = true;
    TEST_ASSERT_EQUAL_UINT32(899, rec.remaining_s);
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&boot, &in, b + 900000 - 1));
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, boot.state);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&boot, &in, b + 900000));
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&boot));

    /* success clears the record and stores the credentials that worked */
    ntrip_sm_connected(&boot, b + 900000);
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

/* Run one boot of a slot from a stored record (NULL: first boot); fail every
 * attempt with `f` until `uptime` ms; return the record the last write left. */
static ntrip_backoff_rec_t reboot_run(const ntrip_sm_cfg_t *cfg, const ntrip_backoff_rec_t *stored,
                                      const ntrip_failure_t *f, uint32_t uptime, uint32_t *attempts)
{
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, cfg, 5, 0);
    ntrip_backoff_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (stored != NULL) {
        ntrip_sm_restore(&sm, stored, 0);
        rec = *stored;
    }
    sm.prev_link_up = true;
    ntrip_sm_in_t in = ready();
    for (uint32_t t = 0; t < uptime; t += 1000) {
        if (step(&sm, &in, t) == NTRIP_ACT_CONNECT) {
            ntrip_sm_connecting(&sm, t);
            ntrip_sm_failed(&sm, f, t, MID);
            (*attempts)++;
        }
        if (ntrip_sm_persist_due(&sm)) {
            ntrip_sm_persist_get(&sm, t, &rec);
            ntrip_sm_persist_done(&sm);
        }
    }
    return rec;
}

TEST_CASE("ntrip a reboot loop never shortens a long backoff", "[ntrip]")
{
    /* A polite slot with a wrong password (never accepted: no AUTH window)
     * on a device that reboots every 10 minutes for a day. Each boot used to
     * restore the first step (5 min) and try again: about 6 bad passwords an
     * hour. Now each boot waits the stored step in full. */
    uint32_t attempts = 0;
    ntrip_backoff_rec_t rec = reboot_run(&k_polite, NULL, &k_auth, 600000, &attempts);
    for (int boot = 1; boot < 144; boot++) {
        rec = reboot_run(&k_polite, &rec, &k_auth, 600000, &attempts);
    }
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, rec.row);
    TEST_ASSERT_TRUE(attempts <= 3);   /* 0 s, 5 min, then the 15 min step outlasts every boot */

    /* Caster-reaching failures carry over toward the polite hourly limit. */
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_polite, 5, t);
    for (int i = 0; i < 12; i++) {
        t += ntrip_sm_failed(&sm, &k_reached, t, MID);
    }
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_sm_persist_get(&sm, t, &rec);
    TEST_ASSERT_EQUAL_UINT8(12, rec.reached);
    ntrip_sm_t next;
    ntrip_sm_init(&next, &k_polite, 5, 0);
    ntrip_sm_restore(&next, &rec, 0);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&next));
    for (int i = 0; i < 7; i++) {
        ntrip_sm_failed(&next, &k_reached, 1000, MID);
        TEST_ASSERT_FALSE(next.hourly);
    }
    ntrip_sm_failed(&next, &k_reached, 1000, MID);
    TEST_ASSERT_TRUE(next.hourly);
}

TEST_CASE("ntrip the polite hourly limit ends when the caster accepts the source", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_polite, 9, t);
    for (int i = 0; i < 20; i++) {
        t += ntrip_sm_failed(&sm, &k_reached, t, MID);
    }
    TEST_ASSERT_TRUE(sm.hourly);
    ntrip_sm_persist_done(&sm);

    /* Accepted, then the caster restarts 40 s later (no healthy session):
     * the next attempt follows the normal row, not the hour. */
    ntrip_sm_connected(&sm, t);
    TEST_ASSERT_FALSE(sm.hourly);
    TEST_ASSERT_EQUAL_UINT32(0, sm.reached_streak);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));   /* the stored hourly flag goes too */
    ntrip_sm_sent(&sm, 2000, t + 40000);
    uint32_t d = ntrip_sm_failed(&sm, &k_reached, t + 40000, MID);
    TEST_ASSERT_TRUE(d < NTRIP_HOURLY_MS);
    TEST_ASSERT_EQUAL_UINT32(300000, d);   /* the attempt count kept escalating: the polite cap */
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

/* ---- failures that never reached the caster ------------------------------------- */

TEST_CASE("ntrip unreached run: DNS and timed-out connects start it, a refused connect or success end it", "[ntrip]")
{
    TEST_ASSERT_TRUE(ntrip_failure_unreached(&k_dns));
    TEST_ASSERT_TRUE(ntrip_failure_unreached(&k_transient));   /* timed out or failed locally */
    TEST_ASSERT_FALSE(ntrip_failure_unreached(&k_refused));
    TEST_ASSERT_FALSE(ntrip_failure_unreached(&k_reached));
    TEST_ASSERT_FALSE(ntrip_failure_unreached(&k_auth));

    ntrip_sm_t sm;
    uint32_t t = UINT32_MAX - 100000;   /* across the clock wrap */
    ntrip_sm_init(&sm, &k_std, 1, t);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t));
    t += ntrip_sm_failed(&sm, &k_dns, t, MID);
    uint32_t start = t - 2000;
    t += ntrip_sm_failed(&sm, &k_transient, t, MID);
    t += ntrip_sm_failed(&sm, &k_dns, t, MID);
    TEST_ASSERT_EQUAL_UINT32(t - start, ntrip_sm_unreached_ms(&sm, t));
    TEST_ASSERT_TRUE(ntrip_sm_unreached_ms(&sm, t) >= 2000 + 4000 + 8000);

    /* a refused connect reached the caster's host: the run ends */
    ntrip_sm_failed(&sm, &k_refused, t, MID);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 5000));

    /* a new run; a failure after the TCP connection ends it too */
    ntrip_sm_failed(&sm, &k_dns, t + 6000, MID);
    TEST_ASSERT_EQUAL_UINT32(4000, ntrip_sm_unreached_ms(&sm, t + 10000));
    ntrip_sm_failed(&sm, &k_reached, t + 10000, MID);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 10000));

    /* and a success */
    ntrip_sm_failed(&sm, &k_transient, t + 20000, MID);
    TEST_ASSERT_TRUE(ntrip_sm_unreached_ms(&sm, t + 21000) > 0);
    ntrip_sm_connected(&sm, t + 21000);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 22000));
}

TEST_CASE("ntrip unreached run ends when the slot leaves its connect cycle", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 1000;
    ntrip_sm_in_t in = ready();
    ntrip_close_why_t why;

    /* waiting for data (the receiver stopped) */
    ntrip_sm_init(&sm, &k_std, 1, t);
    sm.prev_link_up = true;
    ntrip_sm_failed(&sm, &k_dns, t, MID);
    in.fresh_data = false;
    (void)ntrip_sm_poll(&sm, &in, t + 5000, MID, &why);
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_DATA, sm.state);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 5000));

    /* the network went down */
    ntrip_sm_init(&sm, &k_std, 1, t);
    ntrip_sm_failed(&sm, &k_dns, t, MID);
    in = ready();
    in.have_ip = false;
    in.link_up = false;
    (void)ntrip_sm_poll(&sm, &in, t + 100, MID, &why);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 100));

    /* paused, disabled, or edited by the user */
    ntrip_sm_init(&sm, &k_std, 1, t);
    ntrip_sm_failed(&sm, &k_dns, t, MID);
    in = ready();
    in.paused = true;
    (void)ntrip_sm_poll(&sm, &in, t + 100, MID, &why);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 100));
    ntrip_sm_failed(&sm, &k_dns, t + 200, MID);
    in = ready();
    in.enabled = false;
    (void)ntrip_sm_poll(&sm, &in, t + 300, MID, &why);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 300));
    ntrip_sm_failed(&sm, &k_dns, t + 400, MID);
    ntrip_sm_user_edit(&sm, &k_std, 2, t + 500);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_unreached_ms(&sm, t + 500));

    /* still in backoff with fresh data: the run goes on */
    ntrip_sm_init(&sm, &k_std, 1, t);
    sm.prev_link_up = true;
    ntrip_sm_failed(&sm, &k_dns, t, MID);
    in = ready();
    (void)ntrip_sm_poll(&sm, &in, t + 1000, MID, &why);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL_UINT32(1000, ntrip_sm_unreached_ms(&sm, t + 1000));
}

TEST_CASE("ntrip protected slot: unreached failures retry flat at 5 s without advancing the count", "[ntrip]")
{
    ntrip_sm_t sm;
    uint32_t t = 0;
    ntrip_sm_init(&sm, &k_fast, 1, t);
    for (int i = 0; i < 20; i++) {
        uint32_t d = ntrip_sm_failed(&sm, (i & 1) ? &k_dns : &k_transient, t, MID);
        TEST_ASSERT_EQUAL_UINT32(NTRIP_UNREACHED_RETRY_MS, d);
        TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
        t += d;
    }
    TEST_ASSERT_EQUAL_UINT32(0, sm.attempt);
    TEST_ASSERT_EQUAL_UINT32(NTRIP_UNREACHED_RETRY_MS * 8u / 10u, ntrip_sm_failed(&sm, &k_dns, t, LO));
    TEST_ASSERT_EQUAL_UINT32(NTRIP_UNREACHED_RETRY_MS * 12u / 10u, ntrip_sm_failed(&sm, &k_dns, t, HI));
    TEST_ASSERT_EQUAL_UINT32(0, sm.attempt);

    /* a refused connect and a reached failure follow the row (1 s, then 2 s) */
    TEST_ASSERT_EQUAL_UINT32(1000, ntrip_sm_failed(&sm, &k_refused, t, MID));
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &k_reached, t + 1000, MID));
    TEST_ASSERT_EQUAL_UINT32(2, sm.attempt);
    /* back to an unreached failure: flat again, the count stays */
    TEST_ASSERT_EQUAL_UINT32(NTRIP_UNREACHED_RETRY_MS, ntrip_sm_failed(&sm, &k_dns, t + 3000, MID));
    TEST_ASSERT_EQUAL_UINT32(2, sm.attempt);

    /* other slots keep the row for every failure */
    ntrip_sm_init(&sm, &k_std, 1, 0);
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &k_dns, 0, MID));
    TEST_ASSERT_EQUAL_UINT32(4000, ntrip_sm_failed(&sm, &k_dns, 2000, MID));
    TEST_ASSERT_EQUAL_UINT32(2, sm.attempt);
}

/* ---- connection history ---------------------------------------------------------------- */

TEST_CASE("ntrip history: UP after failures or a gap, DOWN with class and length", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;

    /* first attempt of the boot succeeds: no UP */
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 2, 100, &a, &b));
    /* a long session ends with a failure: DOWN always */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 2, NTRIP_CLS_REMOTE_CLOSE, 400, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2 | (NTRIP_CLS_REMOTE_CLOSE << 8), a);
    TEST_ASSERT_EQUAL_INT32(300, b);
    /* three failed attempts, then streaming again: UP with the count and the outage */
    ntrip_hist_failed(&h, 401);
    ntrip_hist_failed(&h, 405);
    ntrip_hist_failed(&h, 415);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 2, 430, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2 | (3 << 16), a);
    TEST_ASSERT_EQUAL_INT32(30, b);
    /* a planned close (class 0), back 2 s later without a failure: its DOWN was written, so the UP is too */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 2, 0, 1000, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2, a);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 2, 1002, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2, a);
    TEST_ASSERT_EQUAL_INT32(2, b);
    /* the same at once (0 s), and after a failure-free 10 s gap */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 2, 0, 2000, &a, &b));
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 2, 2000, &a, &b));
    TEST_ASSERT_EQUAL_INT32(0, b);
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 2, 0, 3000, &a, &b));
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 2, 3010, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2, a);
    TEST_ASSERT_EQUAL_INT32(10, b);
    /* a second UP without a DOWN between them is not another return (within the 10 s gap) */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 2, 0, 4000, &a, &b));
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 2, 4001, &a, &b));
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 2, 4005, &a, &b));

    /* not streamed this boot: the outage counts from the first failed attempt */
    ntrip_hist_init(&h);
    ntrip_hist_failed(&h, 50);
    ntrip_hist_failed(&h, 60);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 0, 80, &a, &b));
    TEST_ASSERT_EQUAL_INT32(2 << 16, a);
    TEST_ASSERT_EQUAL_INT32(30, b);

    /* a DOWN without a session is nothing */
    ntrip_hist_init(&h);
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 0, 0, 10, &a, &b));

    /* the packing matches the journal's */
    TEST_ASSERT_EQUAL_INT32(diag_caster_a(7, NTRIP_CLS_KEEPALIVE, 40000), ntrip_hist_pack(7, NTRIP_CLS_KEEPALIVE, 40000));
    TEST_ASSERT_EQUAL_INT32(7 | (NTRIP_CLS_KEEPALIVE << 8) | (32767 << 16),
                            ntrip_hist_pack(7, NTRIP_CLS_KEEPALIVE, 40000));
}

TEST_CASE("ntrip history: short sessions are written once per 10 min, the skipped ones counted", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    uint32_t t = 1000;
    unsigned downs = 0;
    unsigned ups = 0;
    int32_t last_down_a = 0;
    /* a session of 20 s every 40 s, for 30 min: one failed attempt in each gap */
    for (int i = 0; i < 45; i++) {
        if (ntrip_hist_up(&h, 1, t, &a, &b)) {
            ups++;
        }
        t += 20;
        if (ntrip_hist_down(&h, 1, NTRIP_CLS_WRITE_STALL, t, &a, &b)) {
            downs++;
            last_down_a = a;
            TEST_ASSERT_EQUAL_INT32(20, b);
        }
        ntrip_hist_failed(&h, t + 5);
        t += 20;
    }
    /* 45 short sessions over 30 min: one DOWN per 10 min (the first, then at 600 s and 1200 s) */
    TEST_ASSERT_EQUAL_UINT32(3, downs);
    /* the third DOWN carries the 14 skipped since the second */
    TEST_ASSERT_EQUAL_INT32(1 | (NTRIP_CLS_WRITE_STALL << 8) | (14 << 16), last_down_a);
    /* the UP after each DOWN written (at 1040, 1640 and 2240); the others fall within 10 min of one of them */
    TEST_ASSERT_EQUAL_UINT32(3, ups);

    /* still after a short session, within 10 min of the last UP: not written */
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 1, t, &a, &b));
    /* a long session's DOWN and the UP after it are always written */
    t += 120;
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 1, NTRIP_CLS_REMOTE_CLOSE, t, &a, &b));
    TEST_ASSERT_EQUAL_INT32(1 | (NTRIP_CLS_REMOTE_CLOSE << 8) | (14 << 16), a);   /* the 14 skipped since the third */
    TEST_ASSERT_EQUAL_INT32(120, b);
    ntrip_hist_failed(&h, t + 1);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 1, t + 5, &a, &b));
}

TEST_CASE("ntrip history: a short session's DOWN that was written gets its UP inside the UP limit", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    (void)ntrip_hist_up(&h, 3, 100, &a, &b);
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1000, &a, &b));   /* long */
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 3, 1001, &a, &b));
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1021, &a, &b));  /* short, DOWN 21 s ago */
    ntrip_hist_failed(&h, 1100);
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 3, 1200, &a, &b));    /* its DOWN skipped, UP 199 s ago: limited */
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1210, &a, &b));  /* short, DOWN 210 s ago */
    ntrip_hist_failed(&h, 1700);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 3, 1801, &a, &b));     /* its DOWN skipped, UP 800 s ago: written */
    /* a short session whose DOWN is written (none for 821 s), back 3 s later: the UP 23 s after the last one is written */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 3, NTRIP_CLS_WRITE_STALL, 1821, &a, &b));
    TEST_ASSERT_EQUAL_INT32(3 | (NTRIP_CLS_WRITE_STALL << 8) | (2 << 16), a);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 3, 1824, &a, &b));
    TEST_ASSERT_EQUAL_INT32(3, a);
    TEST_ASSERT_EQUAL_INT32(3, b);
}

TEST_CASE("ntrip history: after a DOWN not written, an UP after a failure or the 10 s gap, once per 10 min", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    (void)ntrip_hist_up(&h, 3, 100, &a, &b);
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1000, &a, &b));   /* long */
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 3, 1001, &a, &b));
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1021, &a, &b));  /* short, skipped */
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 3, 1025, &a, &b));    /* 4 s, no failure: nothing to say */
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1045, &a, &b));  /* short, skipped */
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 3, 1060, &a, &b));    /* the 15 s gap, but an UP 59 s ago */
    TEST_ASSERT_FALSE(ntrip_hist_down(&h, 3, NTRIP_CLS_REMOTE_CLOSE, 1080, &a, &b));  /* short, skipped */
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 3, 1700, &a, &b));     /* the gap, the last UP 699 s ago */
    TEST_ASSERT_EQUAL_INT32(3, a);
    TEST_ASSERT_EQUAL_INT32(620, b);
}

TEST_CASE("ntrip history: new settings are not an outage, unless an attempt after them fails", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    (void)ntrip_hist_up(&h, 4, 100, &a, &b);
    /* saved settings: no DOWN; the reconnect at once, or after a gap (the slot was off), writes no UP */
    ntrip_hist_new_settings(&h, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_hist_session_s(&h, 1001));
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 4, 1002, &a, &b));
    ntrip_hist_new_settings(&h, 2000);
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 4, 9000, &a, &b));
    /* new settings that fail first (a wrong password): the UP says the outage from the save */
    ntrip_hist_new_settings(&h, 10000);
    ntrip_hist_failed(&h, 10001);
    ntrip_hist_failed(&h, 10300);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 10900, &a, &b));
    TEST_ASSERT_EQUAL_INT32(4 | (2 << 16), a);
    TEST_ASSERT_EQUAL_INT32(900, b);
    /* and the next drop is an ordinary one again: its DOWN, then its UP at once */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 4, NTRIP_CLS_TIMEOUT, 12000, &a, &b));
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 12001, &a, &b));
    TEST_ASSERT_EQUAL_INT32(1, b);
    /* outside a session it changes nothing */
    ntrip_hist_init(&h);
    ntrip_hist_new_settings(&h, 50);
    TEST_ASSERT_FALSE(ntrip_hist_up(&h, 4, 100, &a, &b));
}

TEST_CASE("ntrip history: new settings, a failure: UP past the flap limit; on again: outage from then", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    (void)ntrip_hist_up(&h, 4, 100, &a, &b);
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 4, NTRIP_CLS_REMOTE_CLOSE, 1000, &a, &b));
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 1001, &a, &b));
    /* a 29 s session ends for new settings that then fail once: its UP 199 s after the last one is written */
    ntrip_hist_new_settings(&h, 1030);
    ntrip_hist_failed(&h, 1031);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 1200, &a, &b));
    TEST_ASSERT_EQUAL_INT32(4 | (1 << 16), a);
    TEST_ASSERT_EQUAL_INT32(170, b);
    /* turned off by a save, on again a day later, the first attempt fails: the outage counts from turning on */
    ntrip_hist_new_settings(&h, 2000);
    ntrip_hist_new_settings(&h, 90000);
    ntrip_hist_failed(&h, 90010);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 90030, &a, &b));
    TEST_ASSERT_EQUAL_INT32(30, b);
    /* new settings that fail, saved again to fix them: the outage still counts from the first save */
    ntrip_hist_new_settings(&h, 95000);
    ntrip_hist_failed(&h, 95001);
    ntrip_hist_new_settings(&h, 95100);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 95200, &a, &b));
    TEST_ASSERT_EQUAL_INT32(4 | (1 << 16), a);
    TEST_ASSERT_EQUAL_INT32(200, b);
    /* after an ordinary drop, a save outside the session leaves the outage at the drop */
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 4, NTRIP_CLS_TIMEOUT, 96000, &a, &b));
    ntrip_hist_failed(&h, 96001);
    ntrip_hist_new_settings(&h, 96100);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 4, 96110, &a, &b));
    TEST_ASSERT_EQUAL_INT32(110, b);
}

TEST_CASE("ntrip history: the session length and the outage for the log", "[ntrip]")
{
    ntrip_hist_t h;
    ntrip_hist_init(&h);
    int32_t a = 0;
    int32_t b = 0;
    uint32_t outage = 99;
    uint32_t fails = 99;
    /* the boot's first attempt works: nothing to tell */
    TEST_ASSERT_FALSE(ntrip_hist_outage(&h, 100, &outage, &fails));
    TEST_ASSERT_EQUAL_UINT32(0, outage);
    TEST_ASSERT_EQUAL_UINT32(0, fails);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_hist_session_s(&h, 100));
    (void)ntrip_hist_up(&h, 0, 100, &a, &b);
    TEST_ASSERT_EQUAL_UINT32(250, ntrip_hist_session_s(&h, 350));
    TEST_ASSERT_TRUE(ntrip_hist_down(&h, 0, NTRIP_CLS_TIMEOUT, 400, &a, &b));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_hist_session_s(&h, 401));
    /* after a session: from its end, with the failed attempts since */
    ntrip_hist_failed(&h, 405);
    ntrip_hist_failed(&h, 410);
    TEST_ASSERT_TRUE(ntrip_hist_outage(&h, 430, &outage, &fails));
    TEST_ASSERT_EQUAL_UINT32(30, outage);
    TEST_ASSERT_EQUAL_UINT32(2, fails);
    TEST_ASSERT_TRUE(ntrip_hist_up(&h, 0, 430, &a, &b));
    TEST_ASSERT_EQUAL_INT32(30, b);

    /* not streamed this boot yet: from the first failed attempt */
    ntrip_hist_init(&h);
    ntrip_hist_failed(&h, 50);
    TEST_ASSERT_TRUE(ntrip_hist_outage(&h, 80, &outage, &fails));
    TEST_ASSERT_EQUAL_UINT32(30, outage);
    TEST_ASSERT_EQUAL_UINT32(1, fails);
}

/* ---- Onocoy: what its console gives (mountpoint, username, password) --------------------- */

TEST_CASE("ntrip Onocoy wire names: the console's mountpoint, username and password", "[ntrip]")
{
    ntrip_wire_names_t w;
    /* NTRIP 2.0: POST /<mountpoint>, Basic <username>:<password>. */
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "MP_42", "user.7", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("MP_42", w.mount);
    TEST_ASSERT_EQUAL_STRING("user.7", w.user);
    /* NTRIP 1.0: SOURCE <password> /<username>. */
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), false, "MP_42", "user.7", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("user.7", w.mount);
    TEST_ASSERT_EQUAL_STRING("", w.user);

    char buf[NTRIP_REQUEST_MAX];
    size_t n = ntrip_build_v1_request(buf, sizeof(buf), "s3cret", w.mount, "NTRIP onodiy/0.3.0");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("SOURCE s3cret /user.7\r\nSource-Agent: NTRIP onodiy/0.3.0\r\n\r\n", buf);

    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "MP_42", "user.7", "", &w);
    n = ntrip_build_v2_request(buf, sizeof(buf), "servers.onocoy.com", 2101, false, w.mount, w.user, "s3cret",
                               "NTRIP onodiy/0.3.0", false);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "POST /MP_42 HTTP/1.1\r\n"));
    /* base64("user.7:s3cret") */
    TEST_ASSERT_NOT_NULL(strstr(buf, "Authorization: Basic dXNlci43OnMzY3JldA==\r\n"));
}

TEST_CASE("ntrip Onocoy wire names: a slot stored before the username field keeps its mapping", "[ntrip]")
{
    ntrip_wire_names_t w;
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), false, "credUser", "", "label", &w);
    TEST_ASSERT_EQUAL_STRING("credUser", w.mount);
    TEST_ASSERT_EQUAL_STRING("", w.user);
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "credUser", "", "Roof1", &w);
    TEST_ASSERT_EQUAL_STRING("Roof1", w.mount);
    TEST_ASSERT_EQUAL_STRING("credUser", w.user);
    ntrip_wire_names(ntrip_host_is_onocoy("servers.onocoy.com"), true, "credUser", "", "", &w);
    TEST_ASSERT_EQUAL_STRING("onodiy", w.mount);
    TEST_ASSERT_EQUAL_STRING("credUser", w.user);
    /* Other casters: as stored, whatever the username. */
    ntrip_wire_names(ntrip_host_is_onocoy("rtk2go.com"), false, "MOUNT", "bob", "", &w);
    TEST_ASSERT_EQUAL_STRING("MOUNT", w.mount);
}

TEST_CASE("ntrip Onocoy fields: username like a mountpoint, password without spaces", "[ntrip]")
{
    ntrip_fields_t f = fields("servers.onocoy.com", "MP_42", "user.7", "s3cret", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    f.v2 = false;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));

    f = fields("servers.onocoy.com", "MP_42", "user:7", "s3cret", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_USERNAME, ntrip_validate_fields(&f));
    f = fields("servers.onocoy.com", "MP_42", "user 7", "s3cret", true);   /* 1.0 sends it as the mountpoint */
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_USERNAME, ntrip_validate_fields(&f));
    f = fields("servers.onocoy.com", "MP_42", "user.7", "two words", true);   /* also over 2.0 */
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_PASSWORD, ntrip_validate_fields(&f));
    f = fields("servers.onocoy.com", "MP_42", "user.7", "", true);   /* an enabled Onocoy slot needs it */
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_MISSING, ntrip_validate_fields(&f));
    f.enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
    /* Other casters keep their rules: a ':'-free username, spaces in a 2.0 password. */
    f = fields("caster.example.com", "M", "user 7", "two words", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));
}

TEST_CASE("ntrip Onocoy day figures: none for a slot that does not count", "[ntrip]")
{
    ntrip_day_in_t in = { .counted = false, .online_s = 3600, .offline_s = 10, .epochs_sent_today = 3600,
                          .day_known = true, .unplanned_offline_s = 10, .elapsed_s = 3610 };
    ntrip_day_out_t out;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_FALSE(out.valid);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_FALSE(out.so_far_valid);
    TEST_ASSERT_FALSE(out.streak_ok);

    /* Counted, nothing sent yet today: no data rate, the rest known. */
    in = (ntrip_day_in_t){ .counted = true, .online_s = 0, .offline_s = 20, .epochs_sent_today = 0,
                           .day_known = true, .unplanned_offline_s = 20, .elapsed_s = 20 };
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_FALSE(out.rate_valid);
    TEST_ASSERT_TRUE(out.so_far_valid);
    TEST_ASSERT_TRUE(out.streak_ok);
    TEST_ASSERT_EQUAL_UINT32(473 - 20, out.budget_left_s);
    TEST_ASSERT_EQUAL_UINT32(473, out.budget_s);             /* the day's allowance */

    /* Without a trusted clock the day so far is unknown. */
    in.day_known = false;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_FALSE(out.so_far_valid);
}

TEST_CASE("ntrip Onocoy day figures: the budget and the streak use the best rate the day can still reach", "[ntrip]")
{
    /* Lost epochs: seconds connected (one epoch each) minus epochs sent. */
    TEST_ASSERT_EQUAL_UINT32(1, ntrip_lost_epochs(34, 33));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_lost_epochs(34, 35));   /* epochs that caught up */
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_lost_epochs(0, 0));

    /* The best rate: 1 - lost / (86400 - offline so far), in ppm, rounded down. */
    TEST_ASSERT_EQUAL_UINT32(1000000, ntrip_best_rate_ppm(0, 0));
    TEST_ASSERT_EQUAL_UINT32(999988, ntrip_best_rate_ppm(1, 0));
    TEST_ASSERT_EQUAL_UINT32(999004, ntrip_best_rate_ppm(86, 0));
    TEST_ASSERT_EQUAL_UINT32(999698, ntrip_best_rate_ppm(26, 26));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_best_rate_ppm(86400, 0));   /* the losses fill the day */
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_best_rate_ppm(10, 86395));
    TEST_ASSERT_EQUAL_UINT32(1000000, ntrip_best_rate_ppm(0, 86400));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_best_rate_ppm(1, 86400));

    /* The ppm budget agrees with the per-mille one at whole per mille. */
    TEST_ASSERT_EQUAL_UINT32(ntrip_offline_budget_s(999), ntrip_offline_budget_ppm_s(999000));
    TEST_ASSERT_EQUAL_UINT32(473, ntrip_offline_budget_ppm_s(999988));   /* one lost epoch costs nothing */
    TEST_ASSERT_EQUAL_UINT32(465, ntrip_offline_budget_ppm_s(999004));
    TEST_ASSERT_EQUAL_UINT32(300, ntrip_offline_budget_ppm_s(980000));
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_offline_budget_ppm_s(979999));
    TEST_ASSERT_EQUAL_UINT32(473, ntrip_offline_budget_ppm_s(2000000));

    /* 34 s after a start, one epoch lost (this boot's rate 97.1 %): the old
     * rule gave a budget of 0 and "missed" with any offline second. */
    ntrip_day_in_t in = { .counted = true, .online_s = 34, .offline_s = 20, .rate_online_s = 34,
                          .epochs_sent_today = 33, .lost_epochs = 1 };
    ntrip_day_out_t out;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT16(970, out.data_rate_permille);   /* the shown rate is still this boot's */
    TEST_ASSERT_EQUAL_UINT32(999988, out.best_rate_ppm);
    TEST_ASSERT_EQUAL_UINT32(473, out.budget_s);
    TEST_ASSERT_EQUAL_UINT32(453, out.budget_left_s);
    TEST_ASSERT_TRUE(out.streak_ok);

    /* The stall the screenshots showed: 101 s connected, of them 26 s of a
     * receiver stall with the session left open. The station has the RTCM as
     * fresh for the first 10 s of it (until the last frame is 10 s old), so
     * the ledger counts those as online and they are 10 lost epochs; the 16 s
     * after are offline, and the data rate's time base leaves them out too
     * (ntrip_rate_clock_t): 85 s online, 75 epochs. Counted as lost epochs as
     * well, the 26 s would charge the budget twice. */
    in = (ntrip_day_in_t){ .counted = true, .online_s = 85, .offline_s = 16, .rate_online_s = 85,
                           .epochs_sent_today = 75, .lost_epochs = 10 };
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT16(882, out.data_rate_permille);
    TEST_ASSERT_EQUAL_UINT32(999884, out.best_rate_ppm);
    TEST_ASSERT_EQUAL_UINT32(472, out.budget_s);
    TEST_ASSERT_EQUAL_UINT32(472 - 16, out.budget_left_s);
    TEST_ASSERT_TRUE(out.streak_ok);

    /* Losses from earlier boots count (the ledger keeps them): 86 lost cost about 8 s. */
    in = (ntrip_day_in_t){ .counted = true, .online_s = 3600, .offline_s = 0, .rate_online_s = 600,
                           .epochs_sent_today = 600, .lost_epochs = 86 };
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT16(1000, out.data_rate_permille);
    TEST_ASSERT_EQUAL_UINT32(465, out.budget_s);
    TEST_ASSERT_TRUE(out.streak_ok);

    /* Over the budget: missed. */
    in.offline_s = 466;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT32(0, out.budget_left_s);
    TEST_ASSERT_FALSE(out.streak_ok);

    /* So many lost that even a perfect rest of the day stays under 98 %:
     * missed without a single offline second. */
    in = (ntrip_day_in_t){ .counted = true, .online_s = 3000, .offline_s = 0, .rate_online_s = 3000,
                           .epochs_sent_today = 1000, .lost_epochs = 2000 };
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT32(976851, out.best_rate_ppm);
    TEST_ASSERT_EQUAL_UINT32(0, out.budget_s);
    TEST_ASSERT_FALSE(out.streak_ok);
}

/* The slot task's passes: one tick every `step_ms` over [*t, *t + span_ms) in state `in`. */
static void rate_run(ntrip_rate_clock_t *c, uint32_t *t, uint32_t span_ms, uint32_t step_ms, const ntrip_rate_in_t *in)
{
    for (uint32_t end = *t + span_ms; *t != end;) {
        *t += step_ms;
        ntrip_rate_clock_tick(c, *t, in);
    }
}

TEST_CASE("ntrip data rate time base: online as the ledger counts it through a stall and a pause", "[ntrip]")
{
    ntrip_rate_clock_t c;
    uint32_t t = 1000;
    ntrip_rate_clock_init(&c, t);
    ntrip_rate_in_t on = { .enabled = true, .session_open = true, .streaming = true, .rtcm_ok = true };

    /* The pass that ends with the caster's OK began offline: none of it. Then
     * 75 s streaming fresh RTCM: all of it online. */
    t += 100;
    ntrip_rate_clock_tick(&c, t, &on);
    TEST_ASSERT_EQUAL_UINT64(0, c.on_ms);
    rate_run(&c, &t, 75000, 100, &on);
    TEST_ASSERT_EQUAL_UINT64(75000, c.on_ms);

    /* The receiver stalls for 26 s; the session stays open (up to the stall
     * limit) in STREAMING. The station has the RTCM as fresh for 10 s more
     * (the ledger's rtcm_ok goes false once the last frame is 10 s old):
     * online in the ledger and here, so those seconds are lost epochs. The
     * 16 s after are offline in both; the pass in which it changed goes by
     * the state it began in. */
    rate_run(&c, &t, 10000, 100, &on);
    ntrip_rate_in_t stale = on;
    stale.rtcm_ok = false;
    rate_run(&c, &t, 16000, 100, &stale);
    TEST_ASSERT_EQUAL_UINT64(85100, c.on_ms);
    TEST_ASSERT_EQUAL_UINT32(10, ntrip_lost_epochs((uint32_t)(c.on_ms / 1000u), 75));

    /* A pause for receiver work with the hub held: the session stays open,
     * the state machine says PAUSED (the ledger: down, planned). */
    ntrip_rate_in_t paused = on;
    paused.streaming = false;
    rate_run(&c, &t, 40000, 100, &paused);
    TEST_ASSERT_EQUAL_UINT64(85100, c.on_ms);

    /* Disabled, or the caster client stopped: not online either. */
    ntrip_rate_in_t off = on;
    off.enabled = false;
    rate_run(&c, &t, 5000, 100, &off);
    TEST_ASSERT_EQUAL_UINT64(85100, c.on_ms);

    /* Streaming fresh RTCM again: counted from the pass after the one that
     * sees it, as the ledger's online. 10 s more and 10 epochs: the stall's
     * 10 lost epochs stay, none more. */
    rate_run(&c, &t, 10000, 100, &on);
    TEST_ASSERT_EQUAL_UINT64(95000, c.on_ms);
    TEST_ASSERT_EQUAL_UINT32(10, ntrip_lost_epochs((uint32_t)(c.on_ms / 1000u), 85));
}

TEST_CASE("ntrip data rate time base: a connect is offline, a stalled write that closes the session online", "[ntrip]")
{
    ntrip_rate_clock_t c;
    uint32_t t = 50000;
    ntrip_rate_clock_init(&c, t);
    ntrip_rate_in_t idle = { .enabled = true };
    rate_run(&c, &t, 2000, 200, &idle);
    TEST_ASSERT_EQUAL_UINT64(0, c.on_ms);

    /* One pass blocks 6.1 s in the connect (the TLS gate, DNS, TCP, TLS, the
     * reply) and ends in STREAMING: it began offline, and the ledger turns
     * online only at its end too, so none of it counts. */
    ntrip_rate_in_t on = { .enabled = true, .session_open = true, .streaming = true, .rtcm_ok = true };
    t += 6100;
    ntrip_rate_clock_tick(&c, t, &on);
    TEST_ASSERT_EQUAL_UINT64(0, c.on_ms);
    rate_run(&c, &t, 30000, 100, &on);
    TEST_ASSERT_EQUAL_UINT64(30000, c.on_ms);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_lost_epochs((uint32_t)(c.on_ms / 1000u), 30));

    /* A write stalls for 10 s and the pass ends with the session closed. The
     * ledger learns of the close at the end of that pass, so it has the 10 s
     * online, and so does this clock: the epochs not sent meanwhile are lost
     * epochs, as many as the ledger's online seconds say. */
    t += 10000;
    ntrip_rate_clock_tick(&c, t, &idle);
    TEST_ASSERT_EQUAL_UINT64(40000, c.on_ms);
    TEST_ASSERT_EQUAL_UINT32(10, ntrip_lost_epochs((uint32_t)(c.on_ms / 1000u), 30));
    /* The passes after it began offline: nothing more. */
    rate_run(&c, &t, 5000, 100, &idle);
    TEST_ASSERT_EQUAL_UINT64(40000, c.on_ms);

    /* Across the wrap of the millisecond clock: a pass counts whole. */
    ntrip_rate_clock_init(&c, 0xFFFFFE00u);
    ntrip_rate_clock_tick(&c, 0xFFFFFF00u, &on);
    TEST_ASSERT_EQUAL_UINT64(0, c.on_ms);
    ntrip_rate_clock_tick(&c, 0x00000100u, &on);
    TEST_ASSERT_EQUAL_UINT64(0x200, c.on_ms);
}

TEST_CASE("ntrip Onocoy day figures: graced uptime so far", "[ntrip]")
{
    /* Enabled at 18:00 UTC and online since (two hours later). */
    ntrip_day_in_t in = { .counted = true, .online_s = 7200, .offline_s = 0, .epochs_sent_today = 7200,
                          .day_known = true, .unplanned_offline_s = 0, .elapsed_s = 7200 };
    ntrip_day_out_t out;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_TRUE(out.so_far_valid);
    TEST_ASSERT_TRUE(out.graced_so_far_permille >= 999);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT16(1000, out.data_rate_permille);
    TEST_ASSERT_TRUE(out.streak_ok);
    /* The day fraction (diagnostics) is still the whole-day figure. */
    TEST_ASSERT_EQUAL_UINT16((7200 + 300) * 1000 / 86400, out.graced_uptime_permille);

    /* 400 s offline after the 300 s grace, 6 h into the day: about 995 per mille. */
    TEST_ASSERT_UINT16_WITHIN(1, 995, ntrip_graced_so_far_permille(400, 6 * 3600));
    /* Inside the grace, or nothing elapsed yet: all of it. */
    TEST_ASSERT_EQUAL_UINT16(1000, ntrip_graced_so_far_permille(300, 3600));
    TEST_ASSERT_EQUAL_UINT16(1000, ntrip_graced_so_far_permille(5000, 0));
    /* Never below zero. */
    TEST_ASSERT_EQUAL_UINT16(0, ntrip_graced_so_far_permille(100000, 3600));
}

TEST_CASE("ntrip Onocoy day figures: the day from the ledger, the rate from this boot", "[ntrip]")
{
    /* After a restart: the ledger holds the whole day so far (10 h online,
     * 600 s offline, and this boot's 18 lost epochs); this boot's own
     * counters only its last hour, at 99.5 %. The rate shown is this boot's;
     * the budget is the day's, from every epoch it lost. */
    ntrip_day_in_t in = { .counted = true, .online_s = 36000, .offline_s = 600, .rate_online_s = 3600,
                          .epochs_sent_today = 3582, .lost_epochs = 18, .day_known = true,
                          .unplanned_offline_s = 600, .elapsed_s = 36600 };
    ntrip_day_out_t out;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_TRUE(out.rate_valid);
    TEST_ASSERT_EQUAL_UINT16(995, out.data_rate_permille);   /* epochs over the time online they cover */
    TEST_ASSERT_EQUAL_UINT32(471, out.budget_s);             /* 18 lost of the day's 85800 still expected */
    TEST_ASSERT_EQUAL_UINT32(0, out.budget_left_s);          /* used up: 600 s offline */
    TEST_ASSERT_FALSE(out.streak_ok);
    TEST_ASSERT_EQUAL_UINT16((36000 + 300) * 1000 / 86400, out.graced_uptime_permille);

    /* Earlier boots lost 430 more: the day's allowance shrinks, whatever this boot's rate. */
    in.lost_epochs = 448;
    in.offline_s = 400;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT32(428, out.budget_s);
    TEST_ASSERT_EQUAL_UINT32(28, out.budget_left_s);
    TEST_ASSERT_TRUE(out.streak_ok);

    /* Nothing lost all day: 473 s allowed, 400 s used. */
    in.lost_epochs = 0;
    ntrip_day_metrics(&in, &out);
    TEST_ASSERT_EQUAL_UINT32(473, out.budget_s);
    TEST_ASSERT_EQUAL_UINT32(73, out.budget_left_s);
    TEST_ASSERT_TRUE(out.streak_ok);
}

/* ---- RTK2go mountpoint rules ----------------------------------------------------------------- */

TEST_CASE("ntrip RTK2go mountpoint rules for a new name", "[ntrip]")
{
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_BAD_CHARS, ntrip_rtk2go_mount_check("B.ase"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_BAD_CHARS, ntrip_rtk2go_mount_check("Be nch"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_BAD_CHARS, ntrip_rtk2go_mount_check("A.b"));   /* the characters first */
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_FIRST_NOT_LETTER, ntrip_rtk2go_mount_check("1abc"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_FIRST_NOT_LETTER, ntrip_rtk2go_mount_check("_abc"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_FIRST_A, ntrip_rtk2go_mount_check("Abcd"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_rtk2go_mount_check("abcd"));   /* only the uppercase letter is named */
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_TOO_SHORT, ntrip_rtk2go_mount_check("Bcd"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_TOO_SHORT, ntrip_rtk2go_mount_check(""));
    const char *len21 = "Bcdefghijklmnopqrstuv";
    const char *len20 = "Bcdefghijklmnopqrstu";
    TEST_ASSERT_EQUAL_size_t(21, strlen(len21));
    TEST_ASSERT_EQUAL_size_t(20, strlen(len20));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_TOO_LONG, ntrip_rtk2go_mount_check(len21));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_rtk2go_mount_check(len20));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_rtk2go_mount_check("Bcde"));
    TEST_ASSERT_EQUAL(NTRIP_RTK2GO_OK, ntrip_rtk2go_mount_check("Bench_01-x"));
}

TEST_CASE("ntrip RTK2go rules apply to a new host or a changed mountpoint only", "[ntrip]")
{
    /* A new slot, or another host becoming RTK2go. */
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies("", "", "rtk2go.com", "BENCH"));
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies(NULL, NULL, "rtk2go.com", "BENCH"));
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies("caster.example.org", "BENCH", "rtk2go.com", "BENCH"));
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies("", "", "www.rtk2go.com", "BENCH"));
    /* The same RTK2go slot: only a changed mountpoint (case-sensitive) is a new name. */
    TEST_ASSERT_FALSE(ntrip_rtk2go_rule_applies("rtk2go.com", "A.base", "rtk2go.com", "A.base"));
    TEST_ASSERT_FALSE(ntrip_rtk2go_rule_applies("rtk2go.com", "A.base", "RTK2go.COM", "A.base"));
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies("rtk2go.com", "A.base", "rtk2go.com", "Abase"));
    TEST_ASSERT_TRUE(ntrip_rtk2go_rule_applies("rtk2go.com", "bench1", "rtk2go.com", "Bench1"));
    /* Not RTK2go: never. */
    TEST_ASSERT_FALSE(ntrip_rtk2go_rule_applies("", "", "caster.example.org", "A.b"));
    TEST_ASSERT_FALSE(ntrip_rtk2go_rule_applies("rtk2go.com", "A.b", "caster.example.org", "A.c"));
    TEST_ASSERT_FALSE(ntrip_rtk2go_rule_applies("", "", "notrtk2go.com", "A.b"));
}

/* ---- Onocoy reason texts ------------------------------------------------------------------- */

TEST_CASE("ntrip Onocoy reasons: each phrase, any case, the first in priority wins", "[ntrip]")
{
    static const struct {
        const char *text;
        ntrip_onocoy_reason_t want;
    } k[] = {
        { "Too many unsuccessful logins - temporarily blocking further connections", NTRIP_ONO_LOGIN_BLOCKED },
        { "Credentials already in use by another stream", NTRIP_ONO_IN_USE },
        { "Maximum Number of parallel connections reached for this credential", NTRIP_ONO_IN_USE },
        { "Device reconnected", NTRIP_ONO_REPLACED },
        { "Duplicate of another stream", NTRIP_ONO_DUPLICATE },
        { "Credential is banned", NTRIP_ONO_BANNED },
        { "Authentication failed", NTRIP_ONO_AUTH },
        { "Reauthentication failed", NTRIP_ONO_AUTH },
        { "Invalid request in headers", NTRIP_ONO_AUTH },
        { "Error 5 Too many unsuccessful logins - temporarily blocking further connections [onocoy naomi v2.5.95-x]",
          NTRIP_ONO_LOGIN_BLOCKED },
        { "ERROR 7 AUTHENTICATION FAILED", NTRIP_ONO_AUTH },
        { "error: device RECONNECTED", NTRIP_ONO_REPLACED },
        /* two phrases: the priority decides, not the position */
        { "Authentication failed: credential is banned", NTRIP_ONO_BANNED },
        { "Device reconnected; credentials already in use", NTRIP_ONO_IN_USE },
        { "Duplicate of another stream (too many unsuccessful logins)", NTRIP_ONO_LOGIN_BLOCKED },
        /* none */
        { "ICY 200 OK", NTRIP_ONO_NONE },
        { "Error 11 Unknown - epoll rd hup [caster v2]", NTRIP_ONO_NONE },
        { "Your IP is banned", NTRIP_ONO_NONE },
        { "Device reconnect", NTRIP_ONO_NONE },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(k[i].want, ntrip_onocoy_reason(k[i].text, strlen(k[i].text)), k[i].text);
    }
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, ntrip_onocoy_reason(NULL, 0));
    TEST_ASSERT_EQUAL(NTRIP_ONO_IN_USE, ntrip_onocoy_reason_merge(NTRIP_ONO_AUTH, NTRIP_ONO_IN_USE));
    TEST_ASSERT_EQUAL(NTRIP_ONO_IN_USE, ntrip_onocoy_reason_merge(NTRIP_ONO_IN_USE, NTRIP_ONO_REPLACED));
    TEST_ASSERT_EQUAL(NTRIP_ONO_AUTH, ntrip_onocoy_reason_merge(NTRIP_ONO_NONE, NTRIP_ONO_AUTH));
    TEST_ASSERT_EQUAL(NTRIP_ONO_AUTH, ntrip_onocoy_reason_merge(NTRIP_ONO_AUTH, NTRIP_ONO_NONE));
}

/* A reply fed in two pieces; body_always as a guard slot sets it. */
static ntrip_verdict_t reply_in_two(const char *a, const char *b, bool body_always, ntrip_reply_t *r)
{
    ntrip_reply_init(r, false);
    r->body_always = body_always;
    size_t used = 0;
    bool done = ntrip_reply_feed(r, (const uint8_t *)a, strlen(a), &used);
    if (!done) {
        done = ntrip_reply_feed(r, (const uint8_t *)b, strlen(b), &used);
    }
    if (!done) {
        ntrip_reply_end(r, true);
    }
    TEST_ASSERT_EQUAL(NTRIP_RP_DONE, r->phase);
    return r->verdict;
}

TEST_CASE("ntrip Onocoy reasons in the reply line and body, across reads and chunks", "[ntrip]")
{
    static ntrip_reply_t r;
    /* The naomi form, split inside the phrase: the row stays the plain-text error row. */
    ntrip_verdict_t v = reply_in_two("Error 5 Too many unsucc",
                                     "essful logins - temporarily blocking further connections [onocoy naomi v2.5.95-x]\r\n",
                                     false, &r);
    TEST_ASSERT_EQUAL_UINT8(12, v.rule);
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, v.row);
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, v.cls);
    TEST_ASSERT_EQUAL(NTRIP_ONO_LOGIN_BLOCKED, v.onocoy_reason);

    /* A plain 401 body. */
    v = reply_in_two("HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\n\r\nAuthentic",
                     "ation failed for this mountpoint", false, &r);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, v.row);
    TEST_ASSERT_EQUAL(NTRIP_ONO_AUTH, v.onocoy_reason);

    /* A chunked 403 body, the phrase across a chunk boundary (and across the reads). */
    v = reply_in_two("HTTP/1.1 403 Forbidden\r\nTransfer-Encoding: chunked\r\n\r\n9\r\nCredentia\r\n",
                     "b\r\nl is banned\r\n0\r\n\r\n", false, &r);
    TEST_ASSERT_EQUAL(NTRIP_ONO_BANNED, v.onocoy_reason);
    TEST_ASSERT_EQUAL(NTRIP_CLS_BANNED, v.cls);   /* the body check's "banned", as before */

    /* A 503: no body check unless the slot asks for it (a guard slot does). */
    v = reply_in_two("HTTP/1.1 503 Service Unavailable\r\n\r\n", "Credentials already in use by another stream", false,
                     &r);
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, v.onocoy_reason);
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, v.row);
    v = reply_in_two("HTTP/1.1 503 Service Unavailable\r\n\r\n", "Credentials already in use by another stream", true,
                     &r);
    TEST_ASSERT_EQUAL(NTRIP_ONO_IN_USE, v.onocoy_reason);
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, v.row);   /* the row is still the generic one: the guard maps it */
    TEST_ASSERT_TRUE(r.body_check);

    /* Line and body name different reasons: the priority decides. */
    v = reply_in_two("HTTP/1.1 401 Authentication failed\r\n\r\n", "Device reconnected", true, &r);
    TEST_ASSERT_EQUAL(NTRIP_ONO_REPLACED, v.onocoy_reason);

    /* A success never reads a body. */
    v = reply_in_two("HTTP/1.1 200 OK\r\n\r\n", "Device reconnected", true, &r);
    TEST_ASSERT_TRUE(v.success);
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, v.onocoy_reason);
}

TEST_CASE("ntrip Onocoy reasons leave the reply rows of other slots unchanged", "[ntrip]")
{
    expect("Error 5 Too many unsuccessful logins - temporarily blocking further connections [onocoy naomi v2.5.95-x]\r\n",
           false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 12);
    expect("Error 3 Credentials already in use by another stream\r\n", false, NTRIP_ROW_TRANSIENT,
           NTRIP_CLS_REMOTE_CLOSE, 12);
    expect("Error 4 Authentication failed\r\n", false, NTRIP_ROW_TRANSIENT, NTRIP_CLS_REMOTE_CLOSE, 12);
    expect("Credential is banned\r\n", false, NTRIP_ROW_BANNED, NTRIP_CLS_BANNED, 15);
    expect("ERROR - Bad Password\r\n", false, NTRIP_ROW_AUTH, NTRIP_CLS_AUTH, 6);

    /* The guard's class for each (failures_by_class and the journal on a guard slot). */
    ntrip_failure_t f = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false, false, true, NTRIP_ONO_IN_USE, 0 };
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, ntrip_guard_class(&f));
    f.onocoy = NTRIP_ONO_LOGIN_BLOCKED;
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOGIN_BLOCKED, ntrip_guard_class(&f));
    f.onocoy = NTRIP_ONO_REPLACED;
    TEST_ASSERT_EQUAL(NTRIP_CLS_REPLACED, ntrip_guard_class(&f));
    f.onocoy = NTRIP_ONO_AUTH;
    TEST_ASSERT_EQUAL(NTRIP_CLS_AUTH, ntrip_guard_class(&f));
    f.onocoy = NTRIP_ONO_DUPLICATE;
    TEST_ASSERT_EQUAL(NTRIP_CLS_BANNED, ntrip_guard_class(&f));
    f.onocoy = NTRIP_ONO_NONE;
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, ntrip_guard_class(&f));
    f.cls = NTRIP_CLS_MOUNT_BUSY;
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, ntrip_guard_class(&f));
}

TEST_CASE("ntrip session text: lines across reads, secrets masked, a stop reason wins", "[ntrip]")
{
    static ntrip_sess_text_t t;
    memset(&t, 0, sizeof(t));
    ntrip_sess_text_start(&t);
    const char *secrets[3] = { "fakepass123", "TESTMOUNT", "testuser01" };
#define FEED(s) ntrip_sess_text_feed(&t, (const uint8_t *)(s), strlen(s), secrets, 3)
    TEST_ASSERT_FALSE(FEED("Device reco"));
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, ntrip_sess_text_reason(&t));
    TEST_ASSERT_TRUE(FEED("nnected (testuser01)\r\n"));
    TEST_ASSERT_EQUAL_STRING("Device reconnected (**********)", t.last);
    TEST_ASSERT_EQUAL(NTRIP_ONO_REPLACED, ntrip_sess_text_reason(&t));
    TEST_ASSERT_FALSE(FEED("\r\n\r\n"));   /* empty lines are no lines */
    TEST_ASSERT_TRUE(FEED("Duplicate of another stream\n"));
    TEST_ASSERT_EQUAL(NTRIP_ONO_DUPLICATE, ntrip_sess_text_reason(&t));   /* it stops the slot: it wins */

    /* A new session: nothing seen yet, the last line still shown. */
    ntrip_sess_text_start(&t);
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, ntrip_sess_text_reason(&t));
    TEST_ASSERT_EQUAL_STRING("Duplicate of another stream", t.last);

    /* The session ends inside a line: it still counts; a generic "banned" is BANNED here. */
    TEST_ASSERT_FALSE(FEED("Your IP is banned"));
    TEST_ASSERT_TRUE(ntrip_sess_text_end(&t, secrets, 3));
    TEST_ASSERT_EQUAL(NTRIP_ONO_BANNED, ntrip_sess_text_reason(&t));
    TEST_ASSERT_FALSE(ntrip_sess_text_end(&t, secrets, 3));

    /* A line longer than the buffer is taken in pieces. */
    ntrip_sess_text_start(&t);
    char longline[NTRIP_SESS_LINE_MAX + 40];
    memset(longline, 'x', sizeof(longline));
    longline[sizeof(longline) - 1] = '\0';
    TEST_ASSERT_TRUE(FEED(longline));
    TEST_ASSERT_TRUE(FEED(" already in use\n"));
    TEST_ASSERT_EQUAL(NTRIP_ONO_IN_USE, ntrip_sess_text_reason(&t));
    TEST_ASSERT_NOT_NULL(strstr(t.last, "x already in use"));
#undef FEED
}

/* ---- Onocoy login guard -------------------------------------------------------------------------- */

static const ntrip_sm_cfg_t k_guard = { NTRIP_PROF_FAST, true, 60000, 300000, 30000, true };

/* A login reply that failed. */
static ntrip_failure_t reply_fail(ntrip_cls_t cls, ntrip_row_t row, ntrip_onocoy_reason_t why)
{
    ntrip_failure_t f = { cls, row, true, false, false, true, (uint8_t)why, 0 };
    return f;
}

/* From a parsed reply, exactly as the server builds it on a guard slot (class already the guard's). */
static ntrip_failure_t reply_of(const char *bytes)
{
    static ntrip_reply_t r;
    ntrip_verdict_t v = run_reply(bytes, false, true, &r);
    return ntrip_reply_failure(&v, true);
}

/* The same on a standard or polite slot (the generic class). */
static ntrip_failure_t std_reply_of(const char *bytes)
{
    static ntrip_reply_t r;
    ntrip_verdict_t v = run_reply(bytes, false, true, &r);
    return ntrip_reply_failure(&v, false);
}

/* A healthy session from t (60 s, 10 kB): the end of the guard's incident. */
static void healthy_session(ntrip_sm_t *sm, uint32_t t)
{
    ntrip_sm_connecting(sm, t);
    ntrip_sm_connected(sm, t);
    ntrip_sm_sent(sm, 10000, t + NTRIP_HEALTHY_SESSION_MS);
}

/* No reply at all (nothing in 10 s, or a close before the line). */
static const ntrip_failure_t k_silent = { NTRIP_CLS_TIMEOUT, NTRIP_ROW_TRANSIENT, true, false, false, false, 0, 0 };
/* A session the caster ended (EOF or a reset while the station kept its address). */
static const ntrip_failure_t k_kicked = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false, false, false, 0,
                                          0 };

static const uint32_t k_rnds[2] = { LO, HI };   /* jitter x 1.0 and x 1.2 */

/* A session from t that lasts len_ms (10 kB sent) and ends with `end`. @return the delay. */
static uint32_t guard_session(ntrip_sm_t *sm, uint32_t t, uint32_t len_ms, const ntrip_failure_t *end, uint32_t rnd)
{
    ntrip_sm_connecting(sm, t);
    ntrip_sm_connected(sm, t);
    ntrip_sm_sent(sm, 10000, t + len_ms);
    return ntrip_sm_failed(sm, end, t + len_ms, rnd);
}

/* Polled once a minute for `hours`: never an attempt, always STOPPED. */
static void assert_stays_stopped(ntrip_sm_t *sm, uint32_t t, uint32_t hours)
{
    ntrip_sm_in_t in = ready();
    sm->prev_link_up = true;
    for (uint32_t m = 0; m <= hours * 60u; m++) {
        TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(sm, &in, t + m * 60000u));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm->state);
    }
}

TEST_CASE("ntrip guard: refused logins wait 300 s, then 1800 s, and the third stops for good", "[ntrip]")
{
    const ntrip_failure_t auth = reply_fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_ONO_NONE);
    static const uint32_t first[2] = { 300000, 360000 };
    static const uint32_t second[2] = { 1800000, 2160000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        sm.prev_link_up = true;
        ntrip_sm_in_t in = ready();
        uint32_t t = 400000;   /* fresh credentials, past the start's window */
        TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t));
        ntrip_sm_connecting(&sm, t);
        uint32_t d = ntrip_sm_failed(&sm, &auth, t, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
        TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
        TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + d - 1));
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + d));
        t += d;
        d = ntrip_sm_failed(&sm, &auth, t, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(second[k], d);
        TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, t + d - 1));
        t += d;
        TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t));
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &auth, t, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
        TEST_ASSERT_EQUAL_UINT8(3, sm.rejects);
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_retry_in_ms(&sm, t));
        assert_stays_stopped(&sm, t, 48);
    }

    /* Onocoy's own text, a CONFIG reply and an "authentication failed" line all count the same. */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    ntrip_failure_t f = reply_of("Error 4 Authentication failed [onocoy naomi v2.5.95-x]\r\n");
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &f, 400000, LO));
    TEST_ASSERT_EQUAL(NTRIP_CLS_AUTH, sm.reason);
    f = reply_of("HTTP/1.1 400 Bad Request\r\n\r\n");
    TEST_ASSERT_EQUAL_UINT32(1800000, ntrip_sm_failed(&sm, &f, 800000, LO));
    f = reply_of("SOURCETABLE 200 OK\r\n");
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, 3000000, LO));
    TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
}

TEST_CASE("ntrip guard: right after a session on working credentials the first refusal waits 30 s", "[ntrip]")
{
    const ntrip_failure_t auth = reply_fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_ONO_NONE);
    static const uint32_t reconnect[2] = { 1000, 1200 };
    static const uint32_t first[2] = { 30000, 36000 };
    static const uint32_t second[2] = { 1800000, 2160000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        /* A caster deploy ends a 200 s session: the fast transient row, no take-over score. */
        uint32_t t = 1000;
        uint32_t d = guard_session(&sm, t, 200000, &k_kicked, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(reconnect[k], d);
        TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
        t += 200000 + d;
        d = ntrip_sm_failed(&sm, &auth, t, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
        t += d;
        d = ntrip_sm_failed(&sm, &auth, t, k_rnds[k]);   /* still inside the window: no second cheap chance */
        TEST_ASSERT_EQUAL_UINT32(second[k], d);
        t += d;
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &auth, t, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
    }

    /* The credentials changed since they worked: no cheap chance. */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t d = guard_session(&sm, 1000, 200000, &k_kicked, LO);
    ntrip_sm_user_edit(&sm, &k_guard, 0x78, 201000 + d);
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &auth, 202000 + d, LO));
}

TEST_CASE("ntrip guard: too many unsuccessful logins waits an hour, then two, then stops", "[ntrip]")
{
    const ntrip_failure_t f =
        reply_of("Error 5 Too many unsuccessful logins - temporarily blocking further connections [onocoy naomi v2.5.95-x]\r\n");
    TEST_ASSERT_EQUAL(NTRIP_ROW_TRANSIENT, f.row);   /* the generic row: a standard slot's */
    TEST_ASSERT_EQUAL(NTRIP_ONO_LOGIN_BLOCKED, f.onocoy);
    static const uint32_t first[2] = { 3600000, 4320000 };
    static const uint32_t second[2] = { 7200000, 8640000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 400000;
        uint32_t d = ntrip_sm_failed(&sm, &f, t, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_CLS_LOGIN_BLOCKED, sm.reason);
        TEST_ASSERT_EQUAL_UINT8(1, sm.blocks);
        TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
        t += d;
        TEST_ASSERT_EQUAL_UINT32(second[k], ntrip_sm_failed(&sm, &f, t, k_rnds[k]));
        t += second[k];
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, t, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_STOP_LOGIN_BLOCKED, sm.stop);
    }

    /* A standard slot meets the same line on its transient row, as before. */
    const ntrip_failure_t fs =
        std_reply_of("Error 5 Too many unsuccessful logins - temporarily blocking further connections [onocoy naomi v2.5.95-x]\r\n");
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_std, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &fs, 400000, MID));
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL_UINT8(0, sm.blocks);
}

TEST_CASE("ntrip guard: a credential in use stops at once, unless right after this station's session", "[ntrip]")
{
    const ntrip_failure_t inuse = reply_of("Error 3 Credentials already in use by another stream\r\n");
    TEST_ASSERT_EQUAL(NTRIP_ONO_IN_USE, inuse.onocoy);

    /* Credentials that never worked: stopped at once, even right after the start. */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &inuse, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);

    /* Outside the window on credentials that worked: stopped too. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t d = guard_session(&sm, 1000, 200000, &k_kicked, MID);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &inuse, 201000 + 300000, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);

    /* Inside the window (this station's own dead session may hold it): 15, 30, 60, 120 s, the 5th stops. */
    static const uint32_t steps[2][4] = { { 15000, 30000, 60000, 120000 }, { 18000, 36000, 72000, 144000 } };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 1000;
        d = guard_session(&sm, t, 200000, &k_kicked, k_rnds[k]);
        t += 200000 + d;
        for (int i = 0; i < 4; i++) {
            d = ntrip_sm_failed(&sm, &inuse, t, k_rnds[k]);
            TEST_ASSERT_EQUAL_UINT32(steps[k][i], d);
            TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
            TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, sm.reason);
            t += d;
        }
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &inuse, t, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);
    }

    /* 409 and "Mount Point Taken" on a guard slot are the same thing. */
    static const char *const busy[] = { "HTTP/1.1 409 Conflict\r\n\r\n", "ERROR - Mount Point Taken or Invalid\r\n",
                                        "ERROR - Already Connected\r\n" };
    for (size_t i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        ntrip_failure_t f = reply_of(busy[i]);
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ntrip_sm_failed(&sm, &f, 1000, MID), busy[i]);
        TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        d = guard_session(&sm, 1000, 200000, &k_kicked, LO);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(15000, ntrip_sm_failed(&sm, &f, 201000 + d, LO), busy[i]);
    }
}

TEST_CASE("ntrip retry class: the failure a retry follows, none once connected or after a lost network",
          "[ntrip]")
{
    const ntrip_failure_t inuse = reply_of("Error 3 Credentials already in use by another stream\r\n");
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL(NTRIP_CLS_NONE, ntrip_sm_retry_class(&sm));   /* never failed */

    /* A session, then an in-use reply inside the window: the wait and the
     * connect of the retry both follow that reply. */
    uint32_t t = 1000;
    uint32_t d = guard_session(&sm, t, 200000, &k_kicked, LO);
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, ntrip_sm_retry_class(&sm));
    t += 200000 + d;
    d = ntrip_sm_failed(&sm, &inuse, t, LO);
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, ntrip_sm_retry_class(&sm));
    t += d;
    ntrip_sm_connecting(&sm, t);
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, ntrip_sm_retry_class(&sm));

    /* Accepted: the reply is history. The status' reason keeps it; the retry class does not. */
    ntrip_sm_connected(&sm, t);
    ntrip_sm_sent(&sm, 10000, t + NTRIP_HEALTHY_SESSION_MS);
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, sm.reason);
    TEST_ASSERT_EQUAL(NTRIP_CLS_NONE, ntrip_sm_retry_class(&sm));

    /* Days later Wi-Fi is gone: the planned close for the lost address
     * records no failure, and the slot waits for the network, then
     * connects, with nothing to follow. */
    t += 3u * 86400u * 1000u;
    ntrip_sm_in_t in = ready();
    in.have_ip = false;
    in.link_up = false;
    in.session_open = true;
    ntrip_close_why_t why;
    sm.prev_link_up = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_CLOSE, ntrip_sm_poll(&sm, &in, t, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_WHY_NO_IP, why);
    ntrip_sm_closed(&sm, t);
    in.session_open = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, ntrip_sm_poll(&sm, &in, t + 1000, MID, &why));
    TEST_ASSERT_EQUAL(NTRIP_SM_WAIT_NETWORK, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_CLS_IN_USE, sm.reason);
    TEST_ASSERT_EQUAL(NTRIP_CLS_NONE, ntrip_sm_retry_class(&sm));
    ntrip_sm_connecting(&sm, t + 60000);
    TEST_ASSERT_EQUAL(NTRIP_CLS_NONE, ntrip_sm_retry_class(&sm));

    /* The user's retry clears it as well. */
    t += 120000;
    (void)ntrip_sm_failed(&sm, &inuse, t, LO);
    TEST_ASSERT_TRUE(sm.stop != NTRIP_STOP_NONE || ntrip_sm_retry_class(&sm) == NTRIP_CLS_IN_USE);
    ntrip_sm_retry(&sm, t);
    TEST_ASSERT_EQUAL(NTRIP_CLS_NONE, ntrip_sm_retry_class(&sm));
}

TEST_CASE("ntrip guard: sessions taken over three times stop the slot", "[ntrip]")
{
    static const uint32_t first[2] = { 5000, 6000 };
    static const uint32_t second[2] = { 10000, 12000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 1000;
        uint32_t d = guard_session(&sm, t, 10000, &k_kicked, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL_UINT8(1, sm.kick_score);
        TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
        t += 10000 + d;
        d = guard_session(&sm, t, 10000, &k_kicked, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(second[k], d);
        t += 10000 + d;
        TEST_ASSERT_EQUAL_UINT32(0, guard_session(&sm, t, 10000, &k_kicked, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_STOP_KICKED, sm.stop);
        assert_stays_stopped(&sm, t + 10000, 2);
    }

    /* "Device reconnected" during a session counts 2, whatever its length. */
    const ntrip_failure_t replaced = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false, false, false,
                                       NTRIP_ONO_REPLACED, 0 };
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t d = guard_session(&sm, 1000, 900000, &replaced, MID);
    TEST_ASSERT_EQUAL_UINT8(2, sm.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_CLS_REPLACED, sm.reason);
    TEST_ASSERT_EQUAL_UINT32(11000, d);   /* 10 s x 1.1 */
    TEST_ASSERT_EQUAL_UINT32(0, guard_session(&sm, 901000 + d, 10000, &replaced, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_KICKED, sm.stop);

    /* A session of 10 min or more resets the score, also while it runs. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 1000;
    t += 10000 + guard_session(&sm, t, 10000, &k_kicked, MID);
    t += 10000 + guard_session(&sm, t, 10000, &k_kicked, MID);
    TEST_ASSERT_EQUAL_UINT8(2, sm.kick_score);
    ntrip_sm_connected(&sm, t);
    ntrip_sm_sent(&sm, 10000, t + 599000);
    TEST_ASSERT_EQUAL_UINT8(2, sm.kick_score);
    ntrip_sm_sent(&sm, 1000, t + 600000);
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    d = ntrip_sm_failed(&sm, &k_kicked, t + 700000, LO);
    TEST_ASSERT_EQUAL_UINT32(1000, d);   /* the fast transient row's floor */
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);

    /* Our own ends never count: write stall, keepalive, network loss, source stall, pause. */
    static const ntrip_cls_t own[] = { NTRIP_CLS_WRITE_STALL, NTRIP_CLS_KEEPALIVE };
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    t = 1000;
    for (int i = 0; i < 6; i++) {
        ntrip_failure_t f = { own[i % 2], NTRIP_ROW_TRANSIENT, true, false, false, false, 0, 0 };
        d = guard_session(&sm, t, 10000, &f, HI);
        TEST_ASSERT_TRUE(d <= 18000);
        t += 10000 + d;
    }
    for (int i = 0; i < 4; i++) {
        ntrip_sm_connected(&sm, t);
        ntrip_sm_closed(&sm, t + 10000);   /* network loss, source stall or pause: not a failure */
        t += 20000;
    }
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);

    /* The caster ending a 200 s session: the fast transient row, 1 s floor. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(1000, guard_session(&sm, 1000, 200000, &k_kicked, LO));
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
}

TEST_CASE("ntrip guard: a duplicate stream or a ban stops at once", "[ntrip]")
{
    ntrip_sm_t sm;
    ntrip_failure_t f = reply_of("Error 9 Duplicate of another stream\r\n");
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_DUPLICATE, sm.stop);

    f = reply_of("Error 8 Credential is banned\r\n");
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_BANNED, sm.stop);

    /* A generic ban reply. */
    f = reply_of("<html><body>The NTRIP Caster has banned your IP</body></html>\r\n");
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_BANNED, sm.stop);

    /* Reported during a session (the server ends it at once). */
    const ntrip_failure_t dup = { NTRIP_CLS_BANNED, NTRIP_ROW_TRANSIENT, true, false, false, false,
                                  NTRIP_ONO_DUPLICATE, 0 };
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(0, guard_session(&sm, 1000, 3600000, &dup, MID));
    TEST_ASSERT_EQUAL(NTRIP_STOP_DUPLICATE, sm.stop);

    /* A standard slot keeps its banned row. */
    f = std_reply_of("Your IP is BANNED for 3 hours\r\n");
    ntrip_sm_init(&sm, &k_std, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(10800000, ntrip_sm_failed(&sm, &f, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_SM_BANNED, sm.state);
}

TEST_CASE("ntrip guard: one unrecognised error line is a transient failure, the next a refusal", "[ntrip]")
{
    const ntrip_failure_t line = reply_of("Error 11 Unknown - epoll rd hup [caster v2]\r\n");
    TEST_ASSERT_TRUE(line.replied);
    TEST_ASSERT_EQUAL(NTRIP_ONO_NONE, line.onocoy);
    static const uint32_t first[2] = { 1000, 1200 };
    static const uint32_t refusal[2] = { 300000, 360000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 400000;
        uint32_t d = ntrip_sm_failed(&sm, &line, t, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
        TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
        t += d;
        TEST_ASSERT_EQUAL_UINT32(refusal[k], ntrip_sm_failed(&sm, &line, t, k_rnds[k]));
        TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    }

    /* A failure without a reply in between does not give the free one back; a healthy session does (the
     * caster's acceptance alone does not: it may end the session at once). */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    ntrip_sm_failed(&sm, &line, 400000, LO);
    ntrip_sm_failed(&sm, &k_silent, 401000, LO);
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &line, 403000, LO));
    ntrip_sm_connected(&sm, 800000);
    TEST_ASSERT_TRUE(sm.line_transient_used);
    TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
    healthy_session(&sm, 800000);
    TEST_ASSERT_FALSE(sm.line_transient_used);
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
}

TEST_CASE("ntrip guard: ten failures without a reply hold every retry at 300 s", "[ntrip]")
{
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 400000;
    for (int i = 0; i < 9; i++) {
        uint32_t d = ntrip_sm_failed(&sm, &k_silent, t, HI);
        TEST_ASSERT_TRUE(d <= 18000);
        t += d;
    }
    /* Refused connects and TLS failures sent no login: they do not count. */
    ntrip_sm_failed(&sm, &k_refused, t, MID);
    const ntrip_failure_t tls = { NTRIP_CLS_TLS, NTRIP_ROW_TRANSIENT, true, false, false, false, 0, 0 };
    ntrip_sm_failed(&sm, &tls, t, MID);
    TEST_ASSERT_EQUAL_UINT8(9, sm.silent_run);
    uint32_t d = ntrip_sm_failed(&sm, &k_silent, t, LO);
    TEST_ASSERT_TRUE(d >= 300000);
    /* Every caster-reaching failure now waits as long, the free error line included ... */
    const ntrip_failure_t line = reply_of("Error 11 Unknown - epoll rd hup [caster v2]\r\n");
    TEST_ASSERT_TRUE(ntrip_sm_failed(&sm, &line, t + d, LO) >= 300000);
    /* ... but failures that never reached the caster keep their flat retry. */
    d = ntrip_sm_failed(&sm, &k_dns, t + 2 * d, HI);
    TEST_ASSERT_TRUE(d <= 6000);
    /* A healthy session ends it; the caster's acceptance alone does not. */
    ntrip_sm_connected(&sm, 3000000);
    TEST_ASSERT_TRUE(sm.silent_run >= 10);
    healthy_session(&sm, 3000000);
    TEST_ASSERT_EQUAL_UINT8(0, sm.silent_run);
    TEST_ASSERT_TRUE(ntrip_sm_failed(&sm, &k_silent, 3061000, HI) < 300000);   /* the transient row again */
}

TEST_CASE("ntrip guard: failures that never reached the caster keep the flat retry and touch no count", "[ntrip]")
{
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 1000;
    for (int i = 0; i < 20; i++) {
        uint32_t d = ntrip_sm_failed(&sm, (i & 1) ? &k_dns : &k_transient, t, MID);
        TEST_ASSERT_EQUAL_UINT32(5000, d);
        t += d;
    }
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
    TEST_ASSERT_EQUAL_UINT8(0, sm.blocks);
    TEST_ASSERT_EQUAL_UINT8(0, sm.inuse);
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    TEST_ASSERT_EQUAL_UINT8(0, sm.silent_run);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    TEST_ASSERT_TRUE(ntrip_sm_unreached_ms(&sm, t) > 0);
}

TEST_CASE("ntrip guard: a stopped slot does nothing whatever the inputs; disabled wins and keeps the stop", "[ntrip]")
{
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    const ntrip_failure_t inuse = reply_fail(NTRIP_CLS_IN_USE, NTRIP_ROW_MOUNT_BUSY, NTRIP_ONO_IN_USE);
    ntrip_sm_failed(&sm, &inuse, 1000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
    ntrip_close_why_t why;
    for (int i = 0; i < 32; i++) {
        ntrip_sm_in_t in = ready();
        in.have_ip = (i & 1) != 0;
        in.link_up = (i & 2) != 0;
        in.paused = (i & 4) != 0;
        in.fresh_data = (i & 8) != 0;
        in.silent_ms = (i & 16) ? 100000 : 0;
        TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, ntrip_sm_poll(&sm, &in, 2000 + (uint32_t)i, (uint32_t)i, &why));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
    }
    ntrip_sm_in_t in = ready();
    in.enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, 5000));
    TEST_ASSERT_EQUAL(NTRIP_SM_DISABLED, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);
    in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, 6000));
    TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
}

TEST_CASE("ntrip guard: a save or the user's retry clears everything and tries at once", "[ntrip]")
{
    const ntrip_failure_t auth = reply_fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_ONO_NONE);
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    sm.prev_link_up = true;
    ntrip_sm_in_t in = ready();
    uint32_t t = 400000;
    for (int i = 0; i < 3; i++) {
        t += ntrip_sm_failed(&sm, &auth, t, MID);
    }
    TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
    ntrip_sm_retry(&sm, t);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
    TEST_ASSERT_EQUAL_UINT32(0, sm.attempt);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t));

    /* Waiting out a guard delay: the retry goes at once too. */
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &auth, t, LO));
    ntrip_sm_retry(&sm, t + 1000);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 1000));

    /* A take-over score and a stop are cleared by a save. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    sm.prev_link_up = true;
    t = 1000;
    t += 10000 + guard_session(&sm, t, 10000, &k_kicked, MID);
    t += 10000 + guard_session(&sm, t, 10000, &k_kicked, MID);
    guard_session(&sm, t, 10000, &k_kicked, MID);
    TEST_ASSERT_EQUAL(NTRIP_STOP_KICKED, sm.stop);
    ntrip_sm_user_edit(&sm, &k_guard, 0x78, t + 20000);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, t + 20000));
}

TEST_CASE("ntrip guard: backoff record version 3, older records, restored stops and delays", "[ntrip]")
{
    /* Every guard change is written, and the stop too; then nothing more. */
    const ntrip_failure_t auth = reply_fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_ONO_NONE);
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 400000;
    uint32_t d = ntrip_sm_failed(&sm, &auth, t, LO);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_backoff_rec_t rec;
    ntrip_sm_persist_get(&sm, t + d - 10000, &rec);
    TEST_ASSERT_EQUAL_UINT8(1, rec.rejects);
    TEST_ASSERT_EQUAL_UINT32(10, rec.remaining_s);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, rec.row);
    ntrip_sm_persist_done(&sm);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));

    /* Round trip. */
    rec.blocks = 2;
    rec.kick_score = 1;
    rec.stop = NTRIP_STOP_KICKED;
    rec.inuse = 2;
    rec.silent_run = 7;
    rec.line_transient_used = true;
    uint8_t buf[NTRIP_BACKOFF_REC_MAX];
    size_t n = ntrip_backoff_rec_encode(&rec, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n <= NTRIP_BACKOFF_REC_MAX);
    TEST_ASSERT_EQUAL_UINT8(4, buf[0]);
    ntrip_backoff_rec_t back;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n, &back));
    TEST_ASSERT_EQUAL_UINT8(1, back.rejects);
    TEST_ASSERT_EQUAL_UINT8(2, back.blocks);
    TEST_ASSERT_EQUAL_UINT8(1, back.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_STOP_KICKED, back.stop);
    TEST_ASSERT_EQUAL_UINT8(2, back.inuse);
    TEST_ASSERT_EQUAL_UINT8(7, back.silent_run);
    TEST_ASSERT_TRUE(back.line_transient_used);
    TEST_ASSERT_EQUAL_UINT32(rec.remaining_s, back.remaining_s);
    /* A version 3 record (before the in-use, no-reply and error-line counts): those read as none. */
    buf[0] = 3;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n - 3, &back));
    TEST_ASSERT_EQUAL_UINT8(1, back.rejects);
    TEST_ASSERT_EQUAL(NTRIP_STOP_KICKED, back.stop);
    TEST_ASSERT_EQUAL_UINT8(0, back.inuse);
    TEST_ASSERT_EQUAL_UINT8(0, back.silent_run);
    TEST_ASSERT_FALSE(back.line_transient_used);
    /* A version 2 record (what the firmware before the guard wrote): no guard counts. */
    buf[0] = 2;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n - 7, &back));
    TEST_ASSERT_EQUAL_UINT8(0, back.rejects);
    TEST_ASSERT_EQUAL_UINT8(0, back.blocks);
    TEST_ASSERT_EQUAL_UINT8(0, back.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, back.stop);
    TEST_ASSERT_EQUAL(NTRIP_ROW_AUTH, back.row);
    /* An unknown stop reason: refused. */
    buf[0] = 4;
    buf[n - 4] = NTRIP_STOP_COUNT;
    TEST_ASSERT_FALSE(ntrip_backoff_rec_decode(buf, n, &back));

    /* A stopped slot stays stopped after a restart (48 h, no attempt). */
    ntrip_backoff_rec_t stopped = { .cls = NTRIP_CLS_REMOTE_CLOSE, .row = NTRIP_ROW_NONE, .good_valid = true,
                                    .good_hash = 0x77, .kick_score = 3, .stop = NTRIP_STOP_KICKED };
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    ntrip_sm_restore(&sm, &stopped, 0);
    TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));
    assert_stays_stopped(&sm, 0, 48);

    /* A pending refusal: the full 300 s, though 10 s were left (and the first may have been the 30 s one). */
    ntrip_backoff_rec_t pending = { .cls = NTRIP_CLS_AUTH, .row = NTRIP_ROW_AUTH, .attempt = 1, .remaining_s = 10,
                                    .good_valid = true, .good_hash = 0x77, .rejects = 1 };
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    ntrip_sm_restore(&sm, &pending, 0);
    sm.prev_link_up = true;
    ntrip_sm_in_t in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, 299999));
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, 300000));
    /* ... and the next refusal is the second step. */
    TEST_ASSERT_EQUAL_UINT32(1800000, ntrip_sm_failed(&sm, &auth, 300000, LO));

    /* A pending login block: the full hour. */
    ntrip_backoff_rec_t blocked = { .cls = NTRIP_CLS_LOGIN_BLOCKED, .row = NTRIP_ROW_AUTH, .attempt = 1,
                                    .remaining_s = 5, .blocks = 1 };
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    ntrip_sm_restore(&sm, &blocked, 0);
    sm.prev_link_up = true;
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, 3599999));
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, 3600000));

    /* A slot that no longer counts as Onocoy drops a stored stop at its next write. */
    ntrip_sm_init(&sm, &k_fast, 0x77, 0);
    ntrip_sm_restore(&sm, &stopped, 0);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_sm_persist_get(&sm, 0, &rec);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, rec.stop);
    TEST_ASSERT_EQUAL_UINT8(0, rec.kick_score);
}

TEST_CASE("ntrip guard: standard and polite slots keep their rows", "[ntrip]")
{
    /* The same replies on a standard slot: as before the guard. */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_std, 0x77, 0);
    ntrip_failure_t f = std_reply_of("Error 3 Credentials already in use by another stream\r\n");
    TEST_ASSERT_EQUAL_UINT32(2000, ntrip_sm_failed(&sm, &f, 1000, MID));
    TEST_ASSERT_EQUAL(NTRIP_SM_BACKOFF, sm.state);
    f = std_reply_of("ERROR - Bad Password\r\n");
    ntrip_sm_failed(&sm, &f, 400000, MID);
    TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
    for (int i = 0; i < 10; i++) {
        ntrip_sm_failed(&sm, &f, 400000 + (uint32_t)i, MID);
    }
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    /* Sessions ended soon after login: no score. */
    ntrip_sm_init(&sm, &k_polite, 0x77, 0);
    for (int i = 0; i < 5; i++) {
        guard_session(&sm, 1000 + (uint32_t)i * 100000, 10000, &k_kicked, MID);
    }
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
}

TEST_CASE("ntrip guard: 409, Mount Point Taken, Already Connected as the server hands them over are in use", "[ntrip]")
{
    static const char *const busy[] = { "HTTP/1.1 409 Conflict\r\n\r\n", "ERROR - Mount Point Taken or Invalid\r\n",
                                        "ERROR - Already Connected\r\n" };
    static const uint32_t steps[4] = { 15000, 30000, 60000, 120000 };
    for (size_t i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        /* The server counts and logs the guard's class: it reaches ntrip_sm_failed already converted. */
        const ntrip_failure_t f = reply_of(busy[i]);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_CLS_IN_USE, f.cls, busy[i]);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_ROW_MOUNT_BUSY, f.row, busy[i]);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_ONO_NONE, f.onocoy, busy[i]);
        TEST_ASSERT_TRUE_MESSAGE(f.replied, busy[i]);
        /* The same failure as a literal (cls IN_USE, row MOUNT_BUSY, no Onocoy text): both forms count. */
        const ntrip_failure_t lit = { NTRIP_CLS_IN_USE, NTRIP_ROW_MOUNT_BUSY, true, f.mount_ambiguous, false, true, 0,
                                      f.rule };
        const ntrip_failure_t *forms[2] = { &f, &lit };
        for (int k = 0; k < 2; k++) {
            /* Credentials that never worked: stopped at once, no refused login counted. */
            ntrip_sm_t sm;
            ntrip_sm_init(&sm, &k_guard, 0x77, 0);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ntrip_sm_failed(&sm, forms[k], 400000, MID), busy[i]);
            TEST_ASSERT_EQUAL_MESSAGE(NTRIP_STOP_IN_USE, sm.stop, busy[i]);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sm.rejects, busy[i]);
            /* Outside the window on credentials that worked: stopped at once too. */
            ntrip_sm_init(&sm, &k_guard, 0x77, 0);
            uint32_t d = guard_session(&sm, 1000, 200000, &k_kicked, LO);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ntrip_sm_failed(&sm, forms[k], 201000 + d + 300000, MID), busy[i]);
            TEST_ASSERT_EQUAL_MESSAGE(NTRIP_STOP_IN_USE, sm.stop, busy[i]);
            /* Inside the window after this station's own session: 15, 30, 60, 120 s, the 5th stops. */
            ntrip_sm_init(&sm, &k_guard, 0x77, 0);
            uint32_t t = 1000;
            d = guard_session(&sm, t, 200000, &k_kicked, LO);
            t += 200000 + d;
            for (int n = 0; n < 4; n++) {
                d = ntrip_sm_failed(&sm, forms[k], t, LO);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(steps[n], d, busy[i]);
                TEST_ASSERT_EQUAL_MESSAGE(NTRIP_CLS_IN_USE, sm.reason, busy[i]);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sm.rejects, busy[i]);
                t += d;
            }
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ntrip_sm_failed(&sm, forms[k], t, LO), busy[i]);
            TEST_ASSERT_EQUAL_MESSAGE(NTRIP_STOP_IN_USE, sm.stop, busy[i]);
        }
    }
}

/* A session the caster accepted and then ended with Onocoy's reason (the server's session failure). */
static ntrip_failure_t session_end(ntrip_onocoy_reason_t why)
{
    ntrip_failure_t f = { NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, true, false, false, false, (uint8_t)why, 0 };
    f.cls = ntrip_guard_class(&f);
    return f;
}

TEST_CASE("ntrip guard: an accepted login that ends at once with a refusal counts toward the stop", "[ntrip]")
{
    /* "ICY 200 OK", then "Authentication failed" (or "Reauthentication failed") and a close: three times. */
    const ntrip_failure_t auth = session_end(NTRIP_ONO_AUTH);
    static const uint32_t first[2] = { 30000, 36000 };      /* inside the window on credentials that worked */
    static const uint32_t second[2] = { 1800000, 2160000 };
    for (int k = 0; k < 2; k++) {
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 400000;
        uint32_t d = guard_session(&sm, t, 2000, &auth, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(first[k], d);
        TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
        TEST_ASSERT_EQUAL(NTRIP_SM_AUTH_FAILED, sm.state);
        t += 2000 + d;
        d = guard_session(&sm, t, 2000, &auth, k_rnds[k]);
        TEST_ASSERT_EQUAL_UINT32(second[k], d);
        TEST_ASSERT_EQUAL_UINT8(2, sm.rejects);
        t += 2000 + d;
        TEST_ASSERT_EQUAL_UINT32(0, guard_session(&sm, t, 2000, &auth, k_rnds[k]));
        TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);
        TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
        assert_stays_stopped(&sm, t + 2000, 24);
    }

    /* The counts persist across those sessions: one write per change, never back to 0 at each accept. */
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 400000;
    t += 2000 + guard_session(&sm, t, 2000, &auth, LO);
    ntrip_sm_persist_done(&sm);
    ntrip_sm_connecting(&sm, t);
    ntrip_sm_connected(&sm, t);
    TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm) && sm.rejects != sm.saved_rejects);

    /* "Too many unsuccessful logins" during a session: an hour, two hours, then stopped. */
    const ntrip_failure_t blocked = session_end(NTRIP_ONO_LOGIN_BLOCKED);
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    t = 400000;
    uint32_t d = guard_session(&sm, t, 2000, &blocked, LO);
    TEST_ASSERT_EQUAL_UINT32(3600000, d);
    t += 2000 + d;
    TEST_ASSERT_EQUAL_UINT32(7200000, guard_session(&sm, t, 2000, &blocked, LO));
    t += 2000 + 7200000;
    TEST_ASSERT_EQUAL_UINT32(0, guard_session(&sm, t, 2000, &blocked, LO));
    TEST_ASSERT_EQUAL(NTRIP_STOP_LOGIN_BLOCKED, sm.stop);

    /* A healthy session in between ends the incident: the next refusal is a first one again. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    t = 400000;
    t += 2000 + guard_session(&sm, t, 2000, &auth, LO);
    t += 2000 + guard_session(&sm, t, 2000, &auth, LO);
    TEST_ASSERT_EQUAL_UINT8(2, sm.rejects);
    d = guard_session(&sm, t, NTRIP_HEALTHY_SESSION_MS, &auth, LO);
    TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);
    TEST_ASSERT_EQUAL_UINT32(30000, d);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
}

TEST_CASE("ntrip guard: Onocoy's reason in a reply line the caster closes without its line end", "[ntrip]")
{
    static ntrip_reply_t r;
    /* Closed (eof) and timed out: the partial line is matched all the same. */
    for (int eof = 0; eof < 2; eof++) {
        ntrip_verdict_t v = run_reply("Error 27 Authentication failed [onocoy naomi v2.5.95]", false, eof != 0, &r);
        TEST_ASSERT_EQUAL_UINT8(17, v.rule);
        TEST_ASSERT_EQUAL(NTRIP_ONO_AUTH, v.onocoy_reason);
        ntrip_failure_t f = ntrip_reply_failure(&v, true);
        TEST_ASSERT_TRUE(f.replied);
        TEST_ASSERT_EQUAL(NTRIP_CLS_AUTH, f.cls);
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 400000;
        TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &f, t, LO));
        TEST_ASSERT_EQUAL_UINT32(1800000, ntrip_sm_failed(&sm, &f, t + 300000, LO));
        TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &f, t + 2100000, LO));
        TEST_ASSERT_EQUAL(NTRIP_STOP_REJECTED, sm.stop);
        TEST_ASSERT_EQUAL_UINT8(0, sm.silent_run);
    }
    /* Every reason, each without a line end. */
    static const struct {
        const char *line;
        ntrip_stop_t stop;
    } once[] = {
        { "Error 22 Credentials already in use by another stream", NTRIP_STOP_IN_USE },
        { "Maximum Number of parallel connections reached for this credential", NTRIP_STOP_IN_USE },
        { "Error 24 Device reconnected", NTRIP_STOP_IN_USE },
        { "Error 25 Duplicate of another stream", NTRIP_STOP_DUPLICATE },
        { "Error 26 Credential is banned", NTRIP_STOP_BANNED },
    };
    for (size_t i = 0; i < sizeof(once) / sizeof(once[0]); i++) {
        ntrip_verdict_t v = run_reply(once[i].line, false, true, &r);
        ntrip_failure_t f = ntrip_reply_failure(&v, true);
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, ntrip_sm_failed(&sm, &f, 400000, MID), once[i].line);
        TEST_ASSERT_EQUAL_MESSAGE(once[i].stop, sm.stop, once[i].line);
    }
    ntrip_verdict_t v = run_reply("Error 21 Too many unsuccessful logins", false, true, &r);
    ntrip_failure_t f = ntrip_reply_failure(&v, true);
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(3600000, ntrip_sm_failed(&sm, &f, 400000, LO));
    TEST_ASSERT_EQUAL_UINT8(1, sm.blocks);
    v = run_reply("Error 29 Invalid request in headers", false, true, &r);
    f = ntrip_reply_failure(&v, true);
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &f, 400000, LO));
    TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);

    /* A partial line that names no reason is still no reply (the silent run, the fast row). */
    v = run_reply("Error 11 Unknown", false, true, &r);
    f = ntrip_reply_failure(&v, true);
    TEST_ASSERT_FALSE(f.replied);
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_TRUE(ntrip_sm_failed(&sm, &f, 400000, HI) <= 1200);
    TEST_ASSERT_EQUAL_UINT8(1, sm.silent_run);
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
    /* Nothing at all: no reply either. */
    v = run_reply("", false, true, &r);
    TEST_ASSERT_FALSE(ntrip_reply_failure(&v, true).replied);
}

TEST_CASE("ntrip guard: 500, 503 and Too many sources are the caster's trouble, not refused logins", "[ntrip]")
{
    static const char *const outage[] = { "HTTP/1.1 503 Service Unavailable\r\n\r\nService Unavailable\r\n",
                                          "HTTP/1.1 500 Internal Server Error\r\n\r\n",
                                          "ERROR - Too many sources\r\n" };
    for (size_t i = 0; i < sizeof(outage) / sizeof(outage[0]); i++) {
        const ntrip_failure_t f = reply_of(outage[i]);
        TEST_ASSERT_TRUE_MESSAGE(f.replied, outage[i]);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_ROW_TRANSIENT, f.row, outage[i]);
        ntrip_sm_t sm;
        ntrip_sm_init(&sm, &k_guard, 0x77, 0);
        uint32_t t = 400000;
        /* An outage of half an hour: the fast transient row throughout (cap 15 s, then 30 s after 10 min). */
        for (int n = 0; n < 120; n++) {
            uint32_t d = ntrip_sm_failed(&sm, &f, t, HI);
            TEST_ASSERT_TRUE_MESSAGE(d >= 1000 && d <= 36000, outage[i]);
            TEST_ASSERT_EQUAL_MESSAGE(NTRIP_SM_BACKOFF, sm.state, outage[i]);
            t += d;
        }
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sm.rejects, outage[i]);
        TEST_ASSERT_EQUAL_MESSAGE(NTRIP_STOP_NONE, sm.stop, outage[i]);
        TEST_ASSERT_FALSE_MESSAGE(sm.line_transient_used, outage[i]);
        /* ... and they leave the free retry of an unrecognised error line alone. */
        const ntrip_failure_t line = reply_of("Error 11 Unknown - epoll rd hup [caster v2]\r\n");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(NTRIP_RULE_ERROR_LINE, line.rule, outage[i]);
        TEST_ASSERT_TRUE_MESSAGE(ntrip_sm_failed(&sm, &line, t, LO) <= 30000, outage[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sm.rejects, outage[i]);
    }
}

TEST_CASE("ntrip guard: only the caster's own end of a short session scores, never a local failure", "[ntrip]")
{
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, ntrip_errno_class(ECONNRESET));
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, ntrip_errno_class(EPIPE));
    TEST_ASSERT_EQUAL(NTRIP_CLS_REMOTE_CLOSE, ntrip_errno_class(ENOTCONN));
    TEST_ASSERT_EQUAL(NTRIP_CLS_KEEPALIVE, ntrip_errno_class(ECONNABORTED));
    TEST_ASSERT_EQUAL(NTRIP_CLS_KEEPALIVE, ntrip_errno_class(ETIMEDOUT));
    /* No errno, a stale one from an earlier non-blocking call, memory: this station's own. */
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOCAL, ntrip_errno_class(0));
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOCAL, ntrip_errno_class(EAGAIN));
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOCAL, ntrip_errno_class(ENOMEM));
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOCAL, ntrip_errno_class(ENOBUFS));
    TEST_ASSERT_EQUAL(NTRIP_CLS_LOCAL, ntrip_errno_class(EINVAL));

    /* Short sessions that ended in a local or TLS failure: the fast transient row, never a score. */
    static const ntrip_cls_t own[] = { NTRIP_CLS_LOCAL, NTRIP_CLS_TLS };
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t t = 1000;
    for (int i = 0; i < 8; i++) {
        ntrip_failure_t f = { own[i % 2], NTRIP_ROW_TRANSIENT, true, false, false, false, 0, 0 };
        uint32_t d = guard_session(&sm, t, 10000, &f, HI);
        TEST_ASSERT_TRUE(d <= 18000);
        t += 10000 + d;
    }
    TEST_ASSERT_EQUAL_UINT8(0, sm.kick_score);
    TEST_ASSERT_EQUAL(NTRIP_STOP_NONE, sm.stop);
    /* A local failure while sending the request: no silent run either (no login went out unanswered). */
    ntrip_failure_t req = { NTRIP_CLS_LOCAL, NTRIP_ROW_TRANSIENT, true, false, false, false, 0, 0 };
    for (int i = 0; i < 12; i++) {
        t += ntrip_sm_failed(&sm, &req, t, MID);
    }
    TEST_ASSERT_EQUAL_UINT8(0, sm.silent_run);
    /* The caster's own end (REMOTE_CLOSE) still scores. */
    TEST_ASSERT_EQUAL_UINT32(5000, guard_session(&sm, t, 10000, &k_kicked, LO));
    TEST_ASSERT_EQUAL_UINT8(1, sm.kick_score);
}

/* Persist as the server does after a failure, then "reboot": a fresh state machine restores the record. */
static void reboot_guard(ntrip_sm_t *sm, uint32_t now)
{
    ntrip_backoff_rec_t rec;
    ntrip_sm_persist_get(sm, now, &rec);
    ntrip_sm_persist_done(sm);
    uint8_t buf[NTRIP_BACKOFF_REC_MAX];
    size_t n = ntrip_backoff_rec_encode(&rec, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    ntrip_backoff_rec_t back;
    TEST_ASSERT_TRUE(ntrip_backoff_rec_decode(buf, n, &back));
    ntrip_sm_init(sm, &k_guard, 0x77, 0);   /* boot: uptime 0, the window opens again */
    ntrip_sm_restore(sm, &back, 0);
    sm->prev_link_up = true;
}

TEST_CASE("ntrip guard: a reboot loop never restarts the in-use ladder or the run without a reply", "[ntrip]")
{
    /* Another device holds the credential; the station reboots every two attempts. */
    const ntrip_failure_t inuse = reply_of("Error 3 Credentials already in use by another stream\r\n");
    ntrip_sm_t sm;
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    uint32_t d = guard_session(&sm, 1000, 200000, &k_kicked, LO);
    uint32_t t = 201000 + d;
    TEST_ASSERT_EQUAL_UINT32(15000, ntrip_sm_failed(&sm, &inuse, t, LO));
    TEST_ASSERT_EQUAL_UINT32(30000, ntrip_sm_failed(&sm, &inuse, t + 15000, LO));
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    reboot_guard(&sm, t + 20000);
    TEST_ASSERT_EQUAL_UINT8(2, sm.inuse);
    TEST_ASSERT_TRUE(sm.good_valid);
    /* After the boot (inside the boot's window, credentials that worked): the ladder goes on. */
    TEST_ASSERT_EQUAL_UINT32(60000, ntrip_sm_failed(&sm, &inuse, 1000, LO));
    reboot_guard(&sm, 2000);
    TEST_ASSERT_EQUAL_UINT32(120000, ntrip_sm_failed(&sm, &inuse, 1000, LO));
    reboot_guard(&sm, 2000);
    TEST_ASSERT_EQUAL_UINT32(0, ntrip_sm_failed(&sm, &inuse, 1000, LO));
    TEST_ASSERT_EQUAL(NTRIP_STOP_IN_USE, sm.stop);
    reboot_guard(&sm, 2000);
    TEST_ASSERT_EQUAL(NTRIP_SM_STOPPED, sm.state);

    /* Ten failures without a reply, then a reboot: the floor holds, from the first attempt on. */
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    t = 400000;
    for (int i = 0; i < 10; i++) {
        t += ntrip_sm_failed(&sm, &k_silent, t, LO);
    }
    TEST_ASSERT_EQUAL_UINT8(10, sm.silent_run);
    reboot_guard(&sm, t);
    TEST_ASSERT_EQUAL_UINT8(10, sm.silent_run);
    ntrip_sm_in_t in = ready();
    TEST_ASSERT_EQUAL(NTRIP_ACT_NONE, step(&sm, &in, 299999));
    TEST_ASSERT_EQUAL(NTRIP_ACT_CONNECT, step(&sm, &in, 300000));
    TEST_ASSERT_TRUE(ntrip_sm_failed(&sm, &k_silent, 300000, LO) >= 300000);
    /* The run is stored only up to the floor: nothing more to write after it. */
    ntrip_sm_persist_done(&sm);
    ntrip_sm_failed(&sm, &k_silent, 700000, LO);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));

    /* The free retry of an unrecognised error line is used up across a reboot too. */
    const ntrip_failure_t line = reply_of("Error 11 Unknown - epoll rd hup [caster v2]\r\n");
    ntrip_sm_init(&sm, &k_guard, 0x77, 0);
    TEST_ASSERT_TRUE(ntrip_sm_failed(&sm, &line, 400000, LO) <= 1200);
    reboot_guard(&sm, 401000);
    TEST_ASSERT_TRUE(sm.line_transient_used);
    TEST_ASSERT_EQUAL_UINT32(300000, ntrip_sm_failed(&sm, &line, 1000, LO));
    TEST_ASSERT_EQUAL_UINT8(1, sm.rejects);

    /* A healthy session ends all of it, and that is written once. */
    healthy_session(&sm, 400000);
    TEST_ASSERT_EQUAL_UINT8(0, sm.rejects);
    TEST_ASSERT_FALSE(sm.line_transient_used);
    TEST_ASSERT_TRUE(ntrip_sm_persist_due(&sm));
    ntrip_sm_persist_done(&sm);
    TEST_ASSERT_FALSE(ntrip_sm_persist_due(&sm));
}

TEST_CASE("ntrip a test Onocoy slot uses Onocoy's fields, wire names and the one-Onocoy rule", "[ntrip]")
{
    /* Fields: f->onocoy makes any host an Onocoy slot. */
    ntrip_fields_t f = fields("10.0.2.2", "TESTMOUNT", "testuser 01", "fakepass123", false);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));   /* not Onocoy: no username rule */
    f.onocoy = true;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_USERNAME, ntrip_validate_fields(&f));
    f = fields("10.0.2.2", "TESTMOUNT", "testuser01", "fake pass", true);
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_OK, ntrip_validate_fields(&f));   /* a 2.0 password may have spaces elsewhere */
    f.onocoy = true;
    TEST_ASSERT_EQUAL(NTRIP_FIELDS_BAD_PASSWORD, ntrip_validate_fields(&f));

    /* Wire names: NTRIP 1.0 sends SOURCE <password> /<username>. */
    ntrip_wire_names_t w;
    ntrip_wire_names(true, false, "TESTMOUNT", "testuser01", "", &w);
    char buf[NTRIP_REQUEST_MAX];
    TEST_ASSERT_TRUE(ntrip_build_v1_request(buf, sizeof(buf), "fakepass123", w.mount, "NTRIP onodiy/0.3.0") > 0);
    TEST_ASSERT_EQUAL_STRING("SOURCE fakepass123 /testuser01\r\nSource-Agent: NTRIP onodiy/0.3.0\r\n\r\n",
                             buf);
    ntrip_wire_names(false, false, "TESTMOUNT", "testuser01", "", &w);
    TEST_ASSERT_EQUAL_STRING("TESTMOUNT", w.mount);

    /* Policy: a test Onocoy slot and a real one are two Onocoy slots. */
    ntrip_policy_slot_t slots[3] = {
        { .enabled = true, .host = "10.0.2.2", .dup_ack = false, .onocoy = true },
        { .enabled = true, .host = "servers.onocoy.com", .dup_ack = false, .onocoy = false },
        { .enabled = false, .host = "rtk2go.com", .dup_ack = false, .onocoy = false },
    };
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_ONOCOY, ntrip_check_policy(slots, 3, 0));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_SECOND_ONOCOY, ntrip_check_policy(slots, 3, 1));
    slots[1].enabled = false;
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(slots, 3, 0));
    slots[2].enabled = true;   /* with an open caster: the acknowledgement */
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(slots, 3, 0));
    TEST_ASSERT_EQUAL(NTRIP_POLICY_NEED_DUP_ACK, ntrip_check_policy(slots, 3, 2));
    slots[0].onocoy = false;   /* the same host without the mark: just another caster */
    TEST_ASSERT_EQUAL(NTRIP_POLICY_OK, ntrip_check_policy(slots, 3, 0));
}
