/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Ownership of the one driver scan, shared by roaming and the setup page.
 * All calls are serialized by network.c's lock. Only the net task starts,
 * stops or reads a driver scan. The event callback retains completion here
 * even if the net task's message queue is full. A queued user request is
 * not an active scan and has no driver watchdog.
 * A stop request closes admission at once; only the driver's subsequent
 * stop/start events open a new scan lifetime. A successful start() return
 * alone cannot let a delayed old stop event retire a new scan.
 *
 * After a watchdog, keep the old owner until its completion is consumed.
 * If cancellation never completes, reject further scans (but not network
 * reconnects) until a late completion or a station-stop event settles it.
 * Never assign a late completion to a successor using a guessed scan ID.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NET_SCAN_WATCHDOG_MS 20000u
#define NET_SCAN_CANCEL_MS 3000u

typedef enum { NET_SCAN_NONE = 0, NET_SCAN_USER, NET_SCAN_ROAM } net_scan_owner_t;
typedef enum { NET_SCAN_IDLE = 0, NET_SCAN_RUNNING, NET_SCAN_CANCELLING, NET_SCAN_UNCERTAIN } net_scan_phase_t;
typedef enum { NET_SCAN_WAIT = 0, NET_SCAN_STOP, NET_SCAN_GIVE_UP } net_scan_action_t;

typedef struct {
    net_scan_owner_t owner;
    net_scan_phase_t phase;
    bool radio_ready;
    bool stop_pending;
    bool user_queued;
    bool done;
    bool duplicate;
    uint32_t status;
    uint32_t done_ms;
    uint32_t started_ms;
    uint32_t cancelled_ms;
} net_scan_t;

typedef struct {
    bool ready;
    net_scan_owner_t owner;
    bool success;
    bool cancelled;
    bool ambiguous;
    bool user_cancelled;        /**< ambiguity also ended a queued user request */
    uint32_t status;
    uint32_t duration_ms;
} net_scan_done_t;

void net_scan_init(net_scan_t *s);
/** Driver events / stop request, not merely a successful start call. */
void net_scan_started(net_scan_t *s);
void net_scan_stopping(net_scan_t *s, bool station_started);
bool net_scan_request_user(net_scan_t *s);
bool net_scan_begin(net_scan_t *s, net_scan_owner_t owner, uint32_t now_ms);
void net_scan_start_failed(net_scan_t *s);
void net_scan_notify(net_scan_t *s, uint32_t status, uint32_t now_ms);
net_scan_done_t net_scan_take_done(net_scan_t *s);
net_scan_action_t net_scan_tick(net_scan_t *s, uint32_t now_ms);
bool net_scan_radio_busy(const net_scan_t *s);
bool net_scan_user_busy(const net_scan_t *s);

/* Retire the driver's scan lifetime once its station-stop event arrives.
 * Returns whether a queued/active user request must be completed as failed. */
bool net_scan_stopped(net_scan_t *s);
