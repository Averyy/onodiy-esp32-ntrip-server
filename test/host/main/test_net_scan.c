/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "net_scan.h"
#include "unity.h"

TEST_CASE("scan ownership: a stop request closes admission until stop and the next start event", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    net_scan_stopping(&s, true);
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 1));
    net_scan_started(&s);   /* delayed start of the lifetime being stopped */
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 2));
    TEST_ASSERT_TRUE(net_scan_stopped(&s));
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_ROAM, 3));
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 4));
    net_scan_notify(&s, 0, 5);
    TEST_ASSERT_TRUE(net_scan_take_done(&s).success);
}

TEST_CASE("scan ownership: recovery after a failed initial start does not wait for a nonexistent lifetime", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_stopping(&s, false);   /* stop with no successful initial start */
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    net_scan_stopping(&s, true);
    net_scan_stopping(&s, false);   /* a repeated stop cannot release the earlier barrier */
    net_scan_started(&s);
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 1));
    TEST_ASSERT_TRUE(net_scan_stopped(&s));
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 2));
}

TEST_CASE("scan ownership: a user waits behind roaming without starting a second watchdog", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_ROAM, 100));
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 101));
    TEST_ASSERT_EQUAL(NET_SCAN_STOP, net_scan_tick(&s, 100 + NET_SCAN_WATCHDOG_MS));
    TEST_ASSERT_TRUE(s.user_queued);
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 100 + NET_SCAN_WATCHDOG_MS));
    net_scan_notify(&s, 0, 101 + NET_SCAN_WATCHDOG_MS);   /* stop-generated completion is not a successful result */
    net_scan_done_t done = net_scan_take_done(&s);
    TEST_ASSERT_EQUAL(NET_SCAN_ROAM, done.owner);
    TEST_ASSERT_TRUE(done.cancelled);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 102 + NET_SCAN_WATCHDOG_MS));
    TEST_ASSERT_EQUAL(NET_SCAN_WAIT, net_scan_tick(&s, 103 + NET_SCAN_WATCHDOG_MS));
    net_scan_notify(&s, 0, 104 + NET_SCAN_WATCHDOG_MS);
    done = net_scan_take_done(&s);
    TEST_ASSERT_TRUE(done.success);
    TEST_ASSERT_EQUAL(NET_SCAN_USER, done.owner);
    TEST_ASSERT_EQUAL_UINT32(2, done.duration_ms);
}

TEST_CASE("scan ownership: status survives delayed consumption and a failed result is never success", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_ROAM, 1));
    net_scan_notify(&s, 1, 11);
    /* Completion is retained independently of the network task's wakeup queue. */
    TEST_ASSERT_EQUAL(NET_SCAN_WAIT, net_scan_tick(&s, NET_SCAN_WATCHDOG_MS + 10));
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_ROAM, NET_SCAN_WATCHDOG_MS + 10));
    net_scan_done_t done = net_scan_take_done(&s);
    TEST_ASSERT_TRUE(done.ready);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_FALSE(done.cancelled);
    TEST_ASSERT_EQUAL_UINT32(1, done.status);
    TEST_ASSERT_EQUAL_UINT32(10, done.duration_ms);   /* queue delay is not radio time */
    TEST_ASSERT_FALSE(net_scan_take_done(&s).ready);
}

TEST_CASE("scan ownership: missing cancellation ends a queued request without reassigning late events", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_started(&s);
    uint32_t t = UINT32_MAX - 100u;
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_ROAM, t));
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    t += NET_SCAN_WATCHDOG_MS;
    TEST_ASSERT_EQUAL(NET_SCAN_STOP, net_scan_tick(&s, t));
    TEST_ASSERT_EQUAL(NET_SCAN_WAIT, net_scan_tick(&s, t + NET_SCAN_CANCEL_MS - 1));
    t += NET_SCAN_CANCEL_MS;
    TEST_ASSERT_EQUAL(NET_SCAN_GIVE_UP, net_scan_tick(&s, t));
    TEST_ASSERT_FALSE(s.user_queued);
    TEST_ASSERT_FALSE(net_scan_radio_busy(&s));   /* reconnecting the link is still allowed */
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_ROAM, t + 1));
    TEST_ASSERT_EQUAL(NET_SCAN_WAIT, net_scan_tick(&s, t + 1));
    net_scan_notify(&s, 1, t + 2);
    net_scan_done_t done = net_scan_take_done(&s);
    TEST_ASSERT_TRUE(done.cancelled);
    TEST_ASSERT_EQUAL(NET_SCAN_ROAM, done.owner);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, t + 3));
}

TEST_CASE("scan ownership: duplicates fail closed, and a station stop establishes a new lifetime", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_ROAM, 1));
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    net_scan_notify(&s, 0, 19);
    net_scan_notify(&s, 1, 20);
    net_scan_done_t done = net_scan_take_done(&s);
    TEST_ASSERT_TRUE(done.ambiguous);
    TEST_ASSERT_TRUE(done.user_cancelled);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    net_scan_notify(&s, 0, 21);   /* another unowned event does not prove the radio is free */
    done = net_scan_take_done(&s);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_stopped(&s));
    TEST_ASSERT_FALSE(net_scan_request_user(&s));
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 22));
    TEST_ASSERT_TRUE(net_scan_stopped(&s));
    TEST_ASSERT_FALSE(net_scan_user_busy(&s));
}

TEST_CASE("scan ownership: failed starts and unowned completions cannot finish a new scan", "[net_scan]")
{
    net_scan_t s;
    net_scan_init(&s);
    net_scan_started(&s);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 0));
    net_scan_start_failed(&s);
    TEST_ASSERT_FALSE(net_scan_user_busy(&s));
    net_scan_notify(&s, 1, 1);
    TEST_ASSERT_TRUE(net_scan_request_user(&s));
    TEST_ASSERT_FALSE(net_scan_begin(&s, NET_SCAN_USER, 1));
    net_scan_done_t done = net_scan_take_done(&s);
    TEST_ASSERT_EQUAL(NET_SCAN_NONE, done.owner);
    TEST_ASSERT_FALSE(done.success);
    TEST_ASSERT_TRUE(net_scan_begin(&s, NET_SCAN_USER, 3));
    net_scan_notify(&s, 0, 4);
    done = net_scan_take_done(&s);
    TEST_ASSERT_TRUE(done.success);
    TEST_ASSERT_EQUAL(NET_SCAN_USER, done.owner);
}
