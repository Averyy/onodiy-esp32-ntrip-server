/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "boot_guard.h"
#include "unity.h"

#define LIMIT 5u

static boot_guard_result_t boot(boot_guard_state_t *st, boot_hw_reset_t hw)
{
    boot_guard_result_t res;
    boot_guard_on_boot(st, hw, LIMIT, &res);
    return res;
}

static void fresh(boot_guard_state_t *st)
{
    memset(st, 0xA5, sizeof(*st));   /* RTC memory is random after power-up */
    boot_guard_result_t res = boot(st, BOOT_HW_POWER_ON);
    TEST_ASSERT_EQUAL(BOOT_RESET_POWER_ON, res.cls);
}

TEST_CASE("boot guard starts over on power-on and on corrupt state", "[boot_guard]")
{
    boot_guard_state_t st;
    memset(&st, 0x5A, sizeof(st));
    boot_guard_result_t res = boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_TRUE(res.state_was_lost);
    TEST_ASSERT_EQUAL(BOOT_RESET_CRASH, res.cls);
    TEST_ASSERT_EQUAL_UINT32(1, st.crashes);
    TEST_ASSERT_TRUE(boot_guard_valid(&st));

    st.crashes = 3;                  /* tamper without resealing */
    res = boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_TRUE(res.state_was_lost);
    TEST_ASSERT_EQUAL_UINT32(1, st.crashes);

    res = boot(&st, BOOT_HW_POWER_ON);
    TEST_ASSERT_FALSE(res.state_was_lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);
    TEST_ASSERT_EQUAL_UINT32(1, st.boots);
}

TEST_CASE("boot guard enters safe mode at the crash limit", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    for (uint32_t i = 1; i < LIMIT; i++) {
        boot_guard_result_t res = boot(&st, BOOT_HW_CRASH);
        TEST_ASSERT_FALSE(res.safe_mode);
    }
    boot_guard_result_t res = boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_TRUE(res.safe_mode);
    TEST_ASSERT_TRUE(res.entered_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_episodes);
    TEST_ASSERT_EQUAL_UINT32(3600, boot_guard_safe_retry_s(&st));

    /* A planned reboot inside safe mode stays in safe mode, same episode. */
    boot_guard_set_reboot_reason(&st, 1);
    res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL(BOOT_RESET_PLANNED, res.cls);
    TEST_ASSERT_TRUE(res.safe_mode);
    TEST_ASSERT_FALSE(res.entered_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_episodes);
}

TEST_CASE("boot guard never counts brownouts or planned reboots", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    for (int i = 0; i < 20; i++) {
        boot_guard_result_t res = boot(&st, BOOT_HW_BROWNOUT);
        TEST_ASSERT_EQUAL(BOOT_RESET_BROWNOUT, res.cls);
        TEST_ASSERT_FALSE(res.safe_mode);
    }
    TEST_ASSERT_EQUAL_UINT32(20, st.brownouts);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);

    boot_guard_set_reboot_reason(&st, 7);
    boot_guard_result_t res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL(BOOT_RESET_PLANNED, res.cls);
    TEST_ASSERT_EQUAL_UINT32(7, res.prev_reboot_reason);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);

    /* The reason is consumed: an unplanned restart after it is "other". */
    res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL(BOOT_RESET_OTHER, res.cls);
    TEST_ASSERT_EQUAL_UINT32(0, res.prev_reboot_reason);
}

TEST_CASE("boot guard counts a failed-boot reboot as a crash", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    boot_guard_set_reboot_reason(&st, 12u | BOOT_GUARD_REASON_CRASH);
    boot_guard_result_t res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL(BOOT_RESET_CRASH, res.cls);
    TEST_ASSERT_EQUAL_UINT32(1, st.crashes);
}

TEST_CASE("boot guard clears on a stable normal boot only", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    boot(&st, BOOT_HW_CRASH);
    boot(&st, BOOT_HW_CRASH);
    boot_guard_on_stable(&st);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);

    for (uint32_t i = 0; i < LIMIT; i++) {
        boot(&st, BOOT_HW_CRASH);
    }
    TEST_ASSERT_TRUE(st.safe_mode);
    boot_guard_on_stable(&st);       /* stable in safe mode: nothing cleared */
    TEST_ASSERT_EQUAL_UINT32(LIMIT, st.crashes);
    TEST_ASSERT_TRUE(boot(&st, BOOT_HW_OTHER).safe_mode);
}

TEST_CASE("boot guard retries a normal boot with growing intervals", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    for (uint32_t i = 0; i < LIMIT; i++) {
        boot(&st, BOOT_HW_CRASH);
    }
    TEST_ASSERT_EQUAL_UINT32(3600, boot_guard_safe_retry_s(&st));

    /* Episode over: retry. The retry boot is normal... */
    boot_guard_prepare_safe_retry(&st, LIMIT);
    boot_guard_set_reboot_reason(&st, 9);
    boot_guard_result_t res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_FALSE(res.safe_mode);

    /* ...and one early crash puts it straight back, with a longer episode. */
    res = boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_TRUE(res.entered_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(4u * 3600u, boot_guard_safe_retry_s(&st));

    boot_guard_prepare_safe_retry(&st, LIMIT);
    boot(&st, BOOT_HW_SOFTWARE);
    boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_EQUAL_UINT32(24u * 3600u, boot_guard_safe_retry_s(&st));

    /* A retry that reaches stable uptime ends it all. */
    boot_guard_prepare_safe_retry(&st, LIMIT);
    res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_FALSE(res.safe_mode);
    boot_guard_on_stable(&st);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);
    TEST_ASSERT_EQUAL_UINT32(0, st.safe_episodes);
}

TEST_CASE("boot guard one-shot flags last until power-on", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    TEST_ASSERT_TRUE(boot_guard_claim_once(&st, 1u));
    TEST_ASSERT_FALSE(boot_guard_claim_once(&st, 1u));
    TEST_ASSERT_TRUE(boot_guard_claim_once(&st, 2u));
    boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_FALSE(boot_guard_claim_once(&st, 1u));
    boot(&st, BOOT_HW_POWER_ON);
    TEST_ASSERT_TRUE(boot_guard_claim_once(&st, 1u));
}

static void enter_safe_mode(boot_guard_state_t *st)
{
    for (uint32_t i = 0; i < LIMIT; i++) {
        boot(st, BOOT_HW_CRASH);
    }
    TEST_ASSERT_TRUE(st->safe_mode);
}

TEST_CASE("boot guard: brownouts in safe mode change neither counter nor episode", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    enter_safe_mode(&st);
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_episodes);
    for (int i = 0; i < 5; i++) {
        boot_guard_result_t res = boot(&st, BOOT_HW_BROWNOUT);
        TEST_ASSERT_EQUAL(BOOT_RESET_BROWNOUT, res.cls);
        TEST_ASSERT_TRUE(res.safe_mode);
        TEST_ASSERT_FALSE(res.entered_safe_mode);
    }
    TEST_ASSERT_EQUAL_UINT32(LIMIT, st.crashes);
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_episodes);
    TEST_ASSERT_EQUAL_UINT32(5, st.brownouts);
    TEST_ASSERT_EQUAL_UINT32(3600, boot_guard_safe_retry_s(&st));
}

TEST_CASE("boot guard: a crash after the retry was prepared stays in the same episode", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    enter_safe_mode(&st);

    /* Retry prepared (counter at LIMIT-1), but the safe boot crashes before
     * its planned reboot: back in safe mode, same episode and interval. */
    boot_guard_prepare_safe_retry(&st, LIMIT);
    TEST_ASSERT_EQUAL_UINT32(LIMIT - 1, st.crashes);
    boot_guard_result_t res = boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_TRUE(res.safe_mode);
    TEST_ASSERT_FALSE(res.entered_safe_mode);
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_episodes);
    TEST_ASSERT_EQUAL_UINT32(3600, boot_guard_safe_retry_s(&st));
}

TEST_CASE("boot guard keeps 24 h retries beyond the third episode", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    enter_safe_mode(&st);
    for (uint32_t episode = 2; episode <= 10; episode++) {
        boot_guard_prepare_safe_retry(&st, LIMIT);
        boot_guard_set_reboot_reason(&st, 9);
        TEST_ASSERT_FALSE(boot(&st, BOOT_HW_SOFTWARE).safe_mode);
        boot_guard_result_t res = boot(&st, BOOT_HW_CRASH);
        TEST_ASSERT_TRUE(res.entered_safe_mode);
        TEST_ASSERT_EQUAL_UINT32(episode, st.safe_episodes);
        TEST_ASSERT_EQUAL_UINT32(episode == 2 ? 4u * 3600u : 24u * 3600u, boot_guard_safe_retry_s(&st));
    }

    /* The user's clear ends it all. */
    boot_guard_clear(&st);
    boot_guard_set_reboot_reason(&st, 10);
    TEST_ASSERT_FALSE(boot(&st, BOOT_HW_SOFTWARE).safe_mode);
    TEST_ASSERT_EQUAL_UINT32(0, st.safe_episodes);
}
