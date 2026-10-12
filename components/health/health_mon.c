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

/* ---- power lost ------------------------------------------------------------------ */

void hm_power_init(hm_power_t *p)
{
    memset(p, 0, sizeof(*p));
}

void hm_power_feed(hm_power_t *p, uint16_t kind, int32_t a, int64_t unix_s, uint32_t uptime_s)
{
    if (kind == HM_EV_BOOT) {
        p->cur_power = (a == HM_RESET_POWER_ON);
        if (p->cur_power) {
            if (p->n == HM_POWER_BOOTS) {
                memmove(&p->boot[0], &p->boot[1], (HM_POWER_BOOTS - 1u) * sizeof(p->boot[0]));
                p->n--;
            }
            p->boot[p->n].t = 0;
            p->boot[p->n].ub = 0;
            p->n++;
        }
    }
    if (unix_s <= 0) {
        return;
    }
    /* This record's boot started at `start`: every earlier boot came before it. */
    int64_t start = unix_s - (int64_t)uptime_s;
    if (start <= 0) {
        start = 1;
    }
    uint8_t earlier = p->n;
    if (p->cur_power && p->n > 0) {
        earlier = (uint8_t)(p->n - 1u);
        if (p->boot[p->n - 1u].t == 0) {
            p->boot[p->n - 1u].t = start;
        }
    }
    for (uint8_t i = 0; i < earlier; i++) {
        if (p->boot[i].t == 0 && (p->boot[i].ub == 0 || start < p->boot[i].ub)) {
            p->boot[i].ub = start;
        }
    }
}

void hm_power_running(hm_power_t *p, int64_t now_utc, uint32_t uptime_s)
{
    if (now_utc <= 0) {
        return;
    }
    /* A dated point of the running boot, as any of its records would give. */
    hm_power_feed(p, 0, 0, now_utc, uptime_s);
}

bool hm_power_alert(const hm_power_t *p, int64_t now_utc)
{
    if (p->n == 0) {
        return false;
    }
    if (now_utc <= 0) {
        unsigned undated = 0;
        for (uint8_t i = 0; i < p->n; i++) {
            if (p->boot[i].t == 0 && p->boot[i].ub == 0) {
                undated++;
            }
        }
        return undated >= HM_POWER_COUNT;
    }
    int64_t e[HM_POWER_BOOTS];
    for (uint8_t i = 0; i < p->n; i++) {
        int64_t t = p->boot[i].t != 0 ? p->boot[i].t : (p->boot[i].ub != 0 ? p->boot[i].ub : now_utc);
        e[i] = t > now_utc ? now_utc : t;
    }
    uint8_t last = (uint8_t)(p->n - 1u);
    if (now_utc - e[last] >= HM_POWER_WINDOW_S) {
        return false;   /* 24 h without a power-on boot */
    }
    /* The run of boots ending with the newest, each < 24 h after the one before. */
    uint8_t first = last;
    while (first > 0 && e[first] - e[first - 1u] < HM_POWER_WINDOW_S) {
        first--;
    }
    for (uint8_t k = first; k <= last; k++) {
        unsigned in_window = 0;
        for (uint8_t j = first; j <= k; j++) {
            if (e[k] - e[j] < HM_POWER_WINDOW_S) {
                in_window++;
            }
        }
        if (in_window >= HM_POWER_COUNT) {
            return true;
        }
    }
    return false;
}

/* ---- GNSS ladder restart hold ---------------------------------------------------- */

void hm_hold_init(hm_hold_t *h)
{
    h->in_episode = false;
    h->present = false;
    h->last_ms = HM_NEVER;
    h->presence_ms = 0;
}

uint8_t hm_reboot_hold(hm_hold_t *h, const hm_hold_in_t *in, uint32_t *left_s)
{
    int64_t dt = (h->last_ms == HM_NEVER || in->now_ms < h->last_ms) ? 0 : in->now_ms - h->last_ms;
    h->last_ms = in->now_ms;

    uint8_t hold = 0;
    if (in->settings_available && !in->setup_done) {
        hold |= HM_HOLD_SETUP;
    }
    if (in->station_time_s < in->power_on_grace_s && !in->heard) {
        hold |= HM_HOLD_POWER_ON;
    }
    uint8_t presence = 0;
    if (in->ap_clients > 0) {
        presence |= HM_HOLD_AP_CLIENT;
    }
    if (in->admin_at_ms != HM_NEVER && in->now_ms >= in->admin_at_ms &&
        in->now_ms - in->admin_at_ms < (int64_t)in->admin_window_ms) {
        presence |= HM_HOLD_ADMIN;
    }

    /* The presence limit counts per ladder episode. */
    if (!in->episode) {
        h->in_episode = false;
        h->presence_ms = 0;
    } else {
        if (!h->in_episode) {
            h->in_episode = true;
            h->presence_ms = 0;
            dt = 0;   /* the episode starts with this sample */
        }
        /* Time counts between two samples that both had someone present. */
        if (presence != 0 && h->present && h->presence_ms < in->presence_max_ms) {
            h->presence_ms += (uint64_t)dt;
        }
    }
    h->present = presence != 0;
    uint32_t left = UINT32_MAX;
    if (presence != 0) {
        if (h->presence_ms >= in->presence_max_ms) {
            presence = 0;   /* over the limit: a present person no longer holds the restart back */
        } else {
            uint64_t rest = in->presence_max_ms - h->presence_ms;
            left = (uint32_t)((rest + 999u) / 1000u);
        }
    }
    if (left_s != NULL) {
        *left_s = left;
    }
    return (uint8_t)(hold | presence);
}
