/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "rtcm_hub_core.h"

#include <string.h>

/* Record header as stored in the ring (native byte order, 4-aligned). */
typedef struct {
    uint16_t len;
    uint16_t msg_type;
    uint32_t seq;
    uint32_t rx_ms;
    uint32_t epoch;
} rec_hdr_t;

_Static_assert(sizeof(rec_hdr_t) == RTCM_CORE_REC_HDR, "record header layout");
_Static_assert(GNSS_RTCM3_MAX_FRAME < RTCM_CORE_WRAP_MARKER, "a frame length never looks like a wrap marker");

/* Producer state as one consistent view (taken under the lock). */
typedef struct {
    uint64_t w;          /* write_off */
    uint64_t r;          /* reserve_off */
    uint64_t frames;     /* frames_in */
    uint64_t bytes;      /* bytes_in */
    uint64_t last_rx;    /* arrival of the newest frame (64-bit ms) */
    uint32_t epoch;      /* epoch of the newest committed record */
    bool have_epoch;
    bool have_frame;
    bool held;           /* a hold is set ... */
    uint64_t hold_off;   /* ... at this offset */
    uint32_t release_gen;
} snap_t;

/* What a consumer does with one record. */
typedef enum {
    ACT_DELIVER_START,   /* first frame of an epoch: deliver */
    ACT_DELIVER,         /* next frame of the epoch being delivered */
    ACT_SKIP,            /* waiting for an epoch start: skip */
    ACT_SKIP_EPOCH,      /* waiting: an epoch start already over the lag limit, skip that epoch too */
    ACT_DROP,            /* rest of an epoch dropped by the lag limit */
    ACT_DROP_EPOCH,      /* epoch start over the lag limit: drop the whole epoch */
    ACT_SKIP_UNSURE,     /* waiting: an unsure epoch, skip all of it */
    ACT_STOP_UNSURE,     /* streaming: a new unsure epoch, back to waiting */
} act_t;

typedef enum { RUN_DONE, RUN_FULL, RUN_LAPPED, RUN_HELD, RUN_RELEASED } run_t;

/* A read starts over after a lap (sticky replay, then the next epoch start);
 * the bound only matters if the producer laps the consumer again and again
 * within one call. */
#define MAX_LAPS_PER_READ 4
#define STICKY_TRIES      3

static inline void lk(rtcm_core_t *c)
{
    if (c->lock.lock != NULL) {
        c->lock.lock(c->lock.ctx);
    }
}

static inline void unlk(rtcm_core_t *c)
{
    if (c->lock.unlock != NULL) {
        c->lock.unlock(c->lock.ctx);
    }
}

static inline size_t rec_len(size_t frame_len)
{
    return RTCM_CORE_REC_HDR + ((frame_len + 3u) & ~(size_t)3u);
}

static inline size_t ring_pos(const rtcm_core_t *c, uint64_t off)
{
    return (size_t)(off % c->cap);
}

static void snapshot_locked(const rtcm_core_t *c, snap_t *s)
{
    s->w = c->write_off;
    s->r = c->reserve_off;
    s->frames = c->frames_in;
    s->bytes = c->bytes_in;
    s->last_rx = c->last_rx_ms;
    s->epoch = c->committed_epoch;
    s->have_epoch = c->have_epoch;
    s->have_frame = c->have_frame;
    s->held = c->held;
    s->hold_off = c->hold_off;
    s->release_gen = c->release_gen;
}

static void snapshot(rtcm_core_t *c, snap_t *s)
{
    lk(c);
    snapshot_locked(c, s);
    unlk(c);
}

/* True while the byte at `off` (and everything after it) cannot have been
 * overwritten: the producer has not reserved past off + capacity. */
static bool intact(rtcm_core_t *c, uint64_t off)
{
    lk(c);
    uint64_t r = c->reserve_off;
    unlk(c);
    return r - off <= c->cap;
}

/* Age of a record, clamped at 0 (the reader's clock may lag the producer's
 * by a moment). If even the newest frame is older than 2^31 ms, rx_ms values
 * may have wrapped and every record counts as stale. */
static uint32_t record_age(const snap_t *s, uint32_t rx_ms, uint64_t now_ms)
{
    if (s->have_frame && now_ms > s->last_rx && now_ms - s->last_rx > (uint64_t)INT32_MAX) {
        return UINT32_MAX;
    }
    int32_t age = (int32_t)((uint32_t)now_ms - rx_ms);
    return age > 0 ? (uint32_t)age : 0;
}

int rtcm_core_sticky_slot(uint16_t msg_type)
{
    switch (msg_type) {
    case 1005:
    case 1006:
        return 0;
    case 1033:
        return 1;
    case 1230:
        return 2;
    default:
        return -1;
    }
}

bool rtcm_core_init(rtcm_core_t *c, uint8_t *ring, size_t ring_len, const rtcm_core_cfg_t *cfg,
                    const rtcm_core_lock_t *lock)
{
    if (c == NULL || ring == NULL || cfg == NULL) {
        return false;
    }
    size_t cap = ring_len & ~(size_t)3u;
    if (cap < RTCM_CORE_MIN_RING || cfg->max_consumers < 1 || cfg->max_consumers > RTCM_CORE_MAX_CONSUMERS ||
        cfg->default_lag_ms < RTCM_CORE_LAG_MIN_MS || cfg->default_lag_ms > RTCM_CORE_LAG_MAX_MS) {
        return false;
    }
    memset(c, 0, sizeof(*c));
    if (lock != NULL) {
        c->lock = *lock;
    }
    c->ring = ring;
    c->cap = cap;
    c->default_lag_ms = cfg->default_lag_ms;
    c->max_consumers = cfg->max_consumers;
    rtcm_epoch_init(&c->ep, cfg->gap_ms, cfg->first_epoch);
    return true;
}

/* ---- producer ------------------------------------------------------------ */

bool rtcm_core_publish(rtcm_core_t *c, const uint8_t *frame, size_t len, uint16_t msg_type, uint64_t arrival_ms,
                       bool timed, void **wake, size_t *n_wake)
{
    *n_wake = 0;
    if (frame == NULL || len < RTCM_CORE_MIN_FRAME || len > GNSS_RTCM3_MAX_FRAME) {
        lk(c);
        c->publish_failures++;
        unlk(c);
        return false;
    }

    /* write_off and frames_in change only here, so the producer reads them
     * without the lock. */
    const uint64_t w = c->write_off;
    const size_t rlen = rec_len(len);
    size_t pos = ring_pos(c, w);
    const size_t waste = (c->cap - pos < rlen) ? c->cap - pos : 0;
    const uint32_t rx = (uint32_t)arrival_ms;

    lk(c);
    bool unsure = false;
    rtcm_epoch_start_t start = RTCM_EPOCH_CONTINUES;
    uint32_t epoch = rtcm_epoch_assign(&c->ep, frame, len, msg_type, rx, timed, &start, &unsure);
    if (start != RTCM_EPOCH_CONTINUES) {
        c->epoch_start_off = w;   /* a hold here keeps the wrap marker and the record out */
    }
    c->reserve_off = w + waste + rlen;
    unlk(c);

    if (waste != 0) {
        const uint16_t marker = RTCM_CORE_WRAP_MARKER;
        memcpy(c->ring + pos, &marker, sizeof(marker));
        pos = 0;
    }
    const rec_hdr_t h = {
        .len = (uint16_t)len,
        .msg_type = (uint16_t)((msg_type & RTCM_CORE_REC_TYPE_MASK) | (unsure ? RTCM_CORE_REC_UNSURE : 0u)),
        .seq = (uint32_t)c->frames_in,
        .rx_ms = rx,
        .epoch = epoch,
    };
    memcpy(c->ring + pos, &h, sizeof(h));
    memcpy(c->ring + pos + RTCM_CORE_REC_HDR, frame, len);

    /* Sticky copy: fill the inactive buffer now, flip it at commit. */
    int slot = rtcm_core_sticky_slot(msg_type);
    uint8_t next = 0;
    if (slot >= 0) {
        next = (uint8_t)(c->sticky[slot].active ^ 1u);
        memcpy(c->sticky[slot].buf[next], frame, len);
    }

    size_t n = 0;
    lk(c);
    c->write_off = c->reserve_off;
    c->frames_in++;
    c->bytes_in += len;
    c->last_rx_ms = arrival_ms;
    c->have_frame = true;
    c->committed_epoch = epoch;
    c->have_epoch = true;
    if (slot >= 0) {
        rtcm_core_sticky_t *s = &c->sticky[slot];
        s->len[next] = (uint16_t)len;
        s->rx_ms[next] = arrival_ms;
        s->active = next;
        s->valid = true;
        s->gen++;
    }
    for (unsigned i = 0; i < c->max_consumers; i++) {
        rtcm_core_consumer_t *k = &c->cons[i];
        if (k->in_use && k->waiting) {
            k->waiting = false;
            if (k->waiter != NULL) {
                wake[n++] = k->waiter;
            }
        }
    }
    if (n > 0) {
        c->waking = true;
    }
    unlk(c);
    *n_wake = n;
    return true;
}

void rtcm_core_wake_done(rtcm_core_t *c)
{
    lk(c);
    c->waking = false;
    c->wake_gen++;
    unlk(c);
}

void rtcm_core_clear_sticky(rtcm_core_t *c)
{
    lk(c);
    for (unsigned i = 0; i < RTCM_CORE_STICKY_SLOTS; i++) {
        c->sticky[i].valid = false;
        c->sticky[i].gen++;
    }
    rtcm_epoch_force_new(&c->ep);
    unlk(c);
}

/* ---- consumers: positioning ---------------------------------------------- */

/* Start over as on attach: sticky copies, then the first epoch after the
 * newest committed one. `loss`: frames skipped while waiting are losses (a
 * lap), not idle skips. */
static void reposition(rtcm_core_consumer_t *k, const snap_t *s, bool loss)
{
    k->rd = s->w;
    k->frames_pos = s->frames;
    k->bytes_pos = s->bytes;
    k->last_rx_ms = (uint32_t)s->last_rx;
    k->streaming = false;
    k->wait_any = !s->have_epoch;
    k->wait_after = s->epoch;
    k->wait_loss = loss;
    k->last_epoch = 0;
    k->epoch_open = false;
    k->dropping = false;
    k->drop_epoch = 0;
    k->drop_run = false;
    k->sticky_next = 0;
    k->release_gen = s->release_gen;
}

static void publish_counters(rtcm_core_t *c, rtcm_core_consumer_t *k)
{
    lk(c);
    k->pub = k->cnt;
    k->pub_rd = k->rd;
    k->pub_last_rx_ms = k->last_rx_ms;
    unlk(c);
}

rtcm_core_consumer_t *rtcm_core_attach(rtcm_core_t *c, const char *name)
{
    rtcm_core_consumer_t *found = NULL;
    lk(c);
    for (unsigned i = 0; i < c->max_consumers; i++) {
        rtcm_core_consumer_t *k = &c->cons[i];
        if (k->in_use) {
            continue;
        }
        memset(k, 0, sizeof(*k));
        k->in_use = true;
        if (name != NULL) {
            strncpy(k->name, name, sizeof(k->name) - 1);
        }
        k->max_lag_ms = c->default_lag_ms;
        snap_t s;
        snapshot_locked(c, &s);
        reposition(k, &s, false);
        k->pub_rd = k->rd;
        k->pub_last_rx_ms = k->last_rx_ms;
        c->attached++;
        found = k;
        break;
    }
    unlk(c);
    return found;
}

bool rtcm_core_detach(rtcm_core_t *c, rtcm_core_consumer_t *k, uint32_t *gen)
{
    lk(c);
    if (k->in_use) {
        k->in_use = false;
        k->waiting = false;
        k->waiter = NULL;
        c->attached--;
    }
    bool busy = c->waking;
    *gen = c->wake_gen;
    unlk(c);
    return busy;
}

bool rtcm_core_wake_passed(rtcm_core_t *c, uint32_t gen)
{
    lk(c);
    bool passed = !c->waking || c->wake_gen != gen;
    unlk(c);
    return passed;
}

void rtcm_core_resync(rtcm_core_t *c, rtcm_core_consumer_t *k)
{
    snap_t s;
    snapshot(c, &s);
    k->cnt.frames_skipped_idle += s.frames - k->frames_pos;
    reposition(k, &s, false);
    k->finishing = false;
    k->finished = false;
    publish_counters(c, k);
}

bool rtcm_core_set_max_lag(rtcm_core_t *c, rtcm_core_consumer_t *k, uint32_t ms)
{
    if (ms < RTCM_CORE_LAG_MIN_MS || ms > RTCM_CORE_LAG_MAX_MS) {
        return false;
    }
    lk(c);
    k->max_lag_ms = ms;
    unlk(c);
    return true;
}

bool rtcm_core_prepare_wait(rtcm_core_t *c, rtcm_core_consumer_t *k, void *waiter)
{
    lk(c);
    /* While held, pending sticky copies wait for the release: nothing to read. */
    bool idle = (c->held && k->rd >= c->hold_off) ||
                (k->rd == c->write_off && (c->held || k->sticky_next >= RTCM_CORE_STICKY_SLOTS));
    if (idle) {
        k->waiter = waiter;
        k->waiting = true;
    }
    unlk(c);
    return idle;
}

void rtcm_core_cancel_wait(rtcm_core_t *c, rtcm_core_consumer_t *k)
{
    lk(c);
    k->waiting = false;
    unlk(c);
}

/* ---- consumers: reading -------------------------------------------------- */

/* Everything from the cursor up to s->w was overwritten or is about to be:
 * count it and start over as on attach. */
static void lapped(rtcm_core_consumer_t *k, const snap_t *s)
{
    uint64_t frames = s->frames - k->frames_pos;
    uint64_t bytes = s->bytes - k->bytes_pos;
    bool loss = k->streaming || k->wait_loss;
    if (loss) {
        k->cnt.frames_dropped += frames;
        k->cnt.bytes_dropped += bytes;
        k->cnt.laps++;
        /* Epochs never delivered whole and not counted yet: after the one
         * being dropped by the lag limit, or last_epoch+1 .. s->epoch (the
         * newest one's rest is skipped while waiting), or the current one if
         * it is still the newest. Whether last_epoch itself was complete is
         * unknown; it counts neither way. */
        if (s->have_epoch) {
            if (k->streaming && k->dropping) {
                k->cnt.epochs_dropped += (uint32_t)(s->epoch - k->drop_epoch);
            } else if (k->streaming) {
                uint32_t d = s->epoch - k->last_epoch;
                k->cnt.epochs_dropped += (d == 0) ? 1u : d;
            } else if (!k->wait_any) {
                k->cnt.epochs_dropped += (uint32_t)(s->epoch - k->wait_after);
            }
        }
    } else {
        k->cnt.frames_skipped_idle += frames;
    }
    /* A consumer still waiting for its first epoch start keeps its sticky
     * replay state: copies already sent are not sent twice. */
    uint8_t sticky_next = k->sticky_next;
    bool was_streaming = k->streaming;
    reposition(k, s, loss);
    if (!was_streaming) {
        k->sticky_next = sticky_next;
    }
}

static void lapped_now(rtcm_core_t *c, rtcm_core_consumer_t *k, bool corrupt)
{
    snap_t s;
    lk(c);
    if (corrupt) {
        c->corrupt++;
    }
    snapshot_locked(c, &s);
    unlk(c);
    lapped(k, &s);
}

/* `unsure`: the record belongs to an unsure epoch (its head may have been
 * lost at a framer reset), so it is never an epoch start for a consumer. */
static act_t decide(const rtcm_core_consumer_t *k, uint32_t epoch, uint32_t age, bool unsure)
{
    bool old = age > k->max_lag_ms;
    if (!k->streaming) {
        if (k->wait_any || rtcm_epoch_after(epoch, k->wait_after)) {
            if (unsure) {
                return ACT_SKIP_UNSURE;
            }
            return old ? ACT_SKIP_EPOCH : ACT_DELIVER_START;
        }
        return ACT_SKIP;
    }
    if (k->dropping && epoch == k->drop_epoch) {
        return ACT_DROP;
    }
    if (epoch == k->last_epoch) {
        return ACT_DELIVER;   /* finish an epoch already partly delivered */
    }
    if (unsure) {
        return ACT_STOP_UNSURE;
    }
    return old ? ACT_DROP_EPOCH : ACT_DELIVER_START;
}

static void count_skip(rtcm_core_consumer_t *k, uint16_t len)
{
    if (k->wait_loss) {
        k->cnt.frames_dropped++;
        k->cnt.bytes_dropped += len;
    } else {
        k->cnt.frames_skipped_idle++;
    }
}

static void apply(rtcm_core_consumer_t *k, act_t a, uint32_t epoch, uint16_t len)
{
    rtcm_core_counters_t *n = &k->cnt;
    switch (a) {
    case ACT_DELIVER_START:
        if (k->streaming && k->epoch_open) {
            n->epochs_out++;   /* the previous epoch ended here, complete */
        }
        k->streaming = true;
        k->wait_any = false;
        k->wait_loss = false;
        k->last_epoch = epoch;
        k->epoch_open = true;
        k->dropping = false;
        k->drop_run = false;
        n->frames_out++;
        n->bytes_out += len;
        break;
    case ACT_DELIVER:
        n->frames_out++;
        n->bytes_out += len;
        break;
    case ACT_SKIP:
        count_skip(k, len);
        break;
    case ACT_SKIP_EPOCH:
        k->wait_after = epoch;
        k->wait_any = false;
        if (k->wait_loss) {
            n->epochs_dropped++;
        }
        count_skip(k, len);
        break;
    case ACT_DROP_EPOCH:
        if (k->epoch_open) {
            n->epochs_out++;
            k->epoch_open = false;
        }
        if (!k->drop_run) {
            n->lag_drops++;
            k->drop_run = true;
        }
        n->epochs_dropped++;
        k->dropping = true;
        k->drop_epoch = epoch;
        n->frames_dropped++;
        n->bytes_dropped += len;
        break;
    case ACT_DROP:
        n->frames_dropped++;
        n->bytes_dropped += len;
        break;
    case ACT_SKIP_UNSURE:
        k->wait_after = epoch;
        k->wait_any = false;
        count_skip(k, len);
        break;
    case ACT_STOP_UNSURE:
        if (k->epoch_open) {
            n->epochs_out++;   /* the epoch being sent ended here, complete */
        }
        k->streaming = false;
        k->epoch_open = false;
        k->wait_any = false;
        k->wait_loss = false;
        k->wait_after = epoch;
        k->dropping = false;
        k->drop_run = false;
        n->frames_skipped_idle++;
        break;
    }
}

static run_t read_records(rtcm_core_t *c, rtcm_core_consumer_t *k, uint8_t *buf, size_t max_len, size_t *out,
                          uint64_t now_ms)
{
    snap_t s;
    snapshot(c, &s);
    for (;;) {
        if (k->finished) {
            return RUN_HELD;   /* finishing, and its epoch is over */
        }
        if (s.release_gen != k->release_gen) {
            return RUN_RELEASED;   /* start over first (rtcm_core_read) */
        }
        if (s.held && k->rd >= s.hold_off) {
            return RUN_HELD;   /* everything before the hold point is passed */
        }
        if (k->rd == s.w) {
            snapshot(c, &s);   /* more may have been committed meanwhile (or a hold set or released) */
            if (s.release_gen != k->release_gen) {
                return RUN_RELEASED;
            }
            if (s.held && k->rd >= s.hold_off) {
                return RUN_HELD;
            }
            if (k->rd == s.w) {
                return RUN_DONE;
            }
        }
        if (k->rd > s.w || s.r - k->rd > c->cap) {
            lapped(k, &s);
            return RUN_LAPPED;
        }

        const size_t pos = ring_pos(c, k->rd);
        uint16_t first;
        memcpy(&first, c->ring + pos, sizeof(first));
        if (first == RTCM_CORE_WRAP_MARKER) {
            if (!intact(c, k->rd)) {
                lapped_now(c, k, false);
                return RUN_LAPPED;
            }
            if (pos == 0) {
                lapped_now(c, k, true);
                return RUN_LAPPED;
            }
            k->rd += c->cap - pos;
            continue;
        }

        rec_hdr_t h;
        const bool room = c->cap - pos >= RTCM_CORE_REC_HDR;
        if (room) {
            memcpy(&h, c->ring + pos, sizeof(h));
        }
        if (!intact(c, k->rd)) {
            lapped_now(c, k, false);
            return RUN_LAPPED;
        }
        /* Intact, so these checks cannot fail unless the ring is corrupt;
         * they keep a bad header from ever reaching memcpy. */
        if (!room || h.len < RTCM_CORE_MIN_FRAME || h.len > GNSS_RTCM3_MAX_FRAME ||
            rec_len(h.len) > c->cap - pos || h.seq != (uint32_t)k->frames_pos ||
            (h.msg_type & (uint16_t)~(RTCM_CORE_REC_TYPE_MASK | RTCM_CORE_REC_UNSURE)) != 0) {
            lapped_now(c, k, true);
            return RUN_LAPPED;
        }

        act_t a = decide(k, h.epoch, record_age(&s, h.rx_ms, now_ms), (h.msg_type & RTCM_CORE_REC_UNSURE) != 0);
        if (k->finishing && a != ACT_DELIVER) {
            k->finished = true;   /* not the epoch being delivered: it is over (or none was) */
            return RUN_HELD;
        }
        if (a == ACT_DELIVER || a == ACT_DELIVER_START) {
            if (max_len - *out < h.len) {
                return RUN_FULL;
            }
            memcpy(buf + *out, c->ring + pos + RTCM_CORE_REC_HDR, h.len);
            if (!intact(c, k->rd)) {
                lapped_now(c, k, false);   /* the copy may be torn: not delivered */
                return RUN_LAPPED;
            }
            *out += h.len;
        }
        apply(k, a, h.epoch, h.len);
        k->rd += rec_len(h.len);
        k->frames_pos++;
        k->bytes_pos += h.len;
        k->last_rx_ms = h.rx_ms;
    }
}

/* Replay the sticky copies still pending. @return false if the buffer filled
 * up first (the rest goes out with the next read). */
static bool replay_sticky(rtcm_core_t *c, rtcm_core_consumer_t *k, uint8_t *buf, size_t max_len, size_t *out,
                          uint64_t now_ms)
{
    while (k->sticky_next < RTCM_CORE_STICKY_SLOTS) {
        rtcm_core_sticky_t *st = &c->sticky[k->sticky_next];
        for (int tries = 0; tries < STICKY_TRIES; tries++) {
            lk(c);
            bool valid = st->valid;
            uint8_t a = st->active;
            size_t len = st->len[a];
            uint64_t rx = st->rx_ms[a];
            uint32_t gen = st->gen;
            unlk(c);
            if (!valid || (now_ms > rx && now_ms - rx > RTCM_CORE_STICKY_MAX_AGE_MS)) {
                break;
            }
            if (max_len - *out < len) {
                return false;
            }
            memcpy(buf + *out, st->buf[a], len);
            lk(c);
            bool same = st->gen == gen;
            unlk(c);
            if (same) {
                *out += len;
                k->cnt.frames_out++;
                k->cnt.bytes_out += len;
                k->cnt.sticky_out++;
                break;
            }
        }
        k->sticky_next++;
    }
    return true;
}

size_t rtcm_core_read(rtcm_core_t *c, rtcm_core_consumer_t *k, uint8_t *buf, size_t max_len, uint64_t now_ms)
{
    size_t out = 0;
    if (max_len < GNSS_RTCM3_MAX_FRAME) {
        return 0;
    }
    for (int laps = 0; laps < MAX_LAPS_PER_READ; laps++) {
        snap_t s;
        snapshot(c, &s);
        if (s.release_gen != k->release_gen) {
            if (out > 0) {
                break;   /* what was read before the release goes out first; start over next time */
            }
            /* A hold was released since this position was taken: what came
             * from the hold point on belongs to the pause (the receiver work
             * cut its epoch). Start over as on a resync. */
            k->cnt.frames_skipped_idle += s.frames - k->frames_pos;
            reposition(k, &s, false);
            k->restarted = true;
        }
        /* While held no sticky copy goes out (they may be newer than the
         * hold point); the release starts every consumer over anyway. Nor
         * while finishing: sticky copies open an epoch. */
        if (!s.held && !k->finishing && !replay_sticky(c, k, buf, max_len, &out, now_ms)) {
            break;
        }
        run_t r = read_records(c, k, buf, max_len, &out, now_ms);
        if (r != RUN_LAPPED && r != RUN_RELEASED) {
            break;
        }
    }
    publish_counters(c, k);
    return out;
}

/* Under the lock: the newest epoch needs nothing more, as far as the frames
 * stored so far show (an epoch whose last MSM is in, one with no MSM yet, or
 * none at all; the next frame starts an epoch anyway after a reset). */
static bool newest_closed_locked(const rtcm_core_t *c)
{
    const rtcm_epoch_t *e = &c->ep;
    return !e->started || e->force_new || e->terminated || e->time_mask == 0;
}

/* Under the lock, finishing: the epoch this consumer was delivering is over. */
static bool finish_done_locked(const rtcm_core_t *c, const rtcm_core_consumer_t *k)
{
    if (k->finished || !k->streaming || !k->epoch_open) {
        return true;   /* not inside an epoch (or the read already met the next one) */
    }
    if (k->rd != c->write_off || c->reserve_off != c->write_off) {
        return false;  /* records to read, or one being stored (its epoch state is already in c->ep) */
    }
    /* Every record read: the newest epoch is the one delivered, or it ended
     * before the next one was skipped or dropped. */
    return c->committed_epoch != k->last_epoch || newest_closed_locked(c);
}

bool rtcm_core_at_hold(rtcm_core_t *c, rtcm_core_consumer_t *k)
{
    lk(c);
    bool at = c->held && k->rd >= c->hold_off;
    if (!at && k->finishing && finish_done_locked(c, k)) {
        k->finished = true;
        at = true;
    }
    unlk(c);
    return at;
}

void rtcm_core_finish(rtcm_core_t *c, rtcm_core_consumer_t *k)
{
    (void)c;
    k->finishing = true;
}

bool rtcm_core_take_restart(rtcm_core_consumer_t *k)
{
    bool r = k->restarted;
    k->restarted = false;
    return r;
}

/* ---- stats --------------------------------------------------------------- */

void rtcm_core_get_stats(rtcm_core_t *c, uint64_t now_ms, rtcm_core_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    lk(c);
    out->frames_in = c->frames_in;
    out->bytes_in = c->bytes_in;
    out->epochs_in = c->ep.epochs;
    out->epochs_by_mmb = c->ep.by_mmb;
    out->epochs_by_time = c->ep.by_time;
    out->epochs_by_gap = c->ep.by_gap;
    out->publish_failures = c->publish_failures;
    out->consumers = c->attached;
    bool have = c->have_frame;
    uint64_t last = c->last_rx_ms;
    unlk(c);
    out->capacity = c->cap;
    if (!have) {
        out->last_frame_age_ms = UINT32_MAX;
    } else if (now_ms <= last) {
        out->last_frame_age_ms = 0;
    } else {
        uint64_t age = now_ms - last;
        out->last_frame_age_ms = age >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age;
    }
}

bool rtcm_core_epoch_in_flight(rtcm_core_t *c, uint64_t now_ms, uint32_t quiet_ms, uint32_t *age_ms)
{
    lk(c);
    bool have = c->have_frame;
    uint64_t last = c->last_rx_ms;
    bool open = c->ep.msm_open;
    unlk(c);
    uint32_t age = UINT32_MAX;
    if (have) {
        uint64_t d = now_ms > last ? now_ms - last : 0;
        age = d >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)d;
    }
    if (age_ms != NULL) {
        *age_ms = age;
    }
    return have && open && age < quiet_ms;
}

rtcm_core_hold_t rtcm_core_hold(rtcm_core_t *c, uint64_t now_ms, uint32_t quiet_ms, bool force, bool arriving)
{
    lk(c);
    if (c->held) {
        rtcm_core_hold_t kind = c->hold_kind;
        unlk(c);
        return kind;
    }
    const rtcm_epoch_t *e = &c->ep;
    uint64_t at;
    rtcm_core_hold_t kind = RTCM_CORE_HOLD_BOUNDARY;
    if (!e->started || e->force_new || e->terminated) {
        at = c->reserve_off;          /* the next frame starts an epoch */
    } else if (e->time_mask == 0) {
        at = c->epoch_start_off;      /* only the newest epoch's leading station messages so far */
    } else if (!force) {
        /* An MSM announced more: the rest may still come, however quiet the
         * line is now (a slow reader, a stalled line). */
        unlk(c);
        return RTCM_CORE_HOLD_NONE;
    } else {
        /* The caller waited long enough. Quiet for the gap rule, with nothing
         * received and unpublished: its last MSM never came (or the receiver
         * never clears the bit), and what came is the epoch. Otherwise it is
         * still arriving: nobody starts it. The newest frame's arrival is the
         * epoch tracker's (a frame being copied right now counts; last_rx_ms
         * moves only at commit). */
        int32_t age = (int32_t)((uint32_t)now_ms - e->last_rx_ms);
        if (!arriving && age >= 0 && (uint32_t)age >= quiet_ms) {
            at = c->reserve_off;
            kind = RTCM_CORE_HOLD_QUIET;
        } else {
            at = c->epoch_start_off;
            kind = RTCM_CORE_HOLD_CUT;
        }
    }
    c->held = true;
    c->hold_off = at;
    c->hold_kind = kind;
    unlk(c);
    return kind;
}

void rtcm_core_release(rtcm_core_t *c)
{
    lk(c);
    if (c->held) {
        c->held = false;
        c->release_gen++;
    }
    unlk(c);
}

bool rtcm_core_held(rtcm_core_t *c)
{
    lk(c);
    bool held = c->held;
    unlk(c);
    return held;
}

/* rx_ms of the record at `off` (skipping a wrap marker), if still intact. */
static bool peek_rx(rtcm_core_t *c, uint64_t off, const snap_t *s, uint32_t *rx)
{
    if (off >= s->w || s->r - off > c->cap) {
        return false;
    }
    size_t pos = ring_pos(c, off);
    uint16_t first;
    memcpy(&first, c->ring + pos, sizeof(first));
    if (first == RTCM_CORE_WRAP_MARKER) {
        pos = 0;
    } else if (c->cap - pos < RTCM_CORE_REC_HDR) {
        return false;
    }
    rec_hdr_t h;
    memcpy(&h, c->ring + pos, sizeof(h));
    if (!intact(c, off)) {
        return false;
    }
    *rx = h.rx_ms;
    return true;
}

void rtcm_core_get_consumer_stats(rtcm_core_t *c, rtcm_core_consumer_t *k, uint64_t now_ms,
                                  rtcm_core_consumer_stats_t *out)
{
    snap_t s;
    lk(c);
    out->c = k->pub;
    out->max_lag_ms = k->max_lag_ms;
    uint64_t rd = k->pub_rd;
    uint32_t last_rx = k->pub_last_rx_ms;
    snapshot_locked(c, &s);
    unlk(c);

    out->lag_ms = 0;
    if (rd < s.w) {
        uint32_t rx;
        if (!peek_rx(c, rd, &s, &rx)) {
            rx = last_rx;   /* overwritten: the oldest unread frame came right after the last one passed */
        }
        out->lag_ms = record_age(&s, rx, now_ms);
    }
}
