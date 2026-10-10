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

    /* Session uptime and station time. A fresh state has nothing to add. */
    res.prev_session_s = BOOT_GUARD_UNKNOWN;
    if (!res.state_was_lost && hw != BOOT_HW_POWER_ON) {
        if (hw == BOOT_HW_SOFTWARE || hw == BOOT_HW_CRASH) {
            res.prev_session_s = st->session_s;
        }
        if (hw != BOOT_HW_BROWNOUT) {
            /* After a brownout the heartbeat is not trusted: adding nothing
             * makes earlier ladder reboots look more recent, never older. */
            uint32_t base = st->uptime_base_s + st->session_s;
            st->uptime_base_s = (base < st->uptime_base_s) ? UINT32_MAX : base;
        }
    }
    st->session_s = 0;

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

void boot_guard_restart(boot_guard_state_t *st, boot_hw_reset_t hw, uint32_t limit, uint32_t claimed,
                        boot_guard_result_t *out)
{
    memset(st, 0, sizeof(*st));   /* no magic: starts over */
    boot_guard_on_boot(st, hw, limit, out);
    st->once_flags |= claimed;
    boot_guard_seal(st);
}

void boot_guard_stamp(boot_guard_state_t *st, uint32_t gen)
{
    st->gen = gen;
    boot_guard_seal(st);
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

void boot_guard_heartbeat(boot_guard_state_t *st, uint32_t uptime_s)
{
    st->session_s = uptime_s;
    boot_guard_seal(st);
}

uint32_t boot_guard_station_time(const boot_guard_state_t *st, uint32_t uptime_s)
{
    uint32_t t = st->uptime_base_s + uptime_s;
    return (t < st->uptime_base_s) ? UINT32_MAX : t;
}

/* Seconds since a recorded station time; a time in the future (should not
 * happen) counts as "just now", which only ever blocks longer. */
static uint32_t since(uint32_t now, uint32_t then)
{
    return (now >= then) ? now - then : 0;
}

bool boot_guard_ladder_reboot_allowed(const boot_guard_state_t *st, uint32_t uptime_s, uint32_t *retry_in_s)
{
    uint32_t now = boot_guard_station_time(st, uptime_s);
    uint32_t wait = 0;
    uint32_t n = st->ladder_count;
    if (n > BOOT_GUARD_LADDER_KEEP) {
        n = BOOT_GUARD_LADDER_KEEP;
    }
    if (n > 0) {
        uint32_t newest = since(now, st->ladder_at[n - 1]);
        if (newest < BOOT_GUARD_LADDER_GAP_S) {
            wait = BOOT_GUARD_LADDER_GAP_S - newest;
        }
    }
    if (n == BOOT_GUARD_LADDER_KEEP) {
        uint32_t oldest = since(now, st->ladder_at[0]);
        if (oldest < BOOT_GUARD_LADDER_WINDOW_S && BOOT_GUARD_LADDER_WINDOW_S - oldest > wait) {
            wait = BOOT_GUARD_LADDER_WINDOW_S - oldest;
        }
    }
    if (retry_in_s != NULL) {
        *retry_in_s = wait;
    }
    return wait == 0;
}

void boot_guard_record_ladder_reboot(boot_guard_state_t *st, uint32_t uptime_s)
{
    uint32_t now = boot_guard_station_time(st, uptime_s);
    if (st->ladder_count >= BOOT_GUARD_LADDER_KEEP) {
        for (uint32_t i = 1; i < BOOT_GUARD_LADDER_KEEP; i++) {
            st->ladder_at[i - 1] = st->ladder_at[i];
        }
        st->ladder_count = BOOT_GUARD_LADDER_KEEP - 1;
    }
    st->ladder_at[st->ladder_count++] = now;
    st->session_s = uptime_s;
    boot_guard_seal(st);
}
