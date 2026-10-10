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
 * planned downtime counts too and is also shown on its own as planned:
 * casters paused for receiver work, and a reboot the user asked for (see
 * "Planned reboots"). Slots flagged "protected" (the Onocoy slot)
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
 *   - the clock: GNSS time first, SNTP as fallback (taken only while no
 *     GNSS time arrived for 10 min). GNSS time is the receiver's GPS time
 *     of week (its GPS MSM epoch time, fed by the health monitor), which
 *     has no week number: it is dated against the running clock, or after
 *     a reset (not a power cut) against the previous session's last-alive
 *     mark. A station that never had a clock, or one that lost power, needs
 *     one SNTP time first. Until a clock is valid, seconds accrue to an
 *     "unattributed" record and are spread over the days they covered once
 *     time is known. Trusted time is passed on to diag for the event
 *     journal's timestamps.
 *
 * Planned reboots: a reboot that is a direct consequence of a user action
 * (lifecycle_reboot_user_planned(): restart, factory reset, app update or
 * rollback, leaving safe mode) is planned downtime, like receiver work. Its
 * shutdown hook (ledger_shutdown) counts the rest of the session as planned
 * and stores the books at once; the next boot counts the gap until it as
 * planned, and each expected slot's downtime from boot until that slot
 * streams again, for at most CONFIG_LEDGER_PLANNED_BOOT_S after the boot.
 * The next boot learns it from the stored books, or from lifecycle's record
 * of the previous boot (ledger_note_prev_reboot) when they could not say
 * so. Only that one boot does: right after reading the books it stores them
 * again without the flag, so a boot that crashes before it stores anything
 * else does not hand the flag on, and the books count only after a software
 * restart. Lifecycle's record of an unplanned end overrides them. Ladder,
 * low-heap, crash, safe-mode-retry and trial-revert reboots are not planned
 * (nor is the gap after a crash or the reset pin, whatever the books say),
 * and a power cut (or brownout) never is, whatever was going on when it hit.
 *
 * Persistence: the running counters also live in RTC memory, written every
 * tick and checked with magic, size and CRC (they survive software, panic
 * and watchdog resets, so a reboot costs at most a second of accounting).
 * The RTC copy also carries lifecycle's stamp (the image generation of the
 * run that wrote it) and is read back only when lifecycle accepts it: an
 * image that runs again after another one ran would otherwise find its own
 * copy from before that run and miss everything the other image stored.
 * The "last alive" mark is stored in settings every
 * CONFIG_LEDGER_ALIVE_PERSIST_S and at the start of every reboot through
 * lifecycle (ledger_shutdown), so after a power cut the time between that
 * mark and the next boot is added as downtime (and power-off time) for the
 * slots that were expected to stream, and a reboot into another app image
 * (whose RTC layout may differ) counts its gap from the moment the reboot
 * began. When today's counters changed they are stored with it as one blob
 * (ledger/today carries the mark it runs up to; NVS replaces a key as a
 * unit), otherwise the mark alone moves on (ledger/alive); without a clock,
 * today goes with the mark it was restored with, and with no mark at all
 * nothing is stored. So no failed write or power cut leaves the stored
 * today ahead of its mark, and the gap never counts time twice. A restore
 * from settings takes the later of the two marks (a today blob of the older
 * two-key layout has none: the alive key alone).
 * Day totals roll into a CONFIG_LEDGER_HISTORY_DAYS history at UTC midnight
 * (settings: ledger/hist, written when it changes). The whole ledger
 * namespace survives a factory reset.
 *
 * Accounting is a pure core (ledger_core.c, host-tested across clock wraps,
 * day rollovers, power-cut gaps, planned reboots and late clocks); ledger.c
 * adds the lock, the RTC copy and the settings writes. ledger_tick() is
 * driven at 1 Hz by the health monitor; it and ledger_shutdown() are the only
 * calls that write settings (serialised). Every other call is short and may
 * come from any task.
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
    uint32_t planned_s[LEDGER_MAX_SLOTS];     /**< of which planned (receiver work, reboots the user asked for) */
    uint32_t reconnects[LEDGER_MAX_SLOTS];
    uint32_t power_off_s;                     /**< power-cut gaps added at boot */
    uint32_t rx_overflows;                    /**< UART/driver overflow events */
} ledger_day_t;

/**
 * Load today and the history (RTC copy first, then settings). The gap since
 * the last-alive mark is added once the first trusted time arrives.
 * The ledger (L1) cannot call lifecycle (L1), so main passes its RTC stamp
 * check: `rtc_stamp_ok` (lifecycle_rtc_stamp_ok; NULL = never use the RTC
 * copy) decides whether the RTC copy comes from the run right before this
 * boot, and `rtc_stamp` (lifecycle_rtc_stamp()) is stamped into every RTC
 * copy this boot writes. A copy that fails falls back to settings, as after
 * a power cut but with the reset as it was.
 */
esp_err_t ledger_init(bool (*rtc_stamp_ok)(uint32_t stamp), uint32_t rtc_stamp);

/**
 * How the previous session ended, by lifecycle's record of it
 * (lifecycle_prev_session_end(); main calls it right after ledger_init,
 * before the slots are reported, and only when lifecycle has a record).
 * `planned`: it ended in a reboot the user asked for. Usually the stored
 * books say so already (ledger_shutdown); this covers a reboot whose books
 * could not be stored, and is ignored after a power cut, a crash or the
 * reset pin. Not planned (a crash, a failed app trial, a ladder): the gap
 * and this boot's reconnects are not planned, whatever the books say, and
 * nothing makes them planned again this boot.
 */
void ledger_note_prev_reboot(bool planned);

/**
 * A reboot begins (a lifecycle shutdown hook; main registers it first, so it
 * runs before the casters close): settle the books and store them now, so
 * the next boot counts the gap from here even in another app image that
 * cannot read the RTC copy. `planned`: the user asked for this reboot
 * (lifecycle_reboot_user_planned()); the rest of this session, the gap and
 * each caster's reconnect after the boot (at most
 * CONFIG_LEDGER_PLANNED_BOOT_S) count as planned. Takes at most a few
 * hundred ms (settings writes).
 */
void ledger_shutdown(bool planned);

/**
 * Slot state, reported by ntrip_server on every change. `enabled`: the slot
 * is configured to stream, whether or not the caster client runs (time
 * before it starts, in safe mode or while it shuts down is downtime too).
 * Until a slot is reported in a boot, its state from the previous session
 * is assumed (expected to stream if it was then, not streaming).
 */
void ledger_set_slot(size_t slot, bool enabled, bool protected_slot, bool streaming);

/** Planned receiver work in progress (casters paused for receiver flash/config); counted as down and also as planned. */
void ledger_set_planned(bool planned);

/** Fresh RTCM flowing from the receiver (reported by the station). */
void ledger_set_rtcm_ok(bool ok);

/**
 * Feed a UTC time and its source; GNSS beats SNTP. Times before 2026 are
 * ignored. The first one of a boot settles the time since the previous
 * session's last-alive mark and the unattributed seconds.
 */
void ledger_set_utc(int64_t unix_s, ledger_clock_t source);

/**
 * GNSS time from the receiver: the GPS time of week in ms of its latest MSM
 * epoch (as sent), `age_ms` after that MSM arrived. Dated and converted to
 * UTC as described above (GPS-UTC offset 18 s), then taken like
 * ledger_set_utc(..., LEDGER_CLOCK_GNSS); ignored when it cannot be dated.
 */
void ledger_set_gps_time(uint32_t tow_ms, uint32_t age_ms);

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
