/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Health monitor helpers: pure logic, see health_mon.h.
 * PROVISIONAL, to be confirmed on hardware.
 */
#include "health_mon.h"

#include <string.h>

static uint32_t sat_add32(uint32_t a, uint64_t b)
{
    uint64_t s = (uint64_t)a + b;
    return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

/* ---- loss journal ---------------------------------------------------------- */

void hm_loss_init(hm_loss_t *l)
{
    memset(l, 0, sizeof(*l));
    for (unsigned i = 0; i < HM_LOSS_IDS; i++) {
        l->since_ms[i] = HM_NEVER;
        l->written_ms[i] = HM_NEVER;
    }
}

void hm_loss_sample(hm_loss_t *l, const uint64_t vals[HM_LOSS_IDS], int64_t now_ms, uint64_t inc[HM_LOSS_IDS])
{
    for (unsigned i = 0; i < HM_LOSS_IDS; i++) {
        uint64_t d = 0;
        if (l->have_base && vals[i] > l->last[i]) {
            d = vals[i] - l->last[i];
        }
        l->last[i] = vals[i];
        if (d != 0) {
            if (l->pending[i] == 0) {
                l->since_ms[i] = now_ms;
            }
            l->pending[i] = (UINT64_MAX - l->pending[i] < d) ? UINT64_MAX : l->pending[i] + d;
        }
        if (inc != NULL) {
            inc[i] = d;
        }
    }
    l->have_base = true;
}

bool hm_loss_next(const hm_loss_t *l, int64_t now_ms, uint8_t *id, int32_t *delta)
{
    int best = -1;
    for (unsigned i = 0; i < HM_LOSS_IDS; i++) {
        if (l->pending[i] == 0) {
            continue;
        }
        if (l->written_ms[i] != HM_NEVER && now_ms - l->written_ms[i] < HM_LOSS_MIN_INTERVAL_MS) {
            continue;   /* this counter's last record is too recent: the increase merges meanwhile */
        }
        if (best < 0 || l->since_ms[i] < l->since_ms[best]) {
            best = (int)i;
        }
    }
    if (best < 0) {
        return false;
    }
    uint64_t p = l->pending[best];
    *id = (uint8_t)(best + 1);
    *delta = p > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)p;
    return true;
}

void hm_loss_written(hm_loss_t *l, uint8_t id, int32_t delta, int64_t now_ms)
{
    if (id == 0 || id > HM_LOSS_IDS || delta <= 0) {
        return;
    }
    unsigned i = (unsigned)id - 1u;
    l->written_ms[i] = now_ms;
    uint64_t d = (uint64_t)delta;
    l->pending[i] = l->pending[i] > d ? l->pending[i] - d : 0;
    if (l->pending[i] == 0) {
        l->since_ms[i] = HM_NEVER;
    }
}

/* ---- low-heap reboot ------------------------------------------------------------ */

void hm_heap_init(hm_heap_t *h, bool prev_low_heap, uint32_t prev_uptime_s)
{
    memset(h, 0, sizeof(*h));
    h->held = prev_low_heap && prev_uptime_s < HM_HEAP_REBOOT_SPACING_S;
}

bool hm_heap_reboot_due(hm_heap_t *h, uint32_t low_water, uint32_t limit)
{
    if (!h->have_base) {
        h->have_base = true;
        h->base_min = low_water;   /* the start-up peak: a reboot would only repeat it */
        return false;
    }
    if (h->done || h->held || low_water >= limit || low_water >= h->base_min) {
        return false;
    }
    h->done = true;
    return true;
}

/* ---- link-loss alert ----------------------------------------------------------- */

void hm_link_init(hm_link_t *w, int64_t now_ms)
{
    memset(w, 0, sizeof(*w));
    w->bucket_start = now_ms;
    w->last_cond_ms = HM_NEVER;
}

static void link_rotate(hm_link_t *w, int64_t now_ms)
{
    int rounds = 0;
    while (now_ms - w->bucket_start >= HM_LINK_BUCKET_MS) {
        if (rounds++ > HM_LINK_BUCKETS) {
            /* A long gap: everything older has left the window anyway. */
            w->count = 0;
            w->head = 0;
            w->bucket_start = now_ms;
            break;
        }
        w->fifo[w->head] = w->cur_fifo;
        w->crc[w->head] = w->cur_crc;
        w->frames[w->head] = w->cur_frames;
        w->head = (uint8_t)((w->head + 1u) % HM_LINK_BUCKETS);
        if (w->count < HM_LINK_BUCKETS) {
            w->count++;
        }
        w->cur_fifo = 0;
        w->cur_crc = 0;
        w->cur_frames = 0;
        w->bucket_start += HM_LINK_BUCKET_MS;
    }
}

void hm_link_window(const hm_link_t *w, hm_link_window_t *out)
{
    out->fifo = w->cur_fifo;
    out->crc = w->cur_crc;
    out->frames = w->cur_frames;
    for (unsigned i = 0; i < w->count; i++) {
        unsigned k = (w->head + HM_LINK_BUCKETS - 1u - i) % HM_LINK_BUCKETS;
        out->fifo += w->fifo[k];
        out->crc += w->crc[k];
        out->frames += w->frames[k];
    }
}

bool hm_link_tick(hm_link_t *w, uint64_t fifo, uint64_t crc, uint64_t frames, bool suspended, int64_t now_ms)
{
    link_rotate(w, now_ms);
    if (suspended) {
        w->have_base = false;
    } else if (!w->have_base) {
        w->have_base = true;
        w->base_fifo = fifo;
        w->base_crc = crc;
        w->base_frames = frames;
    } else {
        if (fifo > w->base_fifo) {
            w->cur_fifo = sat_add32(w->cur_fifo, fifo - w->base_fifo);
        }
        if (crc > w->base_crc) {
            w->cur_crc = sat_add32(w->cur_crc, crc - w->base_crc);
        }
        if (frames > w->base_frames) {
            w->cur_frames = sat_add32(w->cur_frames, frames - w->base_frames);
        }
        w->base_fifo = fifo;
        w->base_crc = crc;
        w->base_frames = frames;
    }

    hm_link_window_t win;
    hm_link_window(w, &win);
    bool cond = win.fifo >= 1u ||
                (win.frames >= HM_LINK_MIN_FRAMES && win.crc * 100u > win.frames * HM_LINK_CRC_PCT);
    if (cond) {
        w->alert = true;
        w->last_cond_ms = now_ms;
    } else if (w->alert && w->last_cond_ms != HM_NEVER && now_ms - w->last_cond_ms >= HM_LINK_CLEAR_MS) {
        w->alert = false;
    }
    return w->alert;
}

/* ---- caster budgets --------------------------------------------------------------- */

void hm_slot_init(hm_slot_t *s)
{
    s->restart_ms = HM_NEVER;
}

bool hm_slot_overrun(hm_slot_t *s, const hm_slot_in_t *in, int64_t now_ms)
{
    if (!in->enabled || in->state == HM_CST_DISABLED) {
        return false;
    }
    int64_t budget_ms = (int64_t)in->connect_budget_s * 1000;
    if (s->restart_ms != HM_NEVER && now_ms - s->restart_ms < budget_ms) {
        return false;   /* the last restart takes effect once the slot's blocking call returns */
    }
    bool over = false;
    if (in->state == HM_CST_CONNECTING && in->state_age_s > in->connect_budget_s) {
        over = true;
    } else if (in->state == HM_CST_STREAMING &&
               (uint64_t)in->progress_age_ms > (uint64_t)in->send_timeout_ms + HM_SEND_SLACK_MS) {
        over = true;
    } else if ((int64_t)in->progress_age_ms > budget_ms) {
        over = true;   /* the slot task made no progress at all */
    }
    if (over) {
        s->restart_ms = now_ms;
    }
    return over;
}

bool hm_slot_down(const hm_slot_in_t *in)
{
    if (!in->enabled) {
        return false;
    }
    switch (in->state) {
    case HM_CST_WAIT_NETWORK:
    case HM_CST_BACKOFF:
    case HM_CST_AUTH_FAILED:
    case HM_CST_CONFIG_ERROR:
    case HM_CST_BANNED:
        return true;
    default:
        return false;
    }
}

/* ---- debounce ----------------------------------------------------------------- */

void hm_deb_init(hm_deb_t *d)
{
    memset(d, 0, sizeof(*d));
}

bool hm_deb(hm_deb_t *d, bool raw, int64_t now_ms, uint32_t on_ms, uint32_t off_ms)
{
    if (raw != d->raw) {
        d->raw = raw;
        d->changed_ms = now_ms;
    }
    if (d->raw && !d->out && now_ms - d->changed_ms >= (int64_t)on_ms) {
        d->out = true;
    } else if (!d->raw && d->out && now_ms - d->changed_ms >= (int64_t)off_ms) {
        d->out = false;
    }
    return d->out;
}

/* ---- BOOT button ------------------------------------------------------------- */

void hm_button_init(hm_button_t *b)
{
    memset(b, 0, sizeof(*b));
}

hm_btn_event_t hm_button(hm_button_t *b, bool pressed, int64_t now_ms, uint32_t admin_ms, uint32_t factory_ms)
{
    if (!b->armed) {
        if (!pressed) {
            b->armed = true;
        }
        return HM_BTN_NONE;
    }
    if (pressed) {
        if (!b->down) {
            b->down = true;
            b->down_ms = now_ms;
            b->reached = 0;
            return HM_BTN_NONE;
        }
        int64_t held = now_ms - b->down_ms;
        if (held >= (int64_t)factory_ms && b->reached < 2) {
            b->reached = 2;
            return HM_BTN_FACTORY_REACHED;
        }
        if (held >= (int64_t)admin_ms && b->reached < 1) {
            b->reached = 1;
            return HM_BTN_ADMIN_REACHED;
        }
        return HM_BTN_NONE;
    }
    if (!b->down) {
        return HM_BTN_NONE;
    }
    b->down = false;
    uint8_t r = b->reached;
    b->reached = 0;
    return r == 2 ? HM_BTN_FACTORY : (r == 1 ? HM_BTN_ADMIN : HM_BTN_NONE);
}

/* ---- overall level ------------------------------------------------------------ */

uint8_t hm_level(uint8_t ladder_level, bool degraded)
{
    uint8_t lvl = ladder_level;
    if (degraded && lvl < 1u) {
        lvl = 1u;   /* DEGRADED */
    }
    return lvl;
}
