/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "boot_guard.h"

#include <stddef.h>
#include <string.h>

static uint32_t checksum(const boot_guard_state_t *st)
{
    const uint8_t *p = (const uint8_t *)st;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(boot_guard_state_t, check); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

bool boot_guard_valid(const boot_guard_state_t *st)
{
    return st->magic == BOOT_GUARD_MAGIC && st->check == checksum(st);
}

void boot_guard_seal(boot_guard_state_t *st)
{
    st->check = checksum(st);
}

static void start_over(boot_guard_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->magic = BOOT_GUARD_MAGIC;
}

void boot_guard_on_boot(boot_guard_state_t *st, boot_hw_reset_t hw, uint32_t limit, boot_guard_result_t *out)
{
    boot_guard_result_t res = { 0 };

    res.state_was_lost = !boot_guard_valid(st);
    if (res.state_was_lost || hw == BOOT_HW_POWER_ON) {
        start_over(st);
    }

    uint32_t pending = st->reboot_reason;
    st->reboot_reason = 0;
    res.prev_reboot_reason = pending;

    switch (hw) {
    case BOOT_HW_POWER_ON:
        res.cls = BOOT_RESET_POWER_ON;
        break;
    case BOOT_HW_SOFTWARE:
        if (pending == 0) {
            res.cls = BOOT_RESET_OTHER;           /* restart we did not plan */
        } else if (pending & BOOT_GUARD_REASON_CRASH) {
            res.cls = BOOT_RESET_CRASH;           /* fatal boot error */
        } else {
            res.cls = BOOT_RESET_PLANNED;
        }
        break;
    case BOOT_HW_CRASH:
        res.cls = BOOT_RESET_CRASH;
        break;
    case BOOT_HW_BROWNOUT:
        res.cls = BOOT_RESET_BROWNOUT;
        break;
    case BOOT_HW_OTHER:
    default:
        res.cls = BOOT_RESET_OTHER;
        break;
    }

    st->boots++;
    if (res.cls == BOOT_RESET_CRASH) {
        st->crashes++;
    } else if (res.cls == BOOT_RESET_BROWNOUT) {
        st->brownouts++;
    }

    bool was_safe = st->safe_mode != 0;
    res.safe_mode = limit > 0 && st->crashes >= limit;
    res.entered_safe_mode = res.safe_mode && !was_safe;
    if (res.entered_safe_mode) {
        st->safe_episodes++;
    }
    st->safe_mode = res.safe_mode ? 1u : 0u;

    boot_guard_seal(st);
    if (out != NULL) {
        *out = res;
    }
}

void boot_guard_on_stable(boot_guard_state_t *st)
{
    if (!st->safe_mode) {
        st->crashes = 0;
        st->safe_episodes = 0;
    }
    boot_guard_seal(st);
}

void boot_guard_set_reboot_reason(boot_guard_state_t *st, uint32_t reason)
{
    st->reboot_reason = reason;
    boot_guard_seal(st);
}

uint32_t boot_guard_safe_retry_s(const boot_guard_state_t *st)
{
    if (st->safe_episodes <= 1) {
        return 3600u;
    }
    if (st->safe_episodes == 2) {
        return 4u * 3600u;
    }
    return 24u * 3600u;
}

void boot_guard_prepare_safe_retry(boot_guard_state_t *st, uint32_t limit)
{
    st->crashes = (limit > 0) ? limit - 1 : 0;
    boot_guard_seal(st);
}

void boot_guard_clear(boot_guard_state_t *st)
{
    st->crashes = 0;
    st->safe_episodes = 0;
    boot_guard_seal(st);
}

bool boot_guard_claim_once(boot_guard_state_t *st, uint32_t flag)
{
    if (st->once_flags & flag) {
        return false;
    }
    st->once_flags |= flag;
    boot_guard_seal(st);
    return true;
}
