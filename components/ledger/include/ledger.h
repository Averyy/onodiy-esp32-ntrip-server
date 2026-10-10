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
 * per slot, the seconds a slot was expected to stream (enabled, not paused
 * for planned work) but did not. Slots flagged "protected" (the Onocoy slot)
 * drive the daily downtime budget shown in the UI and with update prompts,
 * and the network recovery ladder. The budget never gates anything: every
 * update is started by the user, who sees the expected downtime against
 * what is left.
 *
 * Inputs (all push-style, so this L1 component depends on nobody above it):
 *   - ntrip_server reports each slot's streaming state on every change;
 *   - the station reports whether fresh RTCM is flowing;
 *   - the clock: GNSS time first (receiver time output or MSM epoch + week),
 *     SNTP as fallback. Until either is valid, seconds accrue to an
 *     "unattributed" bucket and are assigned to a day once time is known.
 *
 * Persistence: the running counters also live in RTC memory (survive
 * software/panic/watchdog resets); "last alive" is persisted every
 * CONFIG_LEDGER_ALIVE_PERSIST_S so the gap of a power cut is added as
 * downtime at the next boot; day totals roll into a CONFIG_LEDGER_HISTORY_DAYS
 * history at UTC midnight.
 *
 * Status: API settled; accounting core not implemented yet (pure core with an
 * injected clock, host-tested across day rollovers and power-cut gaps).
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
    int32_t utc_day;                          /**< days since 1970-01-01 */
    uint32_t down_s[LEDGER_MAX_SLOTS];        /**< seconds a slot should have streamed but did not */
    uint32_t planned_s[LEDGER_MAX_SLOTS];     /**< of which planned (receiver flash, app update) */
    uint32_t reconnects[LEDGER_MAX_SLOTS];
    uint32_t power_off_s;                     /**< power-cut gaps added at boot */
    uint32_t rx_overflows;                    /**< UART/driver overflow events */
} ledger_day_t;

/** Load today and the history; add a power-off gap if the last-alive mark is old. */
esp_err_t ledger_init(void);

/** Slot state, reported by ntrip_server on every change. */
void ledger_set_slot(size_t slot, bool enabled, bool protected_slot, bool streaming);

/** Planned downtime in progress (paused for receiver flash/config); counted separately. */
void ledger_set_planned(bool planned);

/** Fresh RTCM flowing from the receiver (reported by the station). */
void ledger_set_rtcm_ok(bool ok);

/** Feed a UTC time and its source; GNSS beats SNTP. */
void ledger_set_utc(int64_t unix_s, ledger_clock_t source);

/** Count a data-loss event against today (UART overflow etc.). */
void ledger_count_overflow(void);

/** 1 Hz tick from the health monitor: accrue, roll the day, persist. */
void ledger_tick(void);

/** Today's counters. */
esp_err_t ledger_get_today(ledger_day_t *out);

/** Past days, newest first; *count in = capacity, out = filled. */
esp_err_t ledger_get_history(ledger_day_t *days, size_t *count);

/** Protected-slot downtime so far this UTC day, seconds (worst protected slot). */
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
