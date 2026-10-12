/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ledger_core.h"

#include <string.h>

#include "settings_codec.h"

#define BLOB_VERSION 1u

static uint32_t sat_add(uint32_t a, uint64_t b)
{
    uint64_t s = (uint64_t)a + b;
    return (s > UINT32_MAX) ? UINT32_MAX : (uint32_t)s;
}

static int32_t day_of(int64_t unix_ms)
{
    return (int32_t)(unix_ms / LC_DAY_MS);   /* unix_ms is past the floor, so positive */
}

static uint8_t bit(size_t slot)
{
    return (uint8_t)(1u << slot);
}

/* ---- inputs ------------------------------------------------------------------------- */

static uint8_t expected_mask(const lc_t *c)
{
    return (uint8_t)((c->known_mask & c->enabled_mask) | (~c->known_mask & c->prev_expected_mask));
}

static uint8_t protected_mask(const lc_t *c)
{
    return (uint8_t)((c->known_mask & c->protected_mask) | (~c->known_mask & c->prev_protected_mask));
}

static uint8_t up_mask(const lc_t *c)
{
    return c->rtcm_ok ? (uint8_t)(c->known_mask & c->streaming_mask) : 0u;
}

/* After a planned reboot: a slot that streams again is no longer in its planned reconnect. */
static void note_up(lc_t *c)
{
    c->boot_planned_mask &= (uint8_t)~up_mask(c);
}

/* ---- history ------------------------------------------------------------------------- */

static void mark(lc_t *c, const lc_day_t *r)
{
    if (r == &c->today) {
        c->today_dirty = true;
    } else if (r != &c->pending) {
        c->hist_dirty = true;
    }
}

static void add_day(lc_day_t *dst, const lc_day_t *src)
{
    for (size_t s = 0; s < LC_SLOTS; s++) {
        dst->down_ms[s] = sat_add(dst->down_ms[s], src->down_ms[s]);
        dst->planned_ms[s] = sat_add(dst->planned_ms[s], src->planned_ms[s]);
        dst->reconnects[s] = sat_add(dst->reconnects[s], src->reconnects[s]);
        dst->up_ms[s] = sat_add(dst->up_ms[s], src->up_ms[s]);
    }
    dst->power_off_ms = sat_add(dst->power_off_ms, src->power_off_ms);
    dst->rx_overflows = sat_add(dst->rx_overflows, src->rx_overflows);
}

/* Days older than this are not kept. */
static int32_t oldest_kept(const lc_t *c)
{
    return c->today.utc_day - (int32_t)c->hist_cap;
}

/* Record for an earlier day: found, or inserted in order; NULL if too old. */
static lc_day_t *hist_slot(lc_t *c, int32_t day)
{
    if (c->hist_cap == 0 || day < oldest_kept(c)) {
        return NULL;
    }
    size_t i = 0;
    while (i < c->hist_n && c->hist[i].utc_day > day) {
        i++;
    }
    if (i < c->hist_n && c->hist[i].utc_day == day) {
        return &c->hist[i];
    }
    if (i >= c->hist_cap) {
        return NULL;                    /* full, and older than everything kept */
    }
    size_t n = (c->hist_n < c->hist_cap) ? c->hist_n : c->hist_cap - 1u;
    memmove(&c->hist[i + 1u], &c->hist[i], (n - i) * sizeof(lc_day_t));
    c->hist_n = n + 1u;
    memset(&c->hist[i], 0, sizeof(lc_day_t));
    c->hist[i].utc_day = day;
    c->hist_dirty = true;
    return &c->hist[i];
}

static void trim_hist(lc_t *c)
{
    while (c->hist_n > 0 && c->hist[c->hist_n - 1u].utc_day < oldest_kept(c)) {
        c->hist_n--;
        c->hist_dirty = true;
    }
}

/* The record that collects `day`; today moves forward when the day is later. */
static lc_day_t *rec_for_day(lc_t *c, int32_t day)
{
    if (c->today.utc_day < 0) {
        c->today.utc_day = day;
        c->today_dirty = true;
        return &c->today;
    }
    if (day == c->today.utc_day) {
        return &c->today;
    }
    if (day < c->today.utc_day) {
        return hist_slot(c, day);
    }
    lc_day_t old = c->today;
    memset(&c->today, 0, sizeof(c->today));
    c->today.utc_day = day;
    c->today_dirty = true;
    lc_day_t *h = hist_slot(c, old.utc_day);   /* NULL: a jump left it too old to keep */
    if (h != NULL) {
        add_day(h, &old);
        c->rolled = true;
    }
    trim_hist(c);
    return &c->today;
}

/* ---- accrual ------------------------------------------------------------------------- */

/*
 * `planned`: the slots whose downtime counts as planned. Online time alone
 * does not make the record dirty: while every expected slot streams, today
 * is not stored again and the mark alone moves on (lc_persist_due), and a
 * restore from settings adds the time between the two marks as online
 * (lc_stored_online): nothing else can have happened in it.
 */
static void add_state(lc_t *c, lc_day_t *r, uint64_t ms, uint8_t expected, uint8_t up, uint8_t planned)
{
    uint8_t down = (uint8_t)(expected & ~up);
    uint8_t online = (uint8_t)(expected & up);
    for (size_t s = 0; s < LC_SLOTS; s++) {
        if (down & bit(s)) {
            r->down_ms[s] = sat_add(r->down_ms[s], ms);
            if (planned & bit(s)) {
                r->planned_ms[s] = sat_add(r->planned_ms[s], ms);
            }
        } else if (online & bit(s)) {
            r->up_ms[s] = sat_add(r->up_ms[s], ms);
        }
    }
    if (down != 0) {
        mark(c, r);
    }
}

/* Spread [start, start + ms) over UTC days with a fixed state. */
static void accrue_span(lc_t *c, int64_t start, uint64_t ms, uint8_t expected, uint8_t up, uint8_t planned,
                        bool power_off)
{
    int64_t t = start;
    uint64_t left = ms;
    while (left > 0) {
        int32_t day = day_of(t);
        int64_t day_end = ((int64_t)day + 1) * LC_DAY_MS;
        uint64_t piece = (uint64_t)(day_end - t);
        if (piece > left) {
            piece = left;
        }
        lc_day_t *r = rec_for_day(c, day);
        if (r != NULL) {
            add_state(c, r, piece, expected, up, planned);
            if (power_off) {
                r->power_off_ms = sat_add(r->power_off_ms, piece);
                mark(c, r);
            }
        }
        t += (int64_t)piece;
        left -= piece;
    }
}

/* Accrue `ms` with one state; `planned` as in add_state. */
static void accrue_piece(lc_t *c, uint64_t ms, uint8_t planned)
{
    if (c->clock != LC_CLOCK_NONE) {
        accrue_span(c, c->unix_ms, ms, expected_mask(c), up_mask(c), planned, false);
        c->unix_ms += (int64_t)ms;
        (void)rec_for_day(c, day_of(c->unix_ms));   /* at exactly midnight, today is the new day */
    } else {
        add_state(c, &c->pending, ms, expected_mask(c), up_mask(c), planned);
        c->pending_ms += ms;
        c->pending_boot_ms += ms;
    }
    c->uptime_ms += ms;
}

static void accrue(lc_t *c, uint64_t ms)
{
    while (ms > 0) {
        uint64_t piece = ms;
        uint8_t planned = (c->planned || c->rebooting) ? 0xFFu : 0u;
        if (c->boot_planned_mask != 0) {
            if (c->uptime_ms >= c->boot_window_ms) {
                c->boot_planned_mask = 0;   /* the window after a planned reboot is over */
            } else {
                uint64_t left = c->boot_window_ms - c->uptime_ms;
                if (piece > left) {
                    piece = left;           /* the rest of this step falls after the window */
                }
                planned |= c->boot_planned_mask;
            }
        }
        accrue_piece(c, piece, planned);
        ms -= piece;
    }
}

static void advance(lc_t *c, uint32_t now_ms)
{
    uint32_t d = now_ms - c->last_ms;
    c->last_ms = now_ms;
    accrue(c, d);
}

/* The record events go to now. */
static lc_day_t *event_rec(lc_t *c)
{
    if (c->clock == LC_CLOCK_NONE) {
        return &c->pending;
    }
    lc_day_t *r = rec_for_day(c, day_of(c->unix_ms));
    return (r != NULL) ? r : &c->today;
}

/* ---- the first trusted time of a boot ----------------------------------------------------- */

static uint32_t share(uint32_t total, uint64_t part, uint64_t whole)
{
    return (whole == 0) ? 0 : (uint32_t)((uint64_t)total * part / whole);
}

/* Spread the pending record over [end - pending_ms, end), by time per day. */
static void spread_pending(lc_t *c, int64_t end)
{
    lc_day_t p = c->pending;
    uint64_t span = c->pending_ms;
    lc_day_t done = { 0 };
    int64_t t = end - (int64_t)span;
    while (span > 0 && t < end) {
        int32_t day = day_of(t);
        int64_t day_end = ((int64_t)day + 1) * LC_DAY_MS;
        bool last = day_end >= end;
        uint64_t piece = (uint64_t)((last ? end : day_end) - t);
        lc_day_t *r = rec_for_day(c, day);
        lc_day_t add = { 0 };
        for (size_t s = 0; s < LC_SLOTS; s++) {
            add.down_ms[s] = last ? p.down_ms[s] - done.down_ms[s] : share(p.down_ms[s], piece, span);
            add.planned_ms[s] = last ? p.planned_ms[s] - done.planned_ms[s] : share(p.planned_ms[s], piece, span);
            add.up_ms[s] = last ? p.up_ms[s] - done.up_ms[s] : share(p.up_ms[s], piece, span);
        }
        add_day(&done, &add);
        if (r != NULL) {
            add_day(r, &add);
            mark(c, r);
        }
        t += (int64_t)piece;
    }
    /* Event counts go to the day that ends the span. */
    lc_day_t *r = rec_for_day(c, day_of(end));
    if (r != NULL) {
        for (size_t s = 0; s < LC_SLOTS; s++) {
            r->reconnects[s] = sat_add(r->reconnects[s], p.reconnects[s]);
        }
        r->rx_overflows = sat_add(r->rx_overflows, p.rx_overflows);
        r->power_off_ms = sat_add(r->power_off_ms, p.power_off_ms);
        mark(c, r);
    }
    memset(&c->pending, 0, sizeof(c->pending));
    c->pending.utc_day = -1;
    c->pending_ms = 0;
    c->pending_boot_ms = 0;
}

static void first_clock(lc_t *c, int64_t unix_ms)
{
    int64_t boot_ms = unix_ms - (int64_t)c->uptime_ms;
    uint64_t inherited = c->pending_ms - c->pending_boot_ms;
    if (c->alive_valid) {
        int64_t gap = boot_ms - c->alive_ms - (int64_t)inherited;
        if (gap > 0) {
            int64_t max_gap = ((int64_t)c->hist_cap + 1) * LC_DAY_MS;
            int64_t start = c->alive_ms;
            if (gap > max_gap) {
                start += gap - max_gap;
                gap = max_gap;
            }
            bool planned = c->prev_planned || c->prev_reboot_planned;
            accrue_span(c, start, (uint64_t)gap, c->prev_expected_mask, 0, planned ? 0xFFu : 0u, c->gap_power_off);
        }
    }
    spread_pending(c, unix_ms);
    c->unix_ms = unix_ms;
    (void)rec_for_day(c, day_of(unix_ms));
    c->alive_valid = true;
    c->alive_ms = unix_ms;
}

/* ---- API ---------------------------------------------------------------------------------- */

void lc_init(lc_t *c, lc_day_t *hist, size_t hist_cap, uint32_t now_ms, uint64_t uptime_ms)
{
    memset(c, 0, sizeof(*c));
    c->hist = hist;
    c->hist_cap = (hist == NULL) ? 0 : hist_cap;
    c->last_ms = now_ms - (uint32_t)uptime_ms;
    c->today.utc_day = -1;
    c->pending.utc_day = -1;
}

void lc_set_boot_window(lc_t *c, uint32_t window_ms)
{
    c->boot_window_ms = window_ms;
}

void lc_planned_boot(lc_t *c)
{
    if (c->gap_power_off || c->prev_unplanned) {
        return;   /* a power cut, a crash or a reboot known not to be planned never is */
    }
    c->prev_reboot_planned = true;
    if (c->uptime_ms < c->boot_window_ms && c->boot_planned_mask == 0 && !c->boot_planned_started) {
        c->boot_planned_mask = 0xFFu;   /* every slot until it streams again */
    }
    c->boot_planned_started = true;
}

void lc_unplanned_boot(lc_t *c)
{
    c->prev_unplanned = true;
    c->prev_reboot_planned = false;
    c->boot_planned_mask = 0;   /* nothing accrued yet: the window never started */
}

void lc_restore(lc_t *c, const lc_snapshot_t *snap, lc_reset_t reset)
{
    bool power_cut = reset == LC_RESET_POWER_CUT;
    c->today = snap->today;
    if (c->today.utc_day < 0) {
        memset(&c->today, 0, sizeof(c->today));
        c->today.utc_day = -1;
    }
    c->pending = snap->pending;
    c->pending.utc_day = -1;
    c->pending_ms = snap->pending_ms;
    c->pending_boot_ms = 0;
    c->prev_expected_mask = snap->expected_mask;
    c->prev_protected_mask = snap->protected_mask;
    c->prev_planned = snap->planned && !power_cut;   /* a power cut is never planned */
    c->planned = false;
    c->alive_valid = snap->alive_ms >= LC_UTC_FLOOR_S * 1000;
    c->alive_ms = snap->alive_ms;
    c->gap_power_off = power_cut;
    trim_hist(c);
    /* Stored again with the next mark: an RTC copy may be newer than the
     * stored today, and the stored flags belong to the session that ended. */
    c->today_dirty = true;
    if (reset == LC_RESET_OTHER) {
        lc_unplanned_boot(c);   /* the flag, if any, is from a boot before the one that crashed */
    } else if (snap->reboot_planned) {
        lc_planned_boot(c);     /* ignored after a power cut */
    }
}

void lc_set_history(lc_t *c, const lc_day_t *days, size_t n)
{
    c->hist_n = 0;
    for (size_t i = 0; i < n && c->hist_n < c->hist_cap; i++) {
        if (days[i].utc_day < 0 || (c->hist_n > 0 && days[i].utc_day >= c->hist[c->hist_n - 1u].utc_day)) {
            continue;   /* not newest-first: drop */
        }
        if (c->today.utc_day >= 0 && days[i].utc_day >= c->today.utc_day) {
            continue;
        }
        c->hist[c->hist_n++] = days[i];
    }
    trim_hist(c);
}

void lc_merge_stored_day(lc_t *c, const lc_day_t *day)
{
    if (day == NULL || day->utc_day < 0 || c->today.utc_day < 0 || day->utc_day >= c->today.utc_day) {
        return;
    }
    for (size_t i = 0; i < c->hist_n; i++) {
        if (c->hist[i].utc_day == day->utc_day) {
            return;   /* the stored history has it (as stored at or after midnight) */
        }
    }
    lc_day_t *h = hist_slot(c, day->utc_day);
    if (h != NULL) {
        add_day(h, day);
        c->rolled = true;
    }
}

void lc_tick(lc_t *c, uint32_t now_ms)
{
    advance(c, now_ms);
}

void lc_set_slot(lc_t *c, uint32_t now_ms, size_t slot, bool enabled, bool protected_slot, bool streaming)
{
    if (slot >= LC_SLOTS) {
        return;
    }
    advance(c, now_ms);
    uint8_t b = bit(slot);
    bool was_expected = (expected_mask(c) & b) != 0;
    bool was_streaming = (c->known_mask & c->streaming_mask & b) != 0;
    uint8_t old_expected = expected_mask(c);
    uint8_t old_protected = protected_mask(c);
    if (streaming && !was_streaming && was_expected && enabled) {
        lc_day_t *r = event_rec(c);
        r->reconnects[slot] = sat_add(r->reconnects[slot], 1);
        mark(c, r);
    }
    c->known_mask |= b;
    c->enabled_mask = enabled ? (uint8_t)(c->enabled_mask | b) : (uint8_t)(c->enabled_mask & ~b);
    c->protected_mask = protected_slot ? (uint8_t)(c->protected_mask | b) : (uint8_t)(c->protected_mask & ~b);
    c->streaming_mask = streaming ? (uint8_t)(c->streaming_mask | b) : (uint8_t)(c->streaming_mask & ~b);
    note_up(c);
    if (expected_mask(c) != old_expected || protected_mask(c) != old_protected) {
        c->today_dirty = true;          /* the masks are stored with today */
    }
}

void lc_set_planned(lc_t *c, uint32_t now_ms, bool planned)
{
    advance(c, now_ms);
    if (c->planned != planned) {
        c->planned = planned;
        c->today_dirty = true;
    }
}

void lc_set_rtcm_ok(lc_t *c, uint32_t now_ms, bool ok)
{
    advance(c, now_ms);
    c->rtcm_ok = ok;
    note_up(c);
}

void lc_shutdown(lc_t *c, uint32_t now_ms, bool planned)
{
    advance(c, now_ms);
    if (planned && !c->rebooting) {
        c->rebooting = true;
        c->today_dirty = true;   /* the flag is stored with today */
    }
}

bool lc_set_utc(lc_t *c, uint32_t now_ms, int64_t unix_ms, lc_clock_t source)
{
    if (source == LC_CLOCK_NONE || unix_ms < LC_UTC_FLOOR_S * 1000) {
        return false;
    }
    advance(c, now_ms);
    if (source == LC_CLOCK_SNTP && c->gnss_seen && (uint32_t)(now_ms - c->gnss_at_ms) < LC_GNSS_HOLD_MS) {
        return false;
    }
    if (source == LC_CLOCK_GNSS) {
        c->gnss_seen = true;
        c->gnss_at_ms = now_ms;
    }
    if (c->clock == LC_CLOCK_NONE) {
        first_clock(c, unix_ms);
    } else {
        c->unix_ms = unix_ms;
        (void)rec_for_day(c, day_of(unix_ms));   /* forward only */
    }
    c->clock = source;
    return true;
}

bool lc_set_gps_tow(lc_t *c, uint32_t now_ms, uint32_t tow_ms, uint32_t age_ms)
{
    if ((int64_t)tow_ms >= LC_GPS_WEEK_MS) {
        return false;
    }
    advance(c, now_ms);
    int64_t est;
    int64_t lo;
    int64_t hi;
    if (c->clock != LC_CLOCK_NONE) {
        est = c->unix_ms;
        lo = est - LC_GNSS_SKEW_MS;
        hi = est + LC_GNSS_SKEW_MS;
    } else if (c->alive_valid && !c->gap_power_off && c->pending_ms == c->pending_boot_ms) {
        /* The previous session had a clock to its end and only a reset lies
         * between: its last-alive mark plus this boot's uptime. */
        est = c->alive_ms + (int64_t)c->uptime_ms;
        lo = est - LC_GNSS_BACK_MS;
        hi = est + LC_GNSS_RESET_MS;
    } else {
        return false;   /* no week to date it with */
    }
    /* The week that puts the time nearest the estimate. */
    int64_t base = LC_GPS_EPOCH_UNIX_MS - (int64_t)LC_GPS_UTC_LEAP_S * 1000 + (int64_t)tow_ms + (int64_t)age_ms;
    int64_t d = est - base;
    int64_t week = (d >= 0) ? (d + LC_GPS_WEEK_MS / 2) / LC_GPS_WEEK_MS : -((-d + LC_GPS_WEEK_MS / 2) / LC_GPS_WEEK_MS);
    int64_t unix_ms = base + week * LC_GPS_WEEK_MS;
    if (unix_ms < lo || unix_ms > hi) {
        return false;
    }
    return lc_set_utc(c, now_ms, unix_ms, LC_CLOCK_GNSS);
}

void lc_count_overflow(lc_t *c, uint32_t now_ms)
{
    advance(c, now_ms);
    lc_day_t *r = event_rec(c);
    r->rx_overflows = sat_add(r->rx_overflows, 1);
    mark(c, r);
}

int64_t lc_unix_now(const lc_t *c, uint32_t now_ms)
{
    if (c->clock == LC_CLOCK_NONE) {
        return -1;
    }
    return c->unix_ms + (int64_t)(uint32_t)(now_ms - c->last_ms);
}

void lc_today(const lc_t *c, lc_day_t *out)
{
    *out = c->today;
    if (c->clock == LC_CLOCK_NONE) {
        lc_day_t p = c->pending;
        add_day(out, &p);
    }
}

uint64_t lc_protected_down_ms(const lc_t *c)
{
    lc_day_t t;
    lc_today(c, &t);
    uint8_t prot = protected_mask(c);
    uint64_t worst = 0;
    for (size_t s = 0; s < LC_SLOTS; s++) {
        if ((prot & bit(s)) && t.down_ms[s] > worst) {
            worst = t.down_ms[s];
        }
    }
    return worst;
}

void lc_snapshot(const lc_t *c, lc_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->today = c->today;
    out->pending = c->pending;
    out->pending_ms = c->pending_ms;
    if (c->clock != LC_CLOCK_NONE) {
        out->alive_ms = c->unix_ms;
    } else if (c->alive_valid) {
        out->alive_ms = c->alive_ms;
    }
    out->expected_mask = expected_mask(c);
    out->protected_mask = protected_mask(c);
    out->planned = c->planned;
    out->reboot_planned = c->rebooting;
}

bool lc_persist_due(lc_t *c, uint32_t now_ms, uint32_t interval_ms, int64_t *alive_ms, bool *today, bool *hist)
{
    if (c->clock == LC_CLOCK_NONE) {
        /* Today has not grown since the restore: at a reboot it is stored
         * with the mark it runs up to, never without one. */
        if (interval_ms != 0 || !c->alive_valid) {
            return false;
        }
        *alive_ms = c->alive_ms;
    } else {
        if (!c->rolled && c->alive_saved && (uint32_t)(now_ms - c->alive_saved_ms) < interval_ms) {
            return false;
        }
        c->alive_saved = true;
        c->alive_saved_ms = now_ms;
        *alive_ms = c->unix_ms;
        /* The clock went back behind the mark the stored today carries: a
         * restore takes the later mark, so today goes again with this one.
         * Time samples a few ms off the running clock never get here: by
         * the next store the clock is far past the last mark. */
        if (c->today_mark_ms > c->unix_ms) {
            c->today_dirty = true;
        }
    }
    c->rolled = false;
    *today = c->today_dirty;
    *hist = c->hist_dirty;
    if (*today) {
        c->today_mark_ms = *alive_ms;
    }
    c->today_dirty = false;
    c->hist_dirty = false;
    return true;
}

void lc_mark_dirty(lc_t *c, bool today, bool hist)
{
    c->today_dirty |= today;
    c->hist_dirty |= hist;
}

/* ---- NVS blobs -------------------------------------------------------------------------- */

static void put_day(blob_writer_t *w, const lc_day_t *d)
{
    blob_put_u32(w, (uint32_t)d->utc_day);
    for (size_t s = 0; s < LC_SLOTS; s++) {
        blob_put_u32(w, d->down_ms[s]);
        blob_put_u32(w, d->planned_ms[s]);
        blob_put_u32(w, d->reconnects[s]);
    }
    blob_put_u32(w, d->power_off_ms);
    blob_put_u32(w, d->rx_overflows);
}

static void get_day(blob_reader_t *r, lc_day_t *d, size_t slots)
{
    memset(d, 0, sizeof(*d));
    d->utc_day = (int32_t)blob_get_u32(r);
    for (size_t s = 0; s < slots; s++) {
        uint32_t down = blob_get_u32(r);
        uint32_t planned = blob_get_u32(r);
        uint32_t rec = blob_get_u32(r);
        if (s < LC_SLOTS) {
            d->down_ms[s] = down;
            d->planned_ms[s] = planned;
            d->reconnects[s] = rec;
        }
    }
    d->power_off_ms = blob_get_u32(r);
    d->rx_overflows = blob_get_u32(r);
}

static void put_up(blob_writer_t *w, const lc_day_t *d)
{
    for (size_t s = 0; s < LC_SLOTS; s++) {
        blob_put_u32(w, d->up_ms[s]);
    }
}

static void get_up(blob_reader_t *r, lc_day_t *d, size_t slots)
{
    for (size_t s = 0; s < slots; s++) {
        uint32_t up = blob_get_u32(r);
        if (s < LC_SLOTS) {
            d->up_ms[s] = up;
        }
    }
}

size_t lc_encode_today(const lc_snapshot_t *s, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, BLOB_VERSION);
    blob_put_u8(&w, LC_SLOTS);
    put_day(&w, &s->today);
    blob_put_u8(&w, s->expected_mask);
    blob_put_u8(&w, s->protected_mask);
    blob_put_bool(&w, s->planned);
    blob_put_bool(&w, s->reboot_planned);   /* appended later: older blobs end before it */
    blob_put_i64(&w, s->alive_ms);          /* appended later: the mark this today runs up to */
    put_up(&w, &s->today);                  /* appended later: each slot's online time */
    return blob_writer_ok(&w) ? w.len : 0;
}

bool lc_decode_today(const uint8_t *buf, size_t len, lc_snapshot_t *s)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) < BLOB_VERSION) {
        return false;
    }
    size_t slots = blob_get_u8(&r);
    lc_day_t d;
    get_day(&r, &d, slots);
    uint8_t expected = blob_get_u8(&r);
    uint8_t prot = blob_get_u8(&r);
    bool planned = blob_get_bool(&r);
    bool reboot_planned = blob_reader_ok(&r) && blob_reader_remaining(&r) > 0 && blob_get_bool(&r);
    int64_t mark = 0;
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        mark = blob_get_i64(&r);   /* a blob cut inside it is damaged */
    }
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        get_up(&r, &d, slots);     /* likewise */
    }
    if (!blob_reader_ok(&r)) {
        return false;
    }
    s->today = d;
    s->expected_mask = expected;
    s->protected_mask = prot;
    s->planned = planned;
    s->reboot_planned = reboot_planned;
    s->alive_ms = (mark > 0) ? mark : 0;
    return true;
}

void lc_stored_online(lc_snapshot_t *s, int64_t blob_mark_ms, int64_t mark_ms)
{
    if (blob_mark_ms < LC_UTC_FLOOR_S * 1000 || mark_ms <= blob_mark_ms || s->today.utc_day < 0 ||
        day_of(blob_mark_ms) != s->today.utc_day) {
        return;   /* no mark of its own (an older layout), or nothing after it on that day */
    }
    int64_t day_end = ((int64_t)s->today.utc_day + 1) * LC_DAY_MS;
    int64_t end = mark_ms < day_end ? mark_ms : day_end;
    for (size_t i = 0; i < LC_SLOTS; i++) {
        if (s->expected_mask & bit(i)) {
            s->today.up_ms[i] = sat_add(s->today.up_ms[i], (uint64_t)(end - blob_mark_ms));
        }
    }
}

int64_t lc_stored_mark(int64_t blob_mark_ms, int64_t alive_s)
{
    int64_t mark = (blob_mark_ms > 0) ? blob_mark_ms : 0;
    if (alive_s > 0 && alive_s <= INT64_MAX / 1000 && alive_s * 1000 > mark) {
        mark = alive_s * 1000;
    }
    return mark;
}

size_t lc_hist_blob_max(size_t n)
{
    return 3u + n * (4u + LC_SLOTS * 12u + 8u) + n * LC_SLOTS * 4u;
}

size_t lc_encode_hist(const lc_day_t *days, size_t n, uint8_t *buf, size_t cap)
{
    if (n > 255u) {
        n = 255u;
    }
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, BLOB_VERSION);
    blob_put_u8(&w, LC_SLOTS);
    blob_put_u8(&w, (uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        put_day(&w, &days[i]);
    }
    /* Appended later, after every day, so a reader from before stops ahead of it. */
    for (size_t i = 0; i < n; i++) {
        put_up(&w, &days[i]);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

size_t lc_decode_hist(const uint8_t *buf, size_t len, lc_day_t *days, size_t cap)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    if (blob_get_u8(&r) < BLOB_VERSION) {
        return 0;
    }
    size_t slots = blob_get_u8(&r);
    size_t n = blob_get_u8(&r);
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        lc_day_t d;
        get_day(&r, &d, slots);
        if (!blob_reader_ok(&r)) {
            break;
        }
        if (out < cap) {
            days[out++] = d;
        }
    }
    if (blob_reader_ok(&r) && blob_reader_remaining(&r) > 0) {
        for (size_t i = 0; i < n; i++) {
            lc_day_t d;
            memset(&d, 0, sizeof(d));
            get_up(&r, &d, slots);   /* a section cut short is damage */
            if (i < out) {
                memcpy(days[i].up_ms, d.up_ms, sizeof(d.up_ms));
            }
        }
    }
    return blob_reader_ok(&r) ? out : 0;
}

/* ---- RTC copy ---------------------------------------------------------------------------- */

/* CRC-32 (reflected 0xEDB88320), bit by bit: the block is small. */
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void lc_rtc_seal(lc_rtc_t *r, const lc_snapshot_t *snap, uint32_t stamp)
{
    memset(r, 0, sizeof(*r));
    r->magic = LC_RTC_MAGIC;
    r->version = LC_RTC_VERSION;
    r->size = (uint32_t)sizeof(*r);
    r->stamp = stamp;
    r->snap = *snap;
    r->crc = crc32(r, offsetof(lc_rtc_t, crc));
}

bool lc_rtc_valid(const lc_rtc_t *r)
{
    return r->magic == LC_RTC_MAGIC && r->version == LC_RTC_VERSION && r->size == sizeof(*r) &&
           r->crc == crc32(r, offsetof(lc_rtc_t, crc));
}
