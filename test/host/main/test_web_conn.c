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
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 15, MAXS, T0 + 1000u));    /* six open: room left */
    TEST_ASSERT_TRUE(web_conn_open(&t, 16, T0 + 1000u));                     /* the seventh */
    TEST_ASSERT_EQUAL_INT(13, web_conn_victim(&t, 16, MAXS, T0 + 1000u));
    TEST_ASSERT_EQUAL_size_t(MAXS - 1u, web_conn_count(&t));
    /* The closing one is not chosen twice, and while it goes there is room. */
    TEST_ASSERT_EQUAL_INT(-1, web_conn_victim(&t, 16, MAXS, T0 + 1001u));
    web_conn_close(&t, 13);
    TEST_ASSERT_TRUE(web_conn_open(&t, 17, T0 + 2000u));
    TEST_ASSERT_EQUAL_INT(14, web_conn_victim(&t, 17, MAXS, T0 + 2000u));
}

TEST_CASE("web conn: an admin's WebSocket is never closed for someone else's socket", "[web_conn]")
{
    fill();
    /* A client opening sockets one after another (and even sending
     * requests on them) only ever pushes out plain connections: its own,
     * oldest first. The admin's page, its keep-alive connection and the
     * viewer stay. */
    int oldest_own = 16;
    for (int i = 0; i < 20; i++) {
        int fd = 16 + i;
        uint32_t now = T0 + 3000u + (uint32_t)i * 10u;
        TEST_ASSERT_TRUE(web_conn_open(&t, fd, now));
        int v = web_conn_victim(&t, fd, MAXS, now);
        if (i < 3) {
            TEST_ASSERT_EQUAL_INT(13 + i, v);                                /* the idle plain ones first */
        } else {
            TEST_ASSERT_EQUAL_INT(oldest_own++, v);
        }
        web_conn_close(&t, v);
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
    web_conn_received(&t, 6, T0);
    web_conn_open(&t, 7, T0 + 10u);
    TEST_ASSERT_EQUAL_INT(3, web_conn_victim(&t, 7, MAXS, T0 + 10u));
    web_conn_close(&t, 3);

    web_conn_websocket(&t, 7, false, T0 + 11u);                              /* a second viewer */
    web_conn_open(&t, 8, T0 + 20u);
    TEST_ASSERT_EQUAL_INT(2, web_conn_victim(&t, 8, MAXS, T0 + 20u));        /* the older viewer */
}

TEST_CASE("web conn: nothing else may go: the new connection itself is closed", "[web_conn]")
{
    web_conns_init(&t, DEADLINE);
    for (int fd = 1; fd <= 6; fd++) {
        web_conn_open(&t, fd, T0);
    }
    for (int fd = 1; fd <= 4; fd++) {
        web_conn_websocket(&t, fd, true, T0);                                /* four admin pages */
    }
    web_conn_async(&t, 5, true, T0);                                         /* an upload */
    web_conn_received(&t, 6, T0);                                            /* a request being received */
    web_conn_open(&t, 7, T0 + 1u);
    TEST_ASSERT_EQUAL_INT(7, web_conn_victim(&t, 7, MAXS, T0 + 1u));
    TEST_ASSERT_EQUAL_size_t(MAXS - 1u, web_conn_count(&t));
    web_conn_closing(&t, 7, false);                                          /* its close could not start */
    TEST_ASSERT_EQUAL_size_t(MAXS, web_conn_count(&t));

    /* The table never loses track: a number reused without a close notice
     * takes over its old entry. */
    TEST_ASSERT_TRUE(web_conn_open(&t, 3, T0 + 2u));
    TEST_ASSERT_FALSE(t.c[2].ws);
    TEST_ASSERT_TRUE(web_conn_open(&t, 8, T0 + 3u));                         /* the eighth entry */
    TEST_ASSERT_FALSE(web_conn_open(&t, 9, T0 + 3u));                        /* full */
    TEST_ASSERT_FALSE(web_conn_open(&t, -1, T0 + 3u));
}
