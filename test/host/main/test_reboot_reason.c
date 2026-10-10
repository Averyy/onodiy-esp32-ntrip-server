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
