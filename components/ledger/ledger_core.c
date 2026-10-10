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
    }
    trim_hist(c);
    return &c->today;
}

/* ---- accrual ------------------------------------------------------------------------- */

static void add_state(lc_t *c, lc_day_t *r, uint64_t ms, uint8_t expected, uint8_t up, bool planned)
{
    uint8_t down = (uint8_t)(expected & ~up);
    if (down == 0) {
        return;
    }
    for (size_t s = 0; s < LC_SLOTS; s++) {
        if (down & bit(s)) {
            r->down_ms[s] = sat_add(r->down_ms[s], ms);
            if (planned) {
                r->planned_ms[s] = sat_add(r->planned_ms[s], ms);
            }
        }
    }
    mark(c, r);
}

/* Spread [start, start + ms) over UTC days with a fixed state. */
static void accrue_span(lc_t *c, int64_t start, uint64_t ms, uint8_t expected, uint8_t up, bool planned,
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

static void accrue(lc_t *c, uint64_t ms)
{
    if (ms == 0) {
        return;
    }
    if (c->clock != LC_CLOCK_NONE) {
        accrue_span(c, c->unix_ms, ms, expected_mask(c), up_mask(c), c->planned, false);
        c->unix_ms += (int64_t)ms;
        (void)rec_for_day(c, day_of(c->unix_ms));   /* at exactly midnight, today is the new day */
    } else {
        add_state(c, &c->pending, ms, expected_mask(c), up_mask(c), c->planned);
        c->pending_ms += ms;
        c->pending_boot_ms += ms;
    }
    c->uptime_ms += ms;
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
            accrue_span(c, start, (uint64_t)gap, c->prev_expected_mask, 0, c->prev_planned, c->gap_power_off);
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

void lc_restore(lc_t *c, const lc_snapshot_t *snap, bool power_cut)
{
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
    c->prev_planned = snap->planned;
    c->planned = false;
    c->alive_valid = snap->alive_ms >= LC_UTC_FLOOR_S * 1000;
    c->alive_ms = snap->alive_ms;
    c->gap_power_off = power_cut;
    trim_hist(c);
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
}

bool lc_persist_due(lc_t *c, uint32_t now_ms, uint32_t interval_ms, int64_t *alive_ms, bool *today, bool *hist)
{
    if (c->clock == LC_CLOCK_NONE || (c->alive_saved && (uint32_t)(now_ms - c->alive_saved_ms) < interval_ms)) {
        return false;
    }
    c->alive_saved = true;
    c->alive_saved_ms = now_ms;
    *alive_ms = c->unix_ms;
    *today = c->today_dirty;
    *hist = c->hist_dirty;
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
    if (!blob_reader_ok(&r)) {
        return false;
    }
    s->today = d;
    s->expected_mask = expected;
    s->protected_mask = prot;
    s->planned = planned;
    return true;
}

size_t lc_hist_blob_max(size_t n)
{
    return 3u + n * (4u + LC_SLOTS * 12u + 8u);
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

void lc_rtc_seal(lc_rtc_t *r, const lc_snapshot_t *snap)
{
    memset(r, 0, sizeof(*r));
    r->magic = LC_RTC_MAGIC;
    r->version = LC_RTC_VERSION;
    r->size = (uint32_t)sizeof(*r);
    r->snap = *snap;
    r->crc = crc32(r, offsetof(lc_rtc_t, crc));
}

bool lc_rtc_valid(const lc_rtc_t *r)
{
    return r->magic == LC_RTC_MAGIC && r->version == LC_RTC_VERSION && r->size == sizeof(*r) &&
           r->crc == crc32(r, offsetof(lc_rtc_t, crc));
}
