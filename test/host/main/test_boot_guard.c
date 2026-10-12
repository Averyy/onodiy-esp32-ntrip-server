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

TEST_CASE("boot guard: a boot waiting to restart after a fatal boot error is never stable", "[boot_guard]")
{
    /* Two failed boots; the third fails again and waits out its retry delay,
     * longer than the stable uptime: the counter must keep counting. */
    boot_guard_state_t st;
    fresh(&st);
    for (int i = 0; i < 2; i++) {
        boot_guard_set_reboot_reason(&st, 12u | BOOT_GUARD_REASON_CRASH);
        boot(&st, BOOT_HW_SOFTWARE);
    }
    TEST_ASSERT_EQUAL_UINT32(2, st.crashes);
    boot_guard_set_reboot_reason(&st, 12u | BOOT_GUARD_REASON_CRASH);
    TEST_ASSERT_FALSE(boot_guard_on_stable(&st));
    TEST_ASSERT_EQUAL_UINT32(2, st.crashes);
    TEST_ASSERT_TRUE(boot_guard_valid(&st));
    boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL_UINT32(3, st.crashes);

    /* A planned reboot under way does not hold it back. */
    boot_guard_set_reboot_reason(&st, 1u);
    TEST_ASSERT_TRUE(boot_guard_on_stable(&st));
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);
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

TEST_CASE("boot guard reports the previous session uptime after software and crash resets only", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    TEST_ASSERT_EQUAL_UINT32(0, st.session_s);

    boot_guard_heartbeat(&st, 120);
    boot_guard_set_reboot_reason(&st, 4);
    TEST_ASSERT_EQUAL_UINT32(120, boot(&st, BOOT_HW_SOFTWARE).prev_session_s);
    TEST_ASSERT_EQUAL_UINT32(0, st.session_s);           /* the new session starts at 0 */

    boot_guard_heartbeat(&st, 300);
    TEST_ASSERT_EQUAL_UINT32(300, boot(&st, BOOT_HW_CRASH).prev_session_s);

    /* RTC content is not trusted after a brownout; other resets are unknown too. */
    boot_guard_heartbeat(&st, 77);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_UNKNOWN, boot(&st, BOOT_HW_BROWNOUT).prev_session_s);
    boot_guard_heartbeat(&st, 50);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_UNKNOWN, boot(&st, BOOT_HW_OTHER).prev_session_s);
    boot_guard_heartbeat(&st, 60);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_UNKNOWN, boot(&st, BOOT_HW_POWER_ON).prev_session_s);

    /* A lost block (another image, corruption) is a fresh start. */
    boot_guard_heartbeat(&st, 90);
    st.session_s = 91;                                   /* changed without resealing */
    boot_guard_result_t res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_TRUE(res.state_was_lost);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_UNKNOWN, res.prev_session_s);
}

TEST_CASE("boot guard accumulates station time across resets since power-on", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    TEST_ASSERT_EQUAL_UINT32(10, boot_guard_station_time(&st, 10));

    boot_guard_heartbeat(&st, 1000);
    boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL_UINT32(1005, boot_guard_station_time(&st, 5));

    /* A brownout adds nothing (conservative for the reboot budget). */
    boot_guard_heartbeat(&st, 200);
    boot(&st, BOOT_HW_BROWNOUT);
    TEST_ASSERT_EQUAL_UINT32(1000, boot_guard_station_time(&st, 0));

    /* A crash between heartbeats loses at most the time since the last one. */
    boot_guard_heartbeat(&st, 60);
    boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_EQUAL_UINT32(1060, boot_guard_station_time(&st, 0));

    boot(&st, BOOT_HW_OTHER);                            /* nothing beat since: adds 0 */
    TEST_ASSERT_EQUAL_UINT32(1060, boot_guard_station_time(&st, 0));

    boot(&st, BOOT_HW_POWER_ON);
    TEST_ASSERT_EQUAL_UINT32(7, boot_guard_station_time(&st, 7));
}

/* A GNSS-ladder reboot at `uptime` of the current boot, then the boot it causes. */
static void ladder_reboot(boot_guard_state_t *st, uint32_t uptime)
{
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(st, uptime, NULL, NULL));
    boot_guard_record_ladder_reboot(st, uptime);
    boot_guard_set_reboot_reason(st, 6);
    boot(st, BOOT_HW_SOFTWARE);
}

TEST_CASE("GNSS ladder reboots: an hour apart and at most three per day, across reboots", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    uint32_t wait = 99;
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 100, &wait, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, wait);

    ladder_reboot(&st, 100);                             /* station time 100 */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 10, &wait, NULL));
    TEST_ASSERT_EQUAL_UINT32(3590, wait);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 3599, &wait, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, wait);
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 3600, &wait, NULL));

    ladder_reboot(&st, 3600);                            /* station time 3700 */
    ladder_reboot(&st, 3600);                            /* station time 7300 */

    /* Three in the window: the oldest must be 24 h old. */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 3600, &wait, NULL));   /* now 10900 */
    TEST_ASSERT_EQUAL_UINT32(86400u - 10800u, wait);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 86400u - 7300u + 99u, NULL, NULL));
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 86400u - 7300u + 100u, &wait, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, wait);

    /* The next one pushes the oldest out; the gap rule still applies. */
    ladder_reboot(&st, 86400u - 7300u + 100u);           /* station time 86500 */
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_LADDER_KEEP, st.ladder_count);
    TEST_ASSERT_EQUAL_UINT32(3700, st.ladder_at[0]);
    TEST_ASSERT_EQUAL_UINT32(86500, st.ladder_at[2]);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 1800, &wait, NULL));
    TEST_ASSERT_EQUAL_UINT32(1800, wait);

    /* A crash in between only moves station time on (now 87500 at its boot). */
    boot_guard_heartbeat(&st, 1000);
    boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 2599, &wait, NULL));   /* 3599 s after the last one */
    TEST_ASSERT_EQUAL_UINT32(1, wait);
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 2600, NULL, NULL));
}

TEST_CASE("GNSS ladder reboots: the cap names the limit that sets the wait", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    uint32_t wait = 99;
    boot_guard_cap_t cap = BOOT_GUARD_CAP_DAY;
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 100, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_NONE, cap);

    /* One reboot 10 min ago: the hourly limit, 50 min. */
    ladder_reboot(&st, 100);                             /* station time 100 */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 600, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_HOUR, cap);
    TEST_ASSERT_EQUAL_UINT32(50u * 60u, wait);

    ladder_reboot(&st, 3600);                            /* station time 3700 */
    ladder_reboot(&st, 3600);                            /* station time 7300 */
    /* Both limits hold (the newest 10 min ago, three within 24 h): the
     * daily one ends later. */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 600, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_DAY, cap);
    TEST_ASSERT_EQUAL_UINT32(86400u - 7800u, wait);
    /* Three in 24 h, the oldest 3 h ago: the daily limit, 21 h. */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 3600, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_DAY, cap);
    TEST_ASSERT_EQUAL_UINT32(21u * 3600u, wait);

    /* Both hold and the hourly one ends later: the oldest of three is almost
     * a day old, the newest 10 min. */
    fresh(&st);
    ladder_reboot(&st, 100);                             /* station time 100 */
    ladder_reboot(&st, 80000);                           /* 80100 */
    ladder_reboot(&st, 5000);                            /* 85100 */
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 600, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_HOUR, cap);
    TEST_ASSERT_EQUAL_UINT32(3000, wait);
    /* Once the hour is up, the day (the oldest left the window) holds nothing. */
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 3600, &wait, &cap));
    TEST_ASSERT_EQUAL(BOOT_GUARD_CAP_NONE, cap);
    TEST_ASSERT_EQUAL_UINT32(0, wait);
}

TEST_CASE("GNSS ladder reboot budget starts fresh after a power cycle or a lost RTC block", "[boot_guard]")
{
    boot_guard_state_t st;
    fresh(&st);
    ladder_reboot(&st, 100);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 10, NULL, NULL));
    boot(&st, BOOT_HW_POWER_ON);
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 10, NULL, NULL));

    ladder_reboot(&st, 100);
    TEST_ASSERT_FALSE(boot_guard_ladder_reboot_allowed(&st, 10, NULL, NULL));
    st.ladder_at[0] = 5;                                 /* corrupted: invalid block */
    TEST_ASSERT_TRUE(boot(&st, BOOT_HW_SOFTWARE).state_was_lost);
    TEST_ASSERT_TRUE(boot_guard_ladder_reboot_allowed(&st, 10, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, st.ladder_count);
}

TEST_CASE("boot guard: a block from before another image's run is accounted again from scratch", "[boot_guard]")
{
    /* Image A crashed twice, then planned an app update (stamp: generation 4). */
    boot_guard_state_t st;
    fresh(&st);
    boot_guard_stamp(&st, 4, 0xA0A0u);
    boot(&st, BOOT_HW_CRASH);
    boot(&st, BOOT_HW_CRASH);
    TEST_ASSERT_EQUAL_UINT32(2, st.crashes);
    TEST_ASSERT_EQUAL_UINT32(4, st.gen);                 /* accounting keeps the stamp */
    TEST_ASSERT_EQUAL_UINT32(0xA0A0u, st.image);         /* ...and the image that wrote it */
    boot_guard_set_reboot_reason(&st, 4);                /* APP_UPDATE */
    TEST_ASSERT_TRUE(boot_guard_valid(&st));

    /* Another image ran; back on A, the first accounting takes the block as
     * found (the settings are not read yet), and so does one claimed flag. */
    boot_guard_result_t res = boot(&st, BOOT_HW_SOFTWARE);
    TEST_ASSERT_EQUAL(BOOT_RESET_PLANNED, res.cls);
    TEST_ASSERT_EQUAL_UINT32(4, res.prev_reboot_reason);
    TEST_ASSERT_TRUE(boot_guard_claim_once(&st, 1u << 3));

    /* The stamp is not accepted: accounted again as after power loss. */
    boot_guard_restart(&st, BOOT_HW_SOFTWARE, LIMIT, 1u << 3, &res);
    TEST_ASSERT_TRUE(boot_guard_valid(&st));
    TEST_ASSERT_TRUE(res.state_was_lost);
    TEST_ASSERT_EQUAL(BOOT_RESET_OTHER, res.cls);        /* no stale planned reason */
    TEST_ASSERT_EQUAL_UINT32(0, res.prev_reboot_reason);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GUARD_UNKNOWN, res.prev_session_s);
    TEST_ASSERT_EQUAL_UINT32(0, st.crashes);
    TEST_ASSERT_EQUAL_UINT32(1, st.boots);
    TEST_ASSERT_EQUAL_UINT32(0, st.gen);                 /* unstamped until stamped */
    TEST_ASSERT_EQUAL_UINT32(0, st.image);
    TEST_ASSERT_FALSE(boot_guard_claim_once(&st, 1u << 3));   /* this boot's claim kept */
    TEST_ASSERT_TRUE(boot_guard_claim_once(&st, 1u << 2));

    /* A crash reset counts once, from zero; stale safe mode is dropped. */
    for (int i = 0; i < (int)LIMIT; i++) {
        boot(&st, BOOT_HW_CRASH);
    }
    TEST_ASSERT_EQUAL_UINT32(1, st.safe_mode);
    boot_guard_restart(&st, BOOT_HW_CRASH, LIMIT, 0, &res);
    TEST_ASSERT_EQUAL(BOOT_RESET_CRASH, res.cls);
    TEST_ASSERT_EQUAL_UINT32(1, st.crashes);
    TEST_ASSERT_FALSE(res.safe_mode);
    TEST_ASSERT_EQUAL_UINT32(0, st.safe_mode);

    /* The stamp and the writer are covered by the checksum. */
    boot_guard_stamp(&st, 9, 0xB0B0u);
    TEST_ASSERT_TRUE(boot_guard_valid(&st));
    st.gen = 8;
    TEST_ASSERT_FALSE(boot_guard_valid(&st));
    boot_guard_stamp(&st, 9, 0xB0B0u);
    st.image = 0xA0A0u;
    TEST_ASSERT_FALSE(boot_guard_valid(&st));
}
