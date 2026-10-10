/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Availability accounting (pure logic, no IDF or RTOS includes,
 * host-testable). ledger.c wraps it with a lock, the RTC copy and NVS.
 *
 * Time. Accounting runs on the monotonic clock (`now_ms`, any u32 that wraps
 * freely); every call first accrues the time since the previous one with the
 * state that held during it. UTC comes from lc_set_utc(): GNSS time is taken
 * whenever it comes; SNTP only while no GNSS time arrived for
 * LC_GNSS_HOLD_MS (GNSS time first, SNTP as fallback). Times before
 * LC_UTC_FLOOR_S are never trusted. Between samples UTC advances with the
 * monotonic clock. A UTC jump moves the day forward when it crosses
 * midnight, never back: a clock corrected backwards keeps counting into the
 * current day until it catches up.
 *
 * What is counted, per slot and UTC day (milliseconds):
 *   - down: the slot was expected to stream (enabled; for a slot not
 *     reported yet this boot, enabled in the previous session) but was not
 *     streaming with fresh RTCM behind it (a session kept open through a
 *     receiver stall counts as down, which keeps the budget on the safe
 *     side);
 *   - planned: the part of down while planned receiver work paused the
 *     casters;
 *   - reconnects: entries into streaming after a period of expected
 *     downtime;
 *   - per day also power-off time (gaps between the last-alive mark and a
 *     boot after a power cut) and UART overflow events.
 *
 * Before the first trusted UTC of a boot, everything accrues into a
 * "pending" record. When UTC arrives:
 *   1. the gap between the previous session's last-alive mark and this
 *      boot's start (UTC now minus uptime) is counted as down for the slots
 *      that were expected to stream then (planned if planned work was going
 *      on; also as power-off time after a power cut). Pending time carried
 *      over from an earlier boot without a clock is taken out of the gap,
 *      since it covers part of it. The gap is capped at the history length
 *      plus one day;
 *   2. the pending record is spread over the days its span covers (the span
 *      ends now), in proportion to the time on each day; event counts go to
 *      today.
 * Then today follows the clock: crossing UTC midnight moves today into the
 * history (newest first, at most `hist_cap` days, none older than that).
 *
 * The state that must outlive a reset is a snapshot (lc_snapshot_t): the
 * wrapper keeps one in RTC memory every second (software, panic and watchdog
 * resets) and stores today in NVS with the last-alive mark every
 * persist interval (power cuts). Blob codecs for NVS are below.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LC_SLOTS        8u
#define LC_DAY_MS       86400000LL
#define LC_UTC_FLOOR_S  1767225600LL          /* 2026-01-01 00:00 UTC */
#define LC_GNSS_HOLD_MS (600u * 1000u)        /* SNTP is ignored this long after a GNSS time */

/** Clock source; same values as ledger_clock_t. */
typedef enum {
    LC_CLOCK_NONE = 0,
    LC_CLOCK_SNTP,
    LC_CLOCK_GNSS,
} lc_clock_t;

/** One UTC day (or the pending record, whose utc_day is unused). */
typedef struct {
    int32_t utc_day;                    /**< days since 1970-01-01, -1 = not known */
    uint32_t down_ms[LC_SLOTS];
    uint32_t planned_ms[LC_SLOTS];
    uint32_t reconnects[LC_SLOTS];
    uint32_t power_off_ms;
    uint32_t rx_overflows;
} lc_day_t;

/** What must survive a reset. */
typedef struct {
    lc_day_t today;
    lc_day_t pending;
    uint64_t pending_ms;                /**< time covered by `pending` */
    int64_t alive_ms;                   /**< UTC ms accounted up to (0 = never had a clock) */
    uint8_t expected_mask;              /**< slots expected to stream at the time */
    uint8_t protected_mask;
    bool planned;
} lc_snapshot_t;

typedef struct {
    /* history, newest first (caller's storage) */
    lc_day_t *hist;
    size_t hist_cap;
    size_t hist_n;

    /* clock */
    lc_clock_t clock;
    int64_t unix_ms;                    /**< UTC accounted up to (valid with a clock) */
    uint32_t last_ms;                   /**< monotonic time accounted up to */
    uint64_t uptime_ms;                 /**< accounted since boot */
    bool gnss_seen;
    uint32_t gnss_at_ms;

    /* inputs */
    uint8_t known_mask;                 /**< slots reported this boot */
    uint8_t enabled_mask;
    uint8_t protected_mask;
    uint8_t streaming_mask;
    uint8_t prev_expected_mask;         /**< previous session: for slots not reported yet, and its gap */
    uint8_t prev_protected_mask;
    bool planned;
    bool rtcm_ok;

    /* records */
    lc_day_t today;
    lc_day_t pending;
    uint64_t pending_ms;
    uint64_t pending_boot_ms;           /**< part of pending_ms accrued in this boot */

    /* the previous session */
    bool alive_valid;
    int64_t alive_ms;
    bool gap_power_off;
    bool prev_planned;

    /* persistence */
    bool today_dirty;
    bool hist_dirty;
    bool alive_saved;
    uint32_t alive_saved_ms;
} lc_t;

/**
 * Start a ledger at monotonic `now_ms`, `uptime_ms` after boot: the time
 * since boot is accounted on the first accrual. `hist` holds `hist_cap` days.
 */
void lc_init(lc_t *c, lc_day_t *hist, size_t hist_cap, uint32_t now_ms, uint64_t uptime_ms);

/**
 * Carry state over from the previous session (before any other call but
 * lc_init and lc_set_history). `power_cut`: the gap since snap->alive_ms was
 * a power cut (counted as power-off time too).
 */
void lc_restore(lc_t *c, const lc_snapshot_t *snap, bool power_cut);

/** Load the history (newest first; older entries and duplicates are dropped). */
void lc_set_history(lc_t *c, const lc_day_t *days, size_t n);

/** Accrue the time up to now_ms (also rolls the day). */
void lc_tick(lc_t *c, uint32_t now_ms);

/** Slot state as reported by the caster client. */
void lc_set_slot(lc_t *c, uint32_t now_ms, size_t slot, bool enabled, bool protected_slot, bool streaming);

/** Planned receiver work in progress. */
void lc_set_planned(lc_t *c, uint32_t now_ms, bool planned);

/** Fresh RTCM flowing from the receiver. */
void lc_set_rtcm_ok(lc_t *c, uint32_t now_ms, bool ok);

/**
 * A UTC sample. @return true if it was taken (false: before the floor, or
 * SNTP while GNSS time is recent).
 */
bool lc_set_utc(lc_t *c, uint32_t now_ms, int64_t unix_ms, lc_clock_t source);

/** Count one UART overflow event. */
void lc_count_overflow(lc_t *c, uint32_t now_ms);

/** UTC ms now, or -1 without a clock. */
int64_t lc_unix_now(const lc_t *c, uint32_t now_ms);

/**
 * Downtime of the protected slots today (the worst one), ms. Without a
 * clock: the last known day plus what is pending.
 */
uint64_t lc_protected_down_ms(const lc_t *c);

/** Today, with the pending record added while there is no clock. */
void lc_today(const lc_t *c, lc_day_t *out);

/** What must survive a reset, as of the last accrual. */
void lc_snapshot(const lc_t *c, lc_snapshot_t *out);

/**
 * Time to store the last-alive mark (with a clock, every `interval_ms`).
 * On true, *alive_ms is the mark; *today / *hist say whether those records
 * changed since they were last stored (the flags are cleared: call
 * lc_mark_dirty if storing fails).
 */
bool lc_persist_due(lc_t *c, uint32_t now_ms, uint32_t interval_ms, int64_t *alive_ms, bool *today, bool *hist);

/** Storing failed: write these again next time. */
void lc_mark_dirty(lc_t *c, bool today, bool hist);

/* ---- NVS blobs (settings_codec, version byte first; newer writers may append) ---------- */

/** Today's record with the slot masks. @return bytes written, 0 if cap is too small. */
size_t lc_encode_today(const lc_snapshot_t *s, uint8_t *buf, size_t cap);

/** Decode into s->today, s->expected_mask, s->protected_mask, s->planned (other fields untouched). */
bool lc_decode_today(const uint8_t *buf, size_t len, lc_snapshot_t *s);

/** History, newest first. @return bytes written, 0 if cap is too small. */
size_t lc_encode_hist(const lc_day_t *days, size_t n, uint8_t *buf, size_t cap);

/** @return days decoded (at most cap); 0 also for a blob that cannot be read. */
size_t lc_decode_hist(const uint8_t *buf, size_t len, lc_day_t *days, size_t cap);

/** Largest encoded history for `n` days. */
size_t lc_hist_blob_max(size_t n);

/* ---- RTC copy --------------------------------------------------------------------------- */

#define LC_RTC_MAGIC   0x4C444752u   /* "RGDL" */
#define LC_RTC_VERSION 1u

/** The block kept in RTC memory (only ever read back by the same image). */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    lc_snapshot_t snap;
    uint32_t crc;
} lc_rtc_t;

/** Fill and seal a block. */
void lc_rtc_seal(lc_rtc_t *r, const lc_snapshot_t *snap);

/** True if the block's magic, version, size and CRC check out. */
bool lc_rtc_valid(const lc_rtc_t *r);

#ifdef __cplusplus
}
#endif
