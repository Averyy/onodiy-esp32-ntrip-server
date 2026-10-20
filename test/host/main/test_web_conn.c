/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "web_conn.h"

#define DEADLINE 6000u
#define T0       (UINT32_MAX - 2000u)   /* the clock wraps during the tests */
#define MAXS     7u
#define Q        WEB_CONN_QUIET_MS   /* a plain connection quiet this long may be closed outright */

static web_conns_t t;

/* ---- request deadline ---------------------------------------------------------------------- */

TEST_CASE("web conn: a request must be in within the deadline of its first byte", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    TEST_ASSERT_TRUE(web_conn_open(&t, 54, T0));
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 54, T0 + 60000u));   /* idle: no limit of ours */

    web_conn_received(&t, 54, T0 + 1000u);                                   /* first byte */
    TEST_ASSERT_EQUAL_INT32(DEADLINE, web_conn_recv_budget(&t, 54, T0 + 1000u));
    web_conn_received(&t, 54, T0 + 4000u);                                   /* more bytes: no re-arm */
    TEST_ASSERT_EQUAL_INT32(1000, web_conn_recv_budget(&t, 54, T0 + 6000u));
    TEST_ASSERT_EQUAL_INT32(0, web_conn_recv_budget(&t, 54, T0 + 7000u));    /* time is up */
    TEST_ASSERT_EQUAL_INT32(0, web_conn_recv_budget(&t, 54, T0 + 9000u));

    /* A request that made it: the handler ends it; the next one starts afresh. */
    web_conn_received(&t, 55, T0);                                           /* unknown fd: ignored */
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 55, T0));
    TEST_ASSERT_TRUE(web_conn_open(&t, 55, T0));
    web_conn_received(&t, 55, T0 + 100u);
    web_conn_done(&t, 55, T0 + 300u, false);
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 55, T0 + 30000u));
    web_conn_received(&t, 55, T0 + 30000u);
    TEST_ASSERT_EQUAL_INT32(DEADLINE - 2000, web_conn_recv_budget(&t, 55, T0 + 32000u));

    web_conn_close(&t, 54);
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 54, T0 + 9000u));
    TEST_ASSERT_EQUAL_size_t(1, web_conn_count(&t));
}

TEST_CASE("web conn: the worker's reads are not bounded; it hands the connection back", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    web_conn_open(&t, 60, T0);
    web_conn_received(&t, 60, T0 + 10u);                                     /* an upload's headers */
    web_conn_async(&t, 60, true, T0 + 50u);                                  /* to the worker */
    web_conn_done(&t, 60, T0 + 60u, true);                                   /* the handler returns: stays there */
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 60, T0 + 600000u)); /* minutes of body */
    web_conn_received(&t, 60, T0 + 600000u);
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 60, T0 + 700000u));
    web_conn_async(&t, 60, false, T0 + 700000u);                             /* the worker is done */
    web_conn_received(&t, 60, T0 + 800000u);                                 /* the next request is bounded */
    TEST_ASSERT_EQUAL_INT32(DEADLINE, web_conn_recv_budget(&t, 60, T0 + 800000u));
}

TEST_CASE("web conn: a WebSocket frame is bounded like a request; the handshake disarms", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    web_conn_open(&t, 70, T0);
    web_conn_received(&t, 70, T0 + 5u);                                      /* GET /ws */
    web_conn_websocket(&t, 70, true, T0 + 20u);
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 70, T0 + 90000u));
    web_conn_received(&t, 70, T0 + 90000u);                                  /* a frame's first byte */
    TEST_ASSERT_EQUAL_INT32(0, web_conn_recv_budget(&t, 70, T0 + 90000u + DEADLINE));
    web_conn_done(&t, 70, T0 + 91000u, false);                               /* frame handled */
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 70, T0 + 99000u));
    TEST_ASSERT_TRUE(t.c[0].admin);                                          /* the owner stays the handshake's */
}

/* ---- eviction ------------------------------------------------------------------------------------ */

/* Fill the table: admin WebSocket 10, public viewer 11, admin keep-alive 12,
 * three plain keep-alive connections 13-15 (13 the oldest). */
static void fill(void)
{
    web_conns_init(&t, DEADLINE);
    for (int fd = 10; fd <= 15; fd++) {
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, T0 + (uint32_t)fd));
    }
    web_conn_websocket(&t, 10, true, T0 + 10u);
    web_conn_websocket(&t, 11, false, T0 + 11u);
    web_conn_done(&t, 12, T0 + 12u, true);
    web_conn_done(&t, 14, T0 + 500u, false);
    web_conn_done(&t, 15, T0 + 600u, false);
}

TEST_CASE("web conn: one socket stays free; idle plain connections go first, oldest first", "[web_conn]")
{
    fill();
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 15, MAXS, T0 + Q + 1000u));    /* six open: room left */
    TEST_ASSERT_TRUE(web_conn_open(&t, 16, T0 + Q + 1000u));                     /* the seventh */
    TEST_ASSERT_EQUAL_INT(13, web_conn_victim(&t, 16, MAXS, T0 + Q + 1000u));
    TEST_ASSERT_EQUAL_size_t(MAXS - 1u, web_conn_count(&t));
    /* The closing one is not chosen twice, and while it goes there is room. */
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 16, MAXS, T0 + Q + 1001u));
    web_conn_close(&t, 13);
    TEST_ASSERT_TRUE(web_conn_open(&t, 17, T0 + Q + 2000u));
    TEST_ASSERT_EQUAL_INT(14, web_conn_victim(&t, 17, MAXS, T0 + Q + 2000u));
}

TEST_CASE("web conn: a connection with a request waiting in its socket is never closed", "[web_conn]")
{
    fill();
    TEST_ASSERT_TRUE(web_conn_open(&t, 16, T0 + Q + 1000u));
    web_conn_set_unread(&t, 13, true);                                      /* the oldest idle one: a request waits */
    TEST_ASSERT_EQUAL_INT(14, web_conn_victim(&t, 16, MAXS, T0 + Q + 1000u));
    web_conn_close(&t, 14);
    /* Every quiet plain connection with a request waiting: the viewer is not cut for this one; a socket is owed,
     * and a waiting request's answer pays it as it begins. */
    TEST_ASSERT_TRUE(web_conn_open(&t, 17, T0 + Q + 2000u));
    web_conn_set_unread(&t, 12, true);
    web_conn_set_unread(&t, 15, true);
    web_conn_set_unread(&t, 16, true);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 17, MAXS, T0 + Q + 2000u));
    TEST_ASSERT_EQUAL_UINT32(1, t.owed);
    TEST_ASSERT_TRUE(web_conn_take_owed(&t, 13));
    web_conn_close(&t, 13);
    TEST_ASSERT_TRUE(web_conn_open(&t, 18, T0 + Q + 3000u));
    web_conn_set_unread(&t, 17, true);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 18, MAXS, T0 + Q + 3000u));
    TEST_ASSERT_TRUE(web_conn_take_owed(&t, 15));
    web_conn_close(&t, 15);
    web_conn_close(&t, 18);
    TEST_ASSERT_EQUAL_UINT32(0, t.owed);
    /* Read since then: each choice reads the sockets afresh. */
    TEST_ASSERT_TRUE(web_conn_open(&t, 19, T0 + Q + 4000u));
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 19, MAXS, T0 + Q + 4000u));   /* room again */
    TEST_ASSERT_TRUE(web_conn_open(&t, 20, T0 + Q + 4001u));
    web_conn_set_unread(&t, 16, false);
    TEST_ASSERT_EQUAL_INT(16, web_conn_victim(&t, 20, MAXS, T0 + Q + 4001u));   /* not 19: just opened */
    web_conn_set_unread(&t, 99, true);                                          /* an unknown socket: nothing */
}

TEST_CASE("web conn: an admin's WebSocket is never closed for someone else's socket", "[web_conn]")
{
    fill();
    /* A client opening sockets one after another (and sending requests on
     * them) only ever pushes out plain connections: the quiet ones outright,
     * oldest first; then, its own being just busy, one of them ends after its
     * next answer (an owed socket). The admin's page, its keep-alive
     * connection and the viewer stay. */
    int own_oldest = 16;
    for (int i = 0; i < 20; i++) {
        int fd = 16 + i;
        uint32_t now = T0 + Q + 3000u + (uint32_t)i * 10u;
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, now));
        int v = web_conn_victim(&t, fd, MAXS, now);
        if (i < 3) {
            TEST_ASSERT_EQUAL_INT(13 + i, v);                                /* the quiet plain ones first */
            web_conn_close(&t, v);
        } else {
            TEST_ASSERT_EQUAL_INT(-1, v);                                    /* owed instead */
            TEST_ASSERT_EQUAL_UINT32(1, t.owed);
            TEST_ASSERT_TRUE(web_conn_take_owed(&t, own_oldest));            /* its next request's answer ends it */
            web_conn_close(&t, own_oldest++);
            TEST_ASSERT_EQUAL_UINT32(0, t.owed);
        }
        web_conn_done(&t, fd, now, false);
    }
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(10 + i, t.c[i].fd);                            /* still open, not closing */
        TEST_ASSERT_FALSE(t.c[i].closing);
    }
}

TEST_CASE("web conn: an admin's idle connection goes before a viewer, a viewer before the new one", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    for (int fd = 1; fd <= 6; fd++) {
        web_conn_open(&t, fd, T0);
    }
    web_conn_websocket(&t, 1, true, T0);
    web_conn_websocket(&t, 2, false, T0);
    web_conn_done(&t, 3, T0 + 3u, true);                                     /* the admin's keep-alive */
    web_conn_async(&t, 4, true, T0);
    web_conn_async(&t, 5, true, T0);
    web_conn_async(&t, 6, true, T0);
    web_conn_open(&t, 7, T0 + 10u);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 7, MAXS, T0 + 10u));      /* the keep-alive was just busy: owed */
    TEST_ASSERT_TRUE(web_conn_take_owed(&t, 3));
    web_conn_close(&t, 3);
    web_conn_done(&t, 7, T0 + 11u, true);                                    /* the admin's keep-alive again */
    web_conn_open(&t, 8, T0 + Q + 20u);
    TEST_ASSERT_EQUAL_INT(7, web_conn_victim(&t, 8, MAXS, T0 + Q + 20u));   /* quiet now: closed outright */
    web_conn_close(&t, 7);

    web_conn_websocket(&t, 8, false, T0 + Q + 21u);                          /* a second viewer */
    web_conn_open(&t, 9, T0 + Q + 30u);
    TEST_ASSERT_EQUAL_INT(2, web_conn_victim(&t, 9, MAXS, T0 + Q + 30u));   /* the older viewer */
}

TEST_CASE("web conn: nothing else may go: the new connection stays and the next plain answer frees a socket", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    for (int fd = 1; fd <= 6; fd++) {
        web_conn_open(&t, fd, T0);
    }
    for (int fd = 1; fd <= 4; fd++) {
        web_conn_websocket(&t, fd, true, T0);                                /* four admin pages */
    }
    web_conn_async(&t, 5, true, T0);                                         /* an upload */
    web_conn_async(&t, 6, true, T0);                                         /* a download */
    web_conn_open(&t, 7, T0 + 1u);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 7, MAXS, T0 + 1u));       /* kept: a socket is owed */
    TEST_ASSERT_EQUAL_UINT32(1, t.owed);
    TEST_ASSERT_EQUAL_size_t(MAXS, web_conn_count(&t));
    TEST_ASSERT_FALSE(web_conn_take_owed(&t, 1));                            /* never a WebSocket */
    TEST_ASSERT_TRUE(web_conn_take_owed(&t, 7));                             /* its first request's answer ends it */
    TEST_ASSERT_EQUAL_UINT32(0, t.owed);
    TEST_ASSERT_FALSE(web_conn_take_owed(&t, 7));                            /* paid once */
    web_conn_close(&t, 7);
    TEST_ASSERT_EQUAL_UINT32(0, t.owed);                                     /* that close was the payment */
    /* Owed again, and a connection closing by itself pays it. */
    web_conn_open(&t, 8, T0 + 2u);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 8, MAXS, T0 + 2u));
    TEST_ASSERT_EQUAL_UINT32(1, t.owed);
    web_conn_close(&t, 6);
    TEST_ASSERT_EQUAL_UINT32(0, t.owed);
    TEST_ASSERT_FALSE(web_conn_take_owed(&t, 8));
    web_conn_close(&t, 8);
    web_conn_open(&t, 6, T0 + 3u);
    web_conn_async(&t, 6, true, T0 + 3u);
    web_conn_open(&t, 7, T0 + 4u);
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 7, MAXS, T0 + 4u));
    TEST_ASSERT_TRUE(web_conn_take_owed(&t, 7));
    web_conn_closing(&t, 7, true);
    TEST_ASSERT_EQUAL_size_t(MAXS - 1u, web_conn_count(&t));
    web_conn_closing(&t, 7, false);                                          /* its close could not start */
    TEST_ASSERT_EQUAL_size_t(MAXS, web_conn_count(&t));

    /* The table never loses track: a number reused without a close notice
     * takes over its old entry. */
    TEST_ASSERT_TRUE(web_conn_open(&t, 3, T0 + 2u));
    TEST_ASSERT_FALSE(t.c[2].ws);
    for (int fd = 8; fd <= (int)WEB_CONN_MAX; fd++) {
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, T0 + 3u));                    /* up to the last entry */
    }
    TEST_ASSERT_FALSE(web_conn_open(&t, (int)WEB_CONN_MAX + 1, T0 + 3u));    /* full */
    TEST_ASSERT_FALSE(web_conn_open(&t, -1, T0 + 3u));
}

/* ---- a deadline armed after the handler -------------------------------------------------------- */

TEST_CASE("web conn: an arm left after the handler never blocks eviction or the next request", "[web_conn]")
{
    /* Six connections, each with a request whose body nobody read: the
     * handler answered and ended it, then the server read the body away,
     * which re-armed the deadline (the arm is stale from then on). */
    web_conns_init(&t, DEADLINE);
    for (int fd = 54; fd <= 59; fd++) {
        uint32_t at = T0 + (uint32_t)(fd - 54) * 100u;
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, at));
        web_conn_received(&t, fd, at);
        web_conn_done(&t, fd, at + 10u, false);
        web_conn_received(&t, fd, at + 20u);
    }
    /* A minute later new connections arrive: the stale ones go, oldest
     * first; the newcomer is never the one closed. */
    for (int i = 0; i < 3; i++) {
        int fd = 60 + i;
        uint32_t now = T0 + 60000u + (uint32_t)i;
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, now));
        TEST_ASSERT_EQUAL_INT(54 + i, web_conn_victim(&t, fd, MAXS, now));
        web_conn_close(&t, 54 + i);
    }
    /* The next request on one of them is bounded afresh, not refused at once. */
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 58, T0 + 60010u));
    web_conn_received(&t, 58, T0 + 60010u);
    TEST_ASSERT_EQUAL_INT32(DEADLINE, web_conn_recv_budget(&t, 58, T0 + 60010u));
}

TEST_CASE("web conn: between requests, an arm still set is stale and dropped", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    web_conn_open(&t, 40, T0);
    web_conn_open(&t, 41, T0);
    web_conn_open(&t, 42, T0);
    web_conn_received(&t, 40, T0 + 1u);
    web_conn_done(&t, 40, T0 + 2u, true);
    web_conn_received(&t, 40, T0 + 3u);                                      /* after its handler */
    web_conn_received(&t, 41, T0 + 4u);
    web_conn_async(&t, 41, true, T0 + 5u);                                   /* on the worker */
    web_conn_websocket(&t, 42, true, T0 + 6u);
    TEST_ASSERT_EQUAL_INT32(0, web_conn_recv_budget(&t, 40, T0 + 3u + DEADLINE));   /* would refuse it */

    web_conn_between_requests(&t);                                           /* the web tick */
    TEST_ASSERT_EQUAL_INT32(-1, web_conn_recv_budget(&t, 40, T0 + 3u + DEADLINE));
    TEST_ASSERT_TRUE(t.c[0].admin);                                          /* what its last request had */
    TEST_ASSERT_EQUAL_UINT32(T0 + 2u, t.c[0].active_ms);                     /* and when it ended */
    TEST_ASSERT_EQUAL_INT(WEB_CONN_ASYNC, t.c[1].phase);                     /* the worker's stays */
    TEST_ASSERT_EQUAL_INT(WEB_CONN_IDLE, t.c[2].phase);
    TEST_ASSERT_TRUE(t.c[2].ws);
    web_conn_received(&t, 40, T0 + 9000u);
    TEST_ASSERT_EQUAL_INT32(DEADLINE, web_conn_recv_budget(&t, 40, T0 + 9000u));
}

/* ---- HTTP version of each request ------------------------------------------------------------ */

static void feed(web_scan_t *s, const char *text)
{
    web_scan_feed(s, text, strlen(text));
}

/* Feed `text` in pieces of `step` bytes (the server reads what has arrived). */
static void feed_split(web_scan_t *s, const char *text, size_t step)
{
    size_t len = strlen(text);
    for (size_t at = 0; at < len; at += step) {
        web_scan_feed(s, text + at, len - at < step ? len - at : step);
    }
}

TEST_CASE("web conn: the request line tells HTTP/1.0 from HTTP/1.1, split anywhere", "[web_conn]")
{
    static const char req10[] = "GET / HTTP/1.0\r\nHost: onodiy.local\r\n\r\n";
    static const char req11[] = "GET /api/status HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    for (size_t step = 1; step <= sizeof(req10); step++) {
        web_scan_t s;
        memset(&s, 0, sizeof(s));
        feed_split(&s, req10, step);
        TEST_ASSERT_EQUAL_UINT8(1, s.queued);
        TEST_ASSERT_TRUE(web_scan_take(&s));
        feed_split(&s, req11, step);
        TEST_ASSERT_FALSE(web_scan_take(&s));
        TEST_ASSERT_FALSE(web_scan_take(&s));   /* nothing noted: HTTP/1.1 */
        TEST_ASSERT_EQUAL(WEB_SCAN_LINE, s.st);
    }
    /* The split falls between "HTTP/1." and "0", and inside the CR LF. */
    web_scan_t s;
    memset(&s, 0, sizeof(s));
    feed(&s, "GET / HTTP/1.");
    TEST_ASSERT_EQUAL_UINT8(0, s.queued);
    feed(&s, "0\r");
    feed(&s, "\nHost: a\r\n\r\n");
    TEST_ASSERT_TRUE(web_scan_take(&s));
    /* Empty lines before a request line are skipped; a bare LF ends lines too. */
    feed(&s, "\r\n\r\nGET /x HTTP/1.0\nHost: a\n\n");
    TEST_ASSERT_TRUE(web_scan_take(&s));
}

TEST_CASE("web conn: pipelined requests keep their own flags, bodies are skipped", "[web_conn]")
{
    web_scan_t s;
    memset(&s, 0, sizeof(s));
    /* A body that looks like a request line must not count as one. */
    feed(&s, "POST /api/wifi HTTP/1.1\r\nContent-Type: application/json\r\ncontent-length: 26\r\n\r\n"
             "GET /fake HTTP/1.0\r\n\r\n1234"
             "GET / HTTP/1.0\r\n\r\n"
             "GET /api/session HTTP/1.1\r\n\r\n");
    TEST_ASSERT_EQUAL_UINT8(3, s.queued);
    TEST_ASSERT_FALSE(web_scan_take(&s));   /* the POST */
    TEST_ASSERT_TRUE(web_scan_take(&s));    /* GET / HTTP/1.0 */
    TEST_ASSERT_FALSE(web_scan_take(&s));   /* GET /api/session */
    TEST_ASSERT_EQUAL_UINT8(0, s.queued);

    /* A body that arrives in pieces, then the next request. */
    memset(&s, 0, sizeof(s));
    feed(&s, "PUT /api/system HTTP/1.0\r\nContent-Length:   10\r\n\r\n12345");
    feed(&s, "6789");
    TEST_ASSERT_EQUAL(WEB_SCAN_BODY, s.st);
    feed(&s, "0GET / HTTP/1.1\r\n\r\n");
    TEST_ASSERT_TRUE(web_scan_take(&s));
    TEST_ASSERT_FALSE(web_scan_take(&s));
    TEST_ASSERT_EQUAL_UINT8(0, s.queued);

    /* A chunked body cannot be followed: the scanner stops (later requests count as HTTP/1.1). */
    memset(&s, 0, sizeof(s));
    feed(&s, "POST /x HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nGET /\r\n");
    TEST_ASSERT_EQUAL(WEB_SCAN_OFF, s.st);
    TEST_ASSERT_TRUE(web_scan_take(&s));
    feed(&s, "GET / HTTP/1.0\r\n\r\n");
    TEST_ASSERT_FALSE(web_scan_take(&s));
    /* So does an unreadable Content-Length. */
    memset(&s, 0, sizeof(s));
    feed(&s, "POST /x HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n");
    TEST_ASSERT_EQUAL(WEB_SCAN_OFF, s.st);
}

static void scan_str(int fd, const char *text)
{
    web_conn_scan(&t, fd, text, strlen(text));
}

TEST_CASE("web conn: each handler takes its request's flag; WebSocket clients are not scanned", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    TEST_ASSERT_TRUE(web_conn_open(&t, 60, T0));
    static const char two[] = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /app.js HTTP/1.1\r\n\r\n";
    web_conn_scan(&t, 60, two, 20);
    web_conn_scan(&t, 60, two + 20, sizeof(two) - 1 - 20);
    TEST_ASSERT_TRUE(web_conn_begin_request(&t, 60));
    web_conn_set_close_after(&t, 60, true);
    TEST_ASSERT_TRUE(web_conn_close_after(&t, 60));
    TEST_ASSERT_FALSE(web_conn_note_close_hdr(&t, 60));   /* first: set the header */
    TEST_ASSERT_TRUE(web_conn_note_close_hdr(&t, 60));    /* then: it is set already */
    web_conn_done(&t, 60, T0 + 10u, false);
    TEST_ASSERT_FALSE(web_conn_begin_request(&t, 60));   /* the second request: HTTP/1.1 */
    TEST_ASSERT_FALSE(web_conn_close_after(&t, 60));      /* reset for each request */
    TEST_ASSERT_FALSE(web_conn_note_close_hdr(&t, 60));

    /* On the worker, the decision stays with the connection until the next request. */
    scan_str(60, "POST /api/upload/app HTTP/1.0\r\nContent-Length: 3\r\n\r\nabc");
    TEST_ASSERT_TRUE(web_conn_begin_request(&t, 60));
    web_conn_set_close_after(&t, 60, true);
    web_conn_async(&t, 60, true, T0 + 20u);
    TEST_ASSERT_TRUE(web_conn_on_worker(&t, 60));
    web_conn_done(&t, 60, T0 + 21u, true);
    TEST_ASSERT_TRUE(web_conn_close_after(&t, 60));
    web_conn_async(&t, 60, false, T0 + 30u);
    TEST_ASSERT_FALSE(web_conn_on_worker(&t, 60));

    /* After the handshake the bytes are frames: nothing is noted. */
    scan_str(60, "GET /ws HTTP/1.1\r\nUpgrade: websocket\r\n\r\n");
    web_conn_websocket(&t, 60, true, T0 + 40u);
    scan_str(60, "GET / HTTP/1.0\r\n\r\n");
    TEST_ASSERT_FALSE(web_conn_begin_request(&t, 60));
    TEST_ASSERT_EQUAL_UINT8(0, t.c[0].scan.queued);

    /* Unknown connections: nothing to take, nothing kept. */
    scan_str(61, "GET / HTTP/1.0\r\n\r\n");
    TEST_ASSERT_FALSE(web_conn_begin_request(&t, 61));
    TEST_ASSERT_FALSE(web_conn_close_after(&t, 61));
    TEST_ASSERT_FALSE(web_conn_note_close_hdr(&t, 61));
    TEST_ASSERT_FALSE(web_conn_on_worker(&t, 61));
}

TEST_CASE("web conn: the HTTP/1.0 flag follows pipelined requests and stays for the request's lifetime",
          "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    TEST_ASSERT_TRUE(web_conn_open(&t, 70, T0));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 70));   /* nothing taken yet */
    scan_str(70, "GET /api/status HTTP/1.1\r\n\r\nGET /api/logs?format=text HTTP/1.0\r\n\r\n"
                 "GET /api/status HTTP/1.1\r\n\r\n");
    /* First request: HTTP/1.1, for as long as it is answered. */
    TEST_ASSERT_FALSE(web_conn_begin_request(&t, 70));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 70));
    web_conn_done(&t, 70, T0 + 1u, true);
    /* Second: HTTP/1.0, also while the worker answers it and after its handler is done. */
    TEST_ASSERT_TRUE(web_conn_begin_request(&t, 70));
    TEST_ASSERT_TRUE(web_conn_http10(&t, 70));
    web_conn_async(&t, 70, true, T0 + 2u);
    web_conn_done(&t, 70, T0 + 3u, true);
    TEST_ASSERT_TRUE(web_conn_http10(&t, 70));
    TEST_ASSERT_TRUE(web_conn_http10(&t, 70));   /* reading it takes nothing */
    web_conn_async(&t, 70, false, T0 + 4u);
    TEST_ASSERT_TRUE(web_conn_http10(&t, 70));   /* until the next request's handler starts */
    /* Third: HTTP/1.1 again. */
    TEST_ASSERT_FALSE(web_conn_begin_request(&t, 70));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 70));
    /* A connection is its own: another one, and an unknown fd, are not HTTP/1.0. */
    TEST_ASSERT_TRUE(web_conn_open(&t, 71, T0));
    scan_str(71, "GET / HTTP/1.0\r\n\r\n");
    TEST_ASSERT_FALSE(web_conn_http10(&t, 71));   /* not taken by a handler yet */
    TEST_ASSERT_TRUE(web_conn_begin_request(&t, 71));
    TEST_ASSERT_TRUE(web_conn_http10(&t, 71));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 70));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 99));
    /* A closed and reopened number starts afresh. */
    web_conn_close(&t, 71);
    TEST_ASSERT_TRUE(web_conn_open(&t, 71, T0 + 5u));
    TEST_ASSERT_FALSE(web_conn_http10(&t, 71));
}

/* ---- lingering close: the drain ------------------------------------------------------------ */

/*
 * A model of what the drain sees of an lwIP TCP socket (lwIP 2.2, as in
 * ESP-IDF v6.1), from the client's side: data segments that arrive at given
 * times, then possibly the client's FIN.
 *  - FIONREAD (queued) counts data arrived and not read, never the FIN: the
 *    FIN reaches the socket as a zero-length entry (api_msg.c, recv_tcp).
 *  - recv() (drop) copies queued data, at most n and the caller's 512-byte
 *    buffer. A call that finds data returns it and leaves a FIN behind it for
 *    the next call (sockets.c lwip_recv_tcp, NETCONN_NOFIN); a call that
 *    finds no data but the FIN returns 0 and takes it, which shuts lwIP's
 *    receive side (api_lib.c, netconn_recv_data_tcp): `fin_taken`. With
 *    neither, -1 (EWOULDBLOCK).
 *  - readable: data unread or the FIN there; otherwise the clock moves on
 *    to the next arrival, or by the whole wait.
 */
#define DRAIN_MAX_BYTES (64u * 1024u)   /* web.c LINGER_DRAIN_BYTES */
#define DRAIN_MAX_MS    2000u           /* web.c LINGER_DRAIN_MS */
#define DRAIN_BUF       512u            /* web.c drain_drop's buffer */
#define NO_FIN          UINT32_MAX

typedef struct {
    uint32_t now;
    size_t segs;
    uint32_t seg_at[64];       /* ms after the start */
    size_t seg_len[64];
    uint32_t fin_at;           /* ms after the start (after the last segment), NO_FIN = never */
    uint32_t start;
    size_t read;               /* data bytes read */
    int last_queued;           /* what queued() reported last */
    bool queued_fails;
    bool fin_taken;            /* a recv() returned the FIN */
    bool over_asked;           /* drop() asked for more than queued() had reported */
    unsigned drops;
} sock_model_t;

static size_t model_arrived(const sock_model_t *m)
{
    size_t n = 0;
    for (size_t i = 0; i < m->segs; i++) {
        if ((uint32_t)(m->now - m->start) >= m->seg_at[i]) {
            n += m->seg_len[i];
        }
    }
    return n;
}

static bool model_fin_in(const sock_model_t *m)
{
    return m->fin_at != NO_FIN && (uint32_t)(m->now - m->start) >= m->fin_at;
}

static bool model_readable(void *ctx, uint32_t wait_ms)
{
    sock_model_t *m = ctx;
    if (model_arrived(m) > m->read || model_fin_in(m)) {
        return true;
    }
    uint32_t at = (uint32_t)(m->now - m->start);
    uint32_t next = UINT32_MAX;
    for (size_t i = 0; i < m->segs; i++) {
        if (m->seg_at[i] > at && m->seg_at[i] < next) {
            next = m->seg_at[i];
        }
    }
    if (m->fin_at != NO_FIN && m->fin_at > at && m->fin_at < next) {
        next = m->fin_at;
    }
    if (next != UINT32_MAX && next - at <= wait_ms) {
        m->now = m->start + next;
        return true;
    }
    m->now += wait_ms;
    return false;
}

static int model_queued(void *ctx)
{
    sock_model_t *m = ctx;
    m->last_queued = m->queued_fails ? -1 : (int)(model_arrived(m) - m->read);
    return m->last_queued;
}

static int model_drop(void *ctx, size_t n)
{
    sock_model_t *m = ctx;
    m->drops++;
    if (m->last_queued < 0 || n > (size_t)m->last_queued || n == 0) {
        m->over_asked = true;
    }
    size_t avail = model_arrived(m) - m->read;
    if (avail == 0) {
        if (model_fin_in(m)) {
            m->fin_taken = true;
            return 0;
        }
        return -1;
    }
    size_t got = n < avail ? n : avail;
    got = got < DRAIN_BUF ? got : DRAIN_BUF;
    m->read += got;
    return (int)got;
}

static uint32_t model_now(void *ctx)
{
    return ((sock_model_t *)ctx)->now;
}

static void model_init(sock_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->start = m->now = T0;   /* the clock wraps during the drain */
    m->fin_at = NO_FIN;
}

static void model_seg(sock_model_t *m, uint32_t at_ms, size_t len)
{
    TEST_ASSERT_LESS_THAN_size_t(64, m->segs);
    m->seg_at[m->segs] = at_ms;
    m->seg_len[m->segs] = len;
    m->segs++;
}

static web_drain_end_t model_drain(sock_model_t *m, size_t *dropped)
{
    web_drain_io_t io = { .readable = model_readable, .queued = model_queued, .drop = model_drop,
                          .now_ms = model_now, .ctx = m };
    return web_conn_drain(&io, DRAIN_MAX_BYTES, DRAIN_MAX_MS, dropped);
}

TEST_CASE("web conn drain: a client that half-closed after its body: the body is read, its FIN is not",
          "[web_conn]")
{
    /* The client's FIN is queued behind its 2-byte body before the drain starts
     * (the QEMU smoke's half-closing client). */
    sock_model_t m;
    model_init(&m);
    model_seg(&m, 0, 2);
    m.fin_at = 0;
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(WEB_DRAIN_CLOSED, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(2, dropped);
    TEST_ASSERT_FALSE(m.fin_taken);
    TEST_ASSERT_FALSE(m.over_asked);
    TEST_ASSERT_EQUAL_UINT32(T0, m.now);   /* no waiting */

    /* Only the FIN: nothing is read at all. */
    model_init(&m);
    m.fin_at = 0;
    TEST_ASSERT_EQUAL(WEB_DRAIN_CLOSED, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_EQUAL_UINT(0, m.drops);
    TEST_ASSERT_FALSE(m.fin_taken);
}

TEST_CASE("web conn drain: a body arriving in segments is read whole, the FIN after it is not", "[web_conn]")
{
    /* 16 KB in 1460-byte segments every 5 ms, then the FIN 5 ms after the last:
     * more than one buffer per segment, and reads that meet a segment half read. */
    sock_model_t m;
    model_init(&m);
    size_t total = 16u * 1024u;
    uint32_t at = 0;
    for (size_t left = total; left > 0; at += 5u) {
        size_t len = left < 1460u ? left : 1460u;
        model_seg(&m, at, len);
        left -= len;
    }
    m.fin_at = at;
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(WEB_DRAIN_CLOSED, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(total, dropped);
    TEST_ASSERT_FALSE(m.fin_taken);
    TEST_ASSERT_FALSE(m.over_asked);
    TEST_ASSERT_EQUAL_UINT32(T0 + at, m.now);   /* ended when the FIN came */

    /* The FIN in the same instant as the last segment. */
    model_init(&m);
    model_seg(&m, 0, 1000u);
    model_seg(&m, 40u, 3000u);
    m.fin_at = 40u;
    TEST_ASSERT_EQUAL(WEB_DRAIN_CLOSED, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(4000u, dropped);
    TEST_ASSERT_FALSE(m.fin_taken);
    TEST_ASSERT_FALSE(m.over_asked);
}

TEST_CASE("web conn drain: bounded by bytes and by time, the FIN still unread", "[web_conn]")
{
    /* More than the limit queued, FIN behind it: stops at exactly the limit. */
    sock_model_t m;
    model_init(&m);
    model_seg(&m, 0, DRAIN_MAX_BYTES + 5000u);
    m.fin_at = 0;
    size_t dropped = 0;
    TEST_ASSERT_EQUAL(WEB_DRAIN_LIMIT, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(DRAIN_MAX_BYTES, dropped);
    TEST_ASSERT_FALSE(m.fin_taken);
    TEST_ASSERT_FALSE(m.over_asked);

    /* A client that sends nothing more and keeps its side open: the time limit. */
    model_init(&m);
    TEST_ASSERT_EQUAL(WEB_DRAIN_TIMEOUT, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_EQUAL_UINT32(T0 + DRAIN_MAX_MS, m.now);

    /* A trickle: 100 bytes every 500 ms; what came within the time is read. */
    model_init(&m);
    for (uint32_t at = 500u; at <= 3000u; at += 500u) {
        model_seg(&m, at, 100u);
    }
    m.fin_at = 3000u;
    TEST_ASSERT_EQUAL(WEB_DRAIN_TIMEOUT, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(400u, dropped);
    TEST_ASSERT_EQUAL_UINT32(T0 + DRAIN_MAX_MS, m.now);
    TEST_ASSERT_FALSE(m.fin_taken);
    TEST_ASSERT_FALSE(m.over_asked);
}

TEST_CASE("web conn drain: an error ends it", "[web_conn]")
{
    sock_model_t m;
    model_init(&m);
    model_seg(&m, 0, 10u);
    m.queued_fails = true;
    size_t dropped = 1;
    TEST_ASSERT_EQUAL(WEB_DRAIN_ERROR, model_drain(&m, &dropped));
    TEST_ASSERT_EQUAL_size_t(0, dropped);
    TEST_ASSERT_EQUAL_UINT(0, m.drops);
    TEST_ASSERT_EQUAL(WEB_DRAIN_ERROR, model_drain(&m, NULL));   /* `dropped` may be NULL */
}
