/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "reboot_gate.h"
#include "unity.h"

#define F_RETRY 1u
#define F_CLEAR 2u

TEST_CASE("reboot gate commits at once when nothing is inhibited", "[reboot_gate]")
{
    reboot_gate_t g;
    uint32_t apply = 99;
    reboot_gate_init(&g, 4);
    TEST_ASSERT_EQUAL(REBOOT_GATE_COMMIT, reboot_gate_request(&g, 5, F_RETRY, &apply));
    TEST_ASSERT_TRUE(g.rebooting);
    TEST_ASSERT_EQUAL_UINT32(5, g.reason);
    TEST_ASSERT_EQUAL_UINT32(F_RETRY, apply);

    /* Committed: nothing can take an inhibit any more, and later requests
     * join the reboot in progress instead of starting a second one. */
    TEST_ASSERT_EQUAL_INT(REBOOT_GATE_TOKEN_REBOOTING, reboot_gate_acquire(&g, "late"));
    TEST_ASSERT_NULL(reboot_gate_holder(&g));
    TEST_ASSERT_EQUAL(REBOOT_GATE_BUSY, reboot_gate_request(&g, 6, F_CLEAR, &apply));
    TEST_ASSERT_EQUAL_UINT32(F_CLEAR, apply);
    TEST_ASSERT_EQUAL(REBOOT_GATE_BUSY, reboot_gate_now(&g, 7, 0, true, &apply));
    TEST_ASSERT_EQUAL_UINT32(5, g.reason);
}

TEST_CASE("reboot gate queues behind inhibits and commits on the last release", "[reboot_gate]")
{
    reboot_gate_t g;
    uint32_t apply = 99;
    reboot_gate_init(&g, 4);
    int a = reboot_gate_acquire(&g, "receiver flash");
    int b = reboot_gate_acquire(&g, "app write");
    TEST_ASSERT_GREATER_THAN_INT(0, a);
    TEST_ASSERT_GREATER_THAN_INT(0, b);
    TEST_ASSERT_NOT_EQUAL(a, b);

    TEST_ASSERT_EQUAL(REBOOT_GATE_QUEUED, reboot_gate_request(&g, 8, F_RETRY, &apply));
    TEST_ASSERT_EQUAL_UINT32(0, apply);              /* nothing applied while queued */
    TEST_ASSERT_EQUAL(REBOOT_GATE_QUEUED, reboot_gate_request(&g, 9, F_CLEAR, &apply));
    TEST_ASSERT_EQUAL_UINT32(0, apply);
    TEST_ASSERT_EQUAL(REBOOT_GATE_REFUSED, reboot_gate_now(&g, 10, 0, false, &apply));
    TEST_ASSERT_FALSE(g.rebooting);

    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, a, &apply));
    TEST_ASSERT_FALSE(g.rebooting);
    TEST_ASSERT_EQUAL(REBOOT_GATE_COMMIT, reboot_gate_release(&g, b, &apply));
    TEST_ASSERT_TRUE(g.rebooting);
    TEST_ASSERT_EQUAL_UINT32(8, g.reason);           /* first queued reason wins */
    TEST_ASSERT_EQUAL_UINT32(F_RETRY | F_CLEAR, apply);
    TEST_ASSERT_EQUAL_UINT32(0, g.queued_reason);
}

TEST_CASE("reboot gate: a refused reboot changes nothing", "[reboot_gate]")
{
    reboot_gate_t g;
    uint32_t apply = 99;
    reboot_gate_init(&g, 4);
    int a = reboot_gate_acquire(&g, "verify window");

    /* "Clear safe mode" while inhibited: refused, nothing queued, nothing to apply. */
    TEST_ASSERT_EQUAL(REBOOT_GATE_REFUSED, reboot_gate_now(&g, 11, F_CLEAR, false, &apply));
    TEST_ASSERT_EQUAL_UINT32(0, apply);
    TEST_ASSERT_EQUAL_UINT32(0, g.queued_reason);
    TEST_ASSERT_EQUAL_UINT32(0, g.queued_flags);
    TEST_ASSERT_EQUAL_STRING("verify window", reboot_gate_holder(&g));

    /* Its release then has nothing to run. */
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, a, &apply));
    TEST_ASSERT_FALSE(g.rebooting);
    TEST_ASSERT_NULL(reboot_gate_holder(&g));
}

TEST_CASE("reboot gate: a forced reboot overrides inhibits and absorbs the queue", "[reboot_gate]")
{
    reboot_gate_t g;
    uint32_t apply = 99;
    reboot_gate_init(&g, 4);
    int a = reboot_gate_acquire(&g, "receiver flash");
    TEST_ASSERT_EQUAL(REBOOT_GATE_QUEUED, reboot_gate_request(&g, 3, F_RETRY, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_COMMIT, reboot_gate_now(&g, 12, F_CLEAR, true, &apply));
    TEST_ASSERT_EQUAL_UINT32(12, g.reason);
    TEST_ASSERT_EQUAL_UINT32(F_RETRY | F_CLEAR, apply);
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, a, &apply));
}

TEST_CASE("reboot gate checks inhibit slots and tokens", "[reboot_gate]")
{
    reboot_gate_t g;
    uint32_t apply = 99;
    reboot_gate_init(&g, 2);
    int a = reboot_gate_acquire(&g, "one");
    int b = reboot_gate_acquire(&g, "two");
    TEST_ASSERT_EQUAL_INT(REBOOT_GATE_TOKEN_FULL, reboot_gate_acquire(&g, "three"));

    /* Foreign, out-of-range and repeated releases are ignored. */
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, 0, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, -1, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, 3, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_QUEUED, reboot_gate_request(&g, 4, 0, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, a, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_NONE, reboot_gate_release(&g, a, &apply));
    TEST_ASSERT_FALSE(g.rebooting);                  /* "two" still holds it */
    TEST_ASSERT_EQUAL(REBOOT_GATE_COMMIT, reboot_gate_release(&g, b, &apply));

    /* No reason is no request. */
    reboot_gate_init(&g, 2);
    TEST_ASSERT_EQUAL(REBOOT_GATE_REFUSED, reboot_gate_request(&g, 0, F_RETRY, &apply));
    TEST_ASSERT_EQUAL(REBOOT_GATE_REFUSED, reboot_gate_now(&g, 0, 0, true, &apply));
    TEST_ASSERT_FALSE(g.rebooting);

    /* Slot count is clamped. */
    reboot_gate_init(&g, 100);
    TEST_ASSERT_EQUAL_UINT(REBOOT_GATE_MAX_INHIBITS, g.limit);
    reboot_gate_init(&g, 0);
    TEST_ASSERT_EQUAL_UINT(1, g.limit);
}
