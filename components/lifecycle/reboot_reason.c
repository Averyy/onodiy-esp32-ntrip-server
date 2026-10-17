/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Which reboot reasons are planned downtime or start another app image,
 * how the previous session ended, and which resets keep the RTC timer
 * running (pure logic, no RTOS calls, host-testable). The reasons and why each is or is not planned are listed
 * at lifecycle_reboot_user_planned() in lifecycle.h.
 */
#include "lifecycle.h"

bool lifecycle_reboot_user_planned(lifecycle_reboot_reason_t reason)
{
    switch (reason) {
    case LIFECYCLE_REBOOT_USER:
    case LIFECYCLE_REBOOT_FACTORY_RESET:
    case LIFECYCLE_REBOOT_ADMIN_RESET:
    case LIFECYCLE_REBOOT_APP_UPDATE:
    case LIFECYCLE_REBOOT_APP_ROLLBACK:
    case LIFECYCLE_REBOOT_SAFE_MODE_CLEARED:
        return true;
    case LIFECYCLE_REBOOT_NONE:
    case LIFECYCLE_REBOOT_APP_REVERT:
    case LIFECYCLE_REBOOT_GNSS_LADDER:
    case LIFECYCLE_REBOOT_NETWORK_LADDER:
    case LIFECYCLE_REBOOT_LOW_HEAP:
    case LIFECYCLE_REBOOT_SAFE_MODE_RETRY:
    case LIFECYCLE_REBOOT_NVS_SLOT_SWITCH:
    case LIFECYCLE_REBOOT_BOOT_FAILURE:
    default:
        return false;
    }
}

bool lifecycle_reboot_switches_image(lifecycle_reboot_reason_t reason)
{
    switch (reason) {
    case LIFECYCLE_REBOOT_APP_UPDATE:
    case LIFECYCLE_REBOOT_APP_ROLLBACK:
    case LIFECYCLE_REBOOT_APP_REVERT:
        return true;
    case LIFECYCLE_REBOOT_NVS_SLOT_SWITCH:   /* committed before the settings are read: see lifecycle.h */
    default:
        return false;
    }
}

lifecycle_prev_end_t lifecycle_prev_session_end(lifecycle_reset_class_t cls, lifecycle_reboot_reason_t prev)
{
    switch (cls) {
    case LIFECYCLE_RESET_CRASH:
        return LIFECYCLE_PREV_UNPLANNED;
    case LIFECYCLE_RESET_PLANNED:
        return lifecycle_reboot_user_planned(prev) ? LIFECYCLE_PREV_PLANNED : LIFECYCLE_PREV_UNPLANNED;
    case LIFECYCLE_RESET_POWER_ON:
    case LIFECYCLE_RESET_BROWNOUT:
    case LIFECYCLE_RESET_OTHER:
    default:
        return LIFECYCLE_PREV_UNKNOWN;
    }
}

bool lifecycle_reset_keeps_rtc_timer(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}
