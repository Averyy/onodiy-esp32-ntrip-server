/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "lifecycle.h"
#include "unity.h"
#include "web_enums.h"

TEST_CASE("reboots the user asked for are planned downtime, the station's own are not", "[lifecycle]")
{
    static const lifecycle_reboot_reason_t planned[] = {
        LIFECYCLE_REBOOT_USER,         LIFECYCLE_REBOOT_FACTORY_RESET, LIFECYCLE_REBOOT_ADMIN_RESET,
        LIFECYCLE_REBOOT_APP_UPDATE,   LIFECYCLE_REBOOT_APP_ROLLBACK,  LIFECYCLE_REBOOT_SAFE_MODE_CLEARED,
    };
    static const lifecycle_reboot_reason_t not_planned[] = {
        LIFECYCLE_REBOOT_NONE,          LIFECYCLE_REBOOT_APP_REVERT,      LIFECYCLE_REBOOT_GNSS_LADDER,
        LIFECYCLE_REBOOT_NETWORK_LADDER, LIFECYCLE_REBOOT_LOW_HEAP,       LIFECYCLE_REBOOT_SAFE_MODE_RETRY,
        LIFECYCLE_REBOOT_NVS_SLOT_SWITCH, LIFECYCLE_REBOOT_BOOT_FAILURE,
    };
    for (size_t i = 0; i < sizeof(planned) / sizeof(planned[0]); i++) {
        TEST_ASSERT_TRUE(lifecycle_reboot_user_planned(planned[i]));
    }
    for (size_t i = 0; i < sizeof(not_planned) / sizeof(not_planned[0]); i++) {
        TEST_ASSERT_FALSE(lifecycle_reboot_user_planned(not_planned[i]));
    }
    /* Every reason is decided above (a new one must be added to a list). */
    TEST_ASSERT_EQUAL(LIFECYCLE_REBOOT_APP_ROLLBACK + 1,
                      (int)(sizeof(planned) / sizeof(planned[0]) + sizeof(not_planned) / sizeof(not_planned[0])));
    TEST_ASSERT_FALSE(lifecycle_reboot_user_planned((lifecycle_reboot_reason_t)99));
}

TEST_CASE("the user's rollback is its own reboot reason, apart from a failed trial", "[lifecycle]")
{
    TEST_ASSERT_EQUAL_STRING("app_revert", web_reboot_reason_name(LIFECYCLE_REBOOT_APP_REVERT));
    TEST_ASSERT_EQUAL_STRING("app_rollback", web_reboot_reason_name(LIFECYCLE_REBOOT_APP_ROLLBACK));
    TEST_ASSERT_EQUAL(13, LIFECYCLE_REBOOT_APP_ROLLBACK);   /* appended: stored numbers keep their meaning */
    TEST_ASSERT_EQUAL(12, LIFECYCLE_REBOOT_BOOT_FAILURE);
}

TEST_CASE("the previous session's end: a crash never planned, our reboots by reason, else unknown", "[lifecycle]")
{
    /* A crash, also one while the user's restart was shutting down (its
     * reason was recorded), and our own fatal boot error. */
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNPLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_CRASH, LIFECYCLE_REBOOT_NONE));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNPLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_CRASH, LIFECYCLE_REBOOT_USER));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNPLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_CRASH, LIFECYCLE_REBOOT_BOOT_FAILURE));
    /* Our reboots: the reason decides (a failed trial after an update is not planned). */
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_PLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_PLANNED, LIFECYCLE_REBOOT_APP_UPDATE));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_PLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_PLANNED, LIFECYCLE_REBOOT_USER));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNPLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_PLANNED, LIFECYCLE_REBOOT_APP_REVERT));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNPLANNED,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_PLANNED, LIFECYCLE_REBOOT_NETWORK_LADDER));
    /* No record of ours: another image, a reset pin, a power cut. */
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNKNOWN, lifecycle_prev_session_end(LIFECYCLE_RESET_OTHER, LIFECYCLE_REBOOT_NONE));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNKNOWN, lifecycle_prev_session_end(LIFECYCLE_RESET_OTHER, LIFECYCLE_REBOOT_USER));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNKNOWN,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_POWER_ON, LIFECYCLE_REBOOT_NONE));
    TEST_ASSERT_EQUAL(LIFECYCLE_PREV_UNKNOWN,
                      lifecycle_prev_session_end(LIFECYCLE_RESET_BROWNOUT, LIFECYCLE_REBOOT_USER));
}

TEST_CASE("reboots that start another app image", "[lifecycle]")
{
    TEST_ASSERT_TRUE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_APP_UPDATE));
    TEST_ASSERT_TRUE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_APP_ROLLBACK));
    TEST_ASSERT_TRUE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_APP_REVERT));
    /* Committed before the settings are read, by an image that never records a boot. */
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_NVS_SLOT_SWITCH));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_NONE));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_USER));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_FACTORY_RESET));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_GNSS_LADDER));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_SAFE_MODE_CLEARED));
    TEST_ASSERT_FALSE(lifecycle_reboot_switches_image(LIFECYCLE_REBOOT_BOOT_FAILURE));
}
