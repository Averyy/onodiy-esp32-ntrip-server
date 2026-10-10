/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Which reboot reasons are planned downtime (pure logic, no RTOS calls,
 * host-testable). The reasons and why each is or is not planned are listed
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
