/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Availability ledger: how much of each UTC day each caster slot streamed.
 *
 * Onocoy's daily streak needs Availability >= 0.98, which on its scoring
 * curve means a graced uptime of >= 99.8 % per UTC day with a 300 s grant:
 * at most ~473 s offline per day at a perfect data rate (less when epochs
 * are lost; a plain 98 % uptime scores only about 0.81). The ledger counts,
 * per slot, the seconds a slot was expected to stream (enabled) but did not;
 * time paused for planned receiver work counts too and is also shown on its
 * own as planned. Slots flagged "protected" (the Onocoy slot)
 * drive the daily downtime budget shown in the UI and with update prompts,
 * and the network recovery ladder. The budget never gates anything: every
 * update is started by the user, who sees the expected downtime against
 * what is left.
 *
 * Inputs (all push-style, so this L1 component depends on nobody above it):
 *   - ntrip_server reports each slot's state on every change;
 *   - the station reports whether fresh RTCM is flowing (a slot whose
 *     session stays open through a receiver stall streams nothing, so it
 *     counts as down);
 *   - the clock: GNSS time first (receiver time output or MSM epoch + week),
 *     SNTP as fallback (taken only while no GNSS time arrived for 10 min).
 *     Until either is valid, seconds accrue to an "unattributed" record and
 *     are spread over the days they covered once time is known. Trusted
 *     time is passed on to diag for the event journal's timestamps.
 *
 * Persistence: the running counters also live in RTC memory, written every
 * tick and checked with magic, size and CRC (they survive software, panic
 * and watchdog resets, so a reboot costs at most a second of accounting).
 * Today's counters and the "last alive" mark are stored in settings every
 * CONFIG_LEDGER_ALIVE_PERSIST_S (today only when it changed), so after a
 * power cut the time between that mark and the next boot is added as
 * downtime (and power-off time) for the slots that were expected to stream.
 * Day totals roll into a CONFIG_LEDGER_HISTORY_DAYS history at UTC midnight
 * (settings: ledger/hist, written when it changes). The whole ledger
 * namespace survives a factory reset.
 *
 * Accounting is a pure core (ledger_core.c, host-tested across clock wraps,
 * day rollovers, power-cut gaps and late clocks); ledger.c adds the lock,
 * the RTC copy and the settings writes. ledger_tick() is driven at 1 Hz by
 * the health monitor and is the only call that writes settings; every other
 * call is short and may come from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEDGER_MAX_SLOTS 8

/** Where the ledger's notion of UTC came from. */
typedef enum {
    LEDGER_CLOCK_NONE = 0,
    LEDGER_CLOCK_SNTP,
    LEDGER_CLOCK_GNSS,
} ledger_clock_t;

/** One UTC day. */
typedef struct {
    int32_t utc_day;                          /**< days since 1970-01-01, -1 = no day known yet */
    uint32_t down_s[LEDGER_MAX_SLOTS];        /**< seconds a slot should have streamed but did not */
    uint32_t planned_s[LEDGER_MAX_SLOTS];     /**< of which planned (casters paused for receiver work) */
    uint32_t reconnects[LEDGER_MAX_SLOTS];
    uint32_t power_off_s;                     /**< power-cut gaps added at boot */
    uint32_t rx_overflows;                    /**< UART/driver overflow events */
} ledger_day_t;

/**
 * Load today and the history (RTC copy first, then settings). The gap since
 * the last-alive mark is added once the first trusted time arrives.
 */
esp_err_t ledger_init(void);

/**
 * Slot state, reported by ntrip_server on every change. `enabled`: the slot
 * is configured to stream, whether or not the caster client runs (time
 * before it starts, in safe mode or while it shuts down is downtime too).
 * Until a slot is reported in a boot, its state from the previous session
 * is assumed (expected to stream if it was then, not streaming).
 */
void ledger_set_slot(size_t slot, bool enabled, bool protected_slot, bool streaming);

/** Planned downtime in progress (casters paused for receiver flash/config); counted as down and also as planned. */
void ledger_set_planned(bool planned);

/** Fresh RTCM flowing from the receiver (reported by the station). */
void ledger_set_rtcm_ok(bool ok);

/**
 * Feed a UTC time and its source; GNSS beats SNTP. Times before 2026 are
 * ignored. The first one of a boot settles the time since the previous
 * session's last-alive mark and the unattributed seconds.
 */
void ledger_set_utc(int64_t unix_s, ledger_clock_t source);

/** Count a data-loss event against today (UART overflow etc.). */
void ledger_count_overflow(void);

/** 1 Hz tick from the health monitor: accrue, roll the day, refresh the RTC copy, store (may write settings). */
void ledger_tick(void);

/** Today's counters (without a trusted clock: the last known day plus the unattributed seconds). */
esp_err_t ledger_get_today(ledger_day_t *out);

/** Past days, newest first; *count in = capacity, out = filled. */
esp_err_t ledger_get_history(ledger_day_t *days, size_t *count);

/** Protected-slot downtime so far this UTC day, seconds (worst protected slot; unattributed seconds included while there is no clock). */
uint32_t ledger_protected_down_today_s(void);

/**
 * Protected-slot downtime still allowed today: 86400 - (86400 x
 * CONFIG_LEDGER_TARGET_PERMILLE / 1000 - CONFIG_LEDGER_GRACE_S) minus the
 * protected downtime so far, clamped at 0 (998 per mille with the 300 s
 * grant: 473 s a day). The Onocoy slot's estimate adjusted for its data rate
 * comes from the caster status.
 */
uint32_t ledger_protected_budget_left_s(void);

/** Seconds since 00:00 UTC, or -1 if no trusted clock yet. */
int32_t ledger_utc_seconds_of_day(void);

/** Current clock source. */
ledger_clock_t ledger_clock_source(void);

#ifdef __cplusplus
}
#endif
