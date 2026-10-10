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
 * GNSS time is the receiver's GPS time of week (lc_set_gps_tow: an MSM
 * epoch time), which carries no week number. It is dated against the
 * running clock (taken within LC_GNSS_SKEW_MS of it), or, before the first
 * clock of a boot, against the previous session's last-alive mark when that
 * session had a clock and the reset was not a power cut: the mark plus this
 * boot's uptime is then the time give or take the reset itself (taken from
 * LC_GNSS_BACK_MS before to LC_GNSS_RESET_MS after it). Anything else waits
 * for SNTP: after a power cut, or on a station that never had a clock, the
 * week cannot be known.
 *
 * What is counted, per slot and UTC day (milliseconds):
 *   - down: the slot was expected to stream (enabled; for a slot not
 *     reported yet this boot, enabled in the previous session) but was not
 *     streaming with fresh RTCM behind it (a session kept open through a
 *     receiver stall counts as down, which keeps the budget on the safe
 *     side);
 *   - planned: the part of down while planned receiver work paused the
 *     casters, and the part a reboot the user asked for caused (below);
 *   - reconnects: entries into streaming after a period of expected
 *     downtime;
 *   - per day also power-off time (gaps between the last-alive mark and a
 *     boot after a power cut) and UART overflow events.
 *
 * Planned reboots. A reboot the user asked for (restart, app update or
 * rollback, factory reset, leaving safe mode: the wrapper decides, from
 * lifecycle's reason) is planned downtime like receiver work:
 *   - lc_shutdown(planned) at the start of the reboot counts the rest of the
 *     session as planned and records it in the snapshot (reboot_planned),
 *     which the wrapper stores at once, so the record survives a switch to
 *     another app image too;
 *   - the next boot learns it from that snapshot (lc_restore) or from
 *     lifecycle's own record (lc_planned_boot, for when the snapshot was not
 *     stored): the gap since the last-alive mark is planned, and so is each
 *     expected slot's downtime from boot until that slot streams again, for
 *     at most the boot window (lc_set_boot_window) after boot. A slot that
 *     is up once is done: later outages count as unplanned again.
 * The flag belongs to the one boot that follows the planned reboot: only a
 * software restart ends that reboot (lc_restore with LC_RESET_SOFTWARE), and
 * the snapshot of a restored ledger no longer carries it, so the wrapper
 * stores that snapshot (and the stored today without the flag) right after
 * restoring. A boot that crashes before its first store therefore does not
 * hand the flag on: the gap after a crash, a watchdog or a reset pin
 * (LC_RESET_OTHER) is never planned, whatever the books say, and neither is
 * one lifecycle knows ended in a reboot that was not planned (a failed app
 * trial, a ladder: lc_unplanned_boot), which no later call undoes.
 * A power cut is never planned: after one, neither the snapshot's flags nor
 * lifecycle's record make the gap or the boot window planned (planned
 * receiver work that the cut interrupted included).
 *
 * Before the first trusted UTC of a boot, everything accrues into a
 * "pending" record. When UTC arrives:
 *   1. the gap between the previous session's last-alive mark and this
 *      boot's start (UTC now minus uptime) is counted as down for the slots
 *      that were expected to stream then (planned if planned work was going
 *      on or the session ended in a planned reboot, and the reset was not a
 *      power cut; also as power-off time after a power cut). Pending time carried
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
 * persist interval and at every reboot (power cuts). Today is only ever
 * stored together with the mark it runs up to (lc_persist_due), so a
 * restore from NVS does not count the time the stored today covers a second
 * time as gap. Blob codecs for NVS are below.
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

#define LC_GPS_EPOCH_UNIX_MS 315964800000LL   /* 1980-01-06 00:00 UTC, GPS week 0 */
#define LC_GPS_WEEK_MS       604800000LL
#define LC_GPS_UTC_LEAP_S    18               /* GPS time minus UTC since 2017-01-01 (no change announced) */
#define LC_GNSS_SKEW_MS      (10LL * 60000)   /* GNSS time taken within this of the running clock */
#define LC_GNSS_BACK_MS      60000LL          /* ...or of the last-alive mark plus uptime: this much before */
#define LC_GNSS_RESET_MS     (15LL * 60000)   /* ...to this much after (the reset itself) */

/** How the previous session ended, as far as the reset itself tells (lc_restore). */
typedef enum {
    LC_RESET_SOFTWARE = 0,   /**< a software restart: how every reboot through lifecycle ends */
    LC_RESET_POWER_CUT,      /**< power-on, brownout: nothing planned; the gap is power-off time too */
    LC_RESET_OTHER,          /**< a crash, a watchdog, the reset pin: never a planned reboot */
} lc_reset_t;

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
    bool planned;                       /**< planned receiver work was going on */
    bool reboot_planned;                /**< the session is ending in a reboot the user asked for (lc_shutdown) */
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
    bool rebooting;                     /**< a reboot the user asked for is under way (lc_shutdown) */

    /* after a planned reboot */
    uint64_t boot_window_ms;            /**< how long after boot a slot's reconnect counts as planned */
    uint8_t boot_planned_mask;          /**< slots whose downtime is still planned (not up since boot) */
    bool boot_planned_started;          /**< lc_planned_boot ran (once per boot) */

    /* records */
    lc_day_t today;
    lc_day_t pending;
    uint64_t pending_ms;
    uint64_t pending_boot_ms;           /**< part of pending_ms accrued in this boot */

    /* the previous session */
    bool alive_valid;
    int64_t alive_ms;
    bool gap_power_off;
    bool prev_planned;                  /**< planned receiver work was going on at its end */
    bool prev_reboot_planned;           /**< it ended in a reboot the user asked for */
    bool prev_unplanned;                /**< it is known not to have (crash, reset pin, lifecycle's record) */

    /* persistence */
    bool today_dirty;
    bool hist_dirty;
    bool rolled;                        /**< a finished day moved into the history: store it now */
    bool alive_saved;
    uint32_t alive_saved_ms;
} lc_t;

/**
 * Start a ledger at monotonic `now_ms`, `uptime_ms` after boot: the time
 * since boot is accounted on the first accrual. `hist` holds `hist_cap` days.
 */
void lc_init(lc_t *c, lc_day_t *hist, size_t hist_cap, uint32_t now_ms, uint64_t uptime_ms);

/**
 * How long after boot a slot's reconnect still counts as planned after a
 * planned reboot (0 = only the gap until the boot). Call before lc_restore.
 */
void lc_set_boot_window(lc_t *c, uint32_t window_ms);

/**
 * Carry state over from the previous session (before any other call but
 * lc_init, lc_set_boot_window and lc_set_history). `reset`: how it ended.
 * After a power cut the gap since snap->alive_ms is power-off time too and
 * never planned. A snapshot that says the session ended in a planned reboot
 * starts the boot as lc_planned_boot() does, after a software restart only:
 * any other reset means a later boot ended (a crash before it stored its
 * books), and the gap is not planned (lc_unplanned_boot). The restored
 * today is stored again with the next mark (it may be newer than the
 * stored one: an RTC copy).
 */
void lc_restore(lc_t *c, const lc_snapshot_t *snap, lc_reset_t reset);

/**
 * The previous session ended in a reboot the user asked for (lifecycle's
 * record of it, for when the snapshot could not say so): its gap counts as
 * planned, and so does each expected slot's downtime from boot until it
 * streams again, within the boot window. Ignored after a power cut and
 * after lc_unplanned_boot. Call right after lc_restore, before the first
 * input; calling it again changes nothing.
 */
void lc_planned_boot(lc_t *c);

/**
 * The previous session is known not to have ended in a reboot the user
 * asked for (lifecycle's record: a crash, a failed app trial, a ladder):
 * whatever the snapshot said, its gap and this boot's reconnects are not
 * planned (planned receiver work at its end still makes the gap planned),
 * and lc_planned_boot is ignored from here on. Call right after lc_restore,
 * before the first input.
 */
void lc_unplanned_boot(lc_t *c);

/**
 * A reboot starts now (a shutdown hook). Accrues up to now_ms; when
 * `planned` (the user asked for it), the rest of the session counts as
 * planned and the snapshot carries reboot_planned for the next boot. Then
 * the wrapper stores the books at once (lc_persist_due with interval 0).
 */
void lc_shutdown(lc_t *c, uint32_t now_ms, bool planned);

/** Load the history (newest first; older entries and duplicates are dropped). */
void lc_set_history(lc_t *c, const lc_day_t *days, size_t n);

/**
 * The day last stored as today (NVS), after the RTC copy was restored: if
 * it is earlier than the restored today and the history does not hold it,
 * it goes into the history. A reset right after UTC midnight, before the
 * finished day reached the stored history, then keeps that day (as of its
 * last store). Call after lc_restore and lc_set_history.
 */
void lc_merge_stored_day(lc_t *c, const lc_day_t *day);

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

/**
 * GPS time of week from the receiver: `tow_ms` (ms of the GPS week, an MSM
 * epoch time) seen `age_ms` ago. Dated as described above and converted to
 * UTC (LC_GPS_UTC_LEAP_S), then taken like lc_set_utc(..., LC_CLOCK_GNSS).
 * @return true if taken.
 */
bool lc_set_gps_tow(lc_t *c, uint32_t now_ms, uint32_t tow_ms, uint32_t age_ms);

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
 * Time to store the last-alive mark (with a clock, every `interval_ms`, and
 * at once when a finished day moved into the history: the RTC copy holds
 * only today, so until the history is stored a reset would lose that day).
 * `interval_ms` 0 stores now (a reboot); without a clock that is the mark
 * the session was restored with (today has not grown since: this boot's
 * time is pending, kept only in the RTC copy, and a restore from NVS counts
 * it as part of the gap), and nothing at all without any mark.
 * On true, *alive_ms is the mark; *today / *hist say whether those records
 * changed since they were last stored (the flags are cleared: call
 * lc_mark_dirty if storing fails; a failed store waits for the interval).
 * The caller stores today only together with this mark.
 */
bool lc_persist_due(lc_t *c, uint32_t now_ms, uint32_t interval_ms, int64_t *alive_ms, bool *today, bool *hist);

/** Storing failed: write these again next time. */
void lc_mark_dirty(lc_t *c, bool today, bool hist);

/* ---- NVS blobs (settings_codec, version byte first; newer writers may append) ---------- */

/** Today's record with the slot masks. @return bytes written, 0 if cap is too small. */
size_t lc_encode_today(const lc_snapshot_t *s, uint8_t *buf, size_t cap);

/**
 * Decode into s->today, s->expected_mask, s->protected_mask, s->planned and
 * s->reboot_planned (false in a blob written before it existed); other
 * fields untouched.
 */
bool lc_decode_today(const uint8_t *buf, size_t len, lc_snapshot_t *s);

/** History, newest first. @return bytes written, 0 if cap is too small. */
size_t lc_encode_hist(const lc_day_t *days, size_t n, uint8_t *buf, size_t cap);

/** @return days decoded (at most cap); 0 also for a blob that cannot be read. */
size_t lc_decode_hist(const uint8_t *buf, size_t len, lc_day_t *days, size_t cap);

/** Largest encoded history for `n` days. */
size_t lc_hist_blob_max(size_t n);

/* ---- RTC copy --------------------------------------------------------------------------- */

#define LC_RTC_MAGIC   0x4C444752u   /* "RGDL" */
#define LC_RTC_VERSION 2u   /* 2: the snapshot carries reboot_planned */

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
