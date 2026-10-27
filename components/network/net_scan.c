/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "net_scan.h"

#include <string.h>

void net_scan_init(net_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

void net_scan_started(net_scan_t *s)
{
    /* A delayed START from before a requested stop cannot reopen admission. */
    if (!s->stop_pending) {
        s->radio_ready = true;
    }
}

void net_scan_stopping(net_scan_t *s, bool station_started)
{
    /* Recovery may stop after the initial driver start failed. No station
     * lifetime then needs a STOP event, but never release an earlier one
     * which is still pending. */
    s->stop_pending |= station_started || s->radio_ready || s->owner != NET_SCAN_NONE;
    s->radio_ready = false;
}

bool net_scan_request_user(net_scan_t *s)
{
    if (!s->radio_ready || s->user_queued || s->owner == NET_SCAN_USER || s->phase == NET_SCAN_CANCELLING ||
        s->phase == NET_SCAN_UNCERTAIN) {
        return false;
    }
    s->user_queued = true;
    return true;
}

bool net_scan_begin(net_scan_t *s, net_scan_owner_t owner, uint32_t now_ms)
{
    if (!s->radio_ready || owner == NET_SCAN_NONE || s->phase != NET_SCAN_IDLE || s->done ||
        (owner == NET_SCAN_ROAM && s->user_queued) || (owner == NET_SCAN_USER && !s->user_queued)) {
        return false;
    }
    if (owner == NET_SCAN_USER) {
        s->user_queued = false;
    }
    s->owner = owner;
    s->phase = NET_SCAN_RUNNING;
    s->started_ms = now_ms;
    return true;
}

void net_scan_start_failed(net_scan_t *s)
{
    /* A driver call which refused to start has no result to await. If a
     * completion nevertheless arrived, consume it before any new start. */
    s->owner = NET_SCAN_NONE;
    s->phase = NET_SCAN_IDLE;
}

void net_scan_notify(net_scan_t *s, uint32_t status, uint32_t now_ms)
{
    if (s->done) {
        s->duplicate = true;   /* a single owner cannot account for two completions */
    } else {
        s->done = true;
        s->status = status;
        s->done_ms = now_ms;
    }
}

net_scan_done_t net_scan_take_done(net_scan_t *s)
{
    net_scan_done_t out = { 0 };
    if (!s->done) {
        return out;
    }
    out.ready = true;
    out.owner = s->owner;
    out.status = s->status;
    out.ambiguous = s->duplicate;
    out.user_cancelled = s->duplicate && s->user_queued;
    out.cancelled = s->phase == NET_SCAN_CANCELLING || s->phase == NET_SCAN_UNCERTAIN;
    out.duration_ms = s->owner != NET_SCAN_NONE ? s->done_ms - s->started_ms : 0;
    out.success = s->phase == NET_SCAN_RUNNING && s->status == 0 && !s->duplicate;
    s->done = false;
    s->duplicate = false;
    bool unresolved = out.ambiguous || (s->phase == NET_SCAN_UNCERTAIN && s->owner == NET_SCAN_NONE);
    s->owner = NET_SCAN_NONE;
    s->phase = unresolved ? NET_SCAN_UNCERTAIN : NET_SCAN_IDLE;
    if (out.user_cancelled) {
        s->user_queued = false;
    }
    return out;
}

net_scan_action_t net_scan_tick(net_scan_t *s, uint32_t now_ms)
{
    if (s->done) {
        return NET_SCAN_WAIT;   /* completed scans are read before considering a watchdog */
    }
    if (s->phase == NET_SCAN_RUNNING && (uint32_t)(now_ms - s->started_ms) >= NET_SCAN_WATCHDOG_MS) {
        s->phase = NET_SCAN_CANCELLING;
        s->cancelled_ms = now_ms;
        return NET_SCAN_STOP;
    }
    if (s->phase == NET_SCAN_CANCELLING && (uint32_t)(now_ms - s->cancelled_ms) >= NET_SCAN_CANCEL_MS) {
        s->phase = NET_SCAN_UNCERTAIN;
        s->user_queued = false;
        return NET_SCAN_GIVE_UP;
    }
    return NET_SCAN_WAIT;
}

bool net_scan_radio_busy(const net_scan_t *s)
{
    return s->phase == NET_SCAN_RUNNING || s->phase == NET_SCAN_CANCELLING;
}

bool net_scan_user_busy(const net_scan_t *s)
{
    return s->user_queued || (s->owner == NET_SCAN_USER && s->phase == NET_SCAN_RUNNING);
}

bool net_scan_stopped(net_scan_t *s)
{
    bool user = net_scan_user_busy(s);
    net_scan_init(s);
    return user;
}
