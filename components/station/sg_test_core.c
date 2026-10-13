/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "sg_test_core.h"

#include <stdio.h>
#include <string.h>

#include "gnss_rtcm3.h"
#include "settings_codec.h"

#define NEVER INT64_MIN

_Static_assert(STATION_SGT_SYS == GNSS_SYS_COUNT, "one entry per constellation");
_Static_assert(STATION_SGT_BANDS == GNSS_BAND_COUNT, "one entry per band bucket");
/* The longest reason: a refused (or failed) switch with the receiver's
 * longest reply, then a failed switch back with it too, both whole. */
#define SGT_REPLY_MAX (sizeof(((sgt_apply_t *)0)->reason) - 1u)
_Static_assert(sizeof("The receiver refused signal group 255 255 ().") - 1u + SGT_REPLY_MAX + 1u +
                       sizeof("Switching back to 255 255 failed ().") - 1u + SGT_REPLY_MAX <
                   STATION_SGT_REASON_MAX,
               "the reason holds a refusal and a failed switch back with whole replies");
_Static_assert(STATION_SGT_REASON_MAX <= 256u, "the record stores the reason with a one-byte length");

static bool same(station_sg_t a, station_sg_t b)
{
    return a.master == b.master && a.slave == b.slave;
}

/* The constellations Onocoy rewards: the only ones that can be "best". */
static bool rewarded(unsigned sys)
{
    return sys == GNSS_SYS_GPS || sys == GNSS_SYS_GLONASS || sys == GNSS_SYS_GALILEO || sys == GNSS_SYS_BEIDOU;
}

int32_t sgt_note_a(uint8_t event, station_sg_t g)
{
    return (int32_t)((uint32_t)event | ((uint32_t)g.master << 8) | ((uint32_t)g.slave << 16));
}

static int32_t pair_b(station_sg_t g)
{
    return (int32_t)(((uint32_t)g.master << 8) | g.slave);
}

static void note(sgt_t *t, uint8_t event, int32_t a, int32_t b)
{
    if (t->note_n >= SGT_NOTES) {
        memmove(&t->notes[0], &t->notes[1], (SGT_NOTES - 1u) * sizeof(t->notes[0]));
        t->note_n = SGT_NOTES - 1u;
    }
    t->notes[t->note_n++] = (sgt_note_t){ .event = event, .a = a, .b = b };
}

void sgt_init(sgt_t *t)
{
    memset(t, 0, sizeof(*t));
    t->state = STATION_SGT_IDLE;
    t->kept = -1;
    t->phase_s = SGT_PHASE_DEFAULT_S;
    t->state_ms = NEVER;
    t->settle_until_ms = NEVER;
    t->next_sample_ms = NEVER;
    t->quiet_since_ms = NEVER;
    t->decide_until_ms = NEVER;
    t->ended_ms = NEVER;
    t->saved_ms = NEVER;
}

bool sgt_running(const sgt_t *t)
{
    return t->state != STATION_SGT_IDLE;
}

int sgt_check_args(const station_sg_t groups[2], uint32_t phase_s)
{
    if (groups == NULL || same(groups[0], groups[1])) {
        return STATION_SGT_E_GROUPS;
    }
    if (phase_s < SGT_PHASE_MIN_S || phase_s > SGT_PHASE_MAX_S) {
        return STATION_SGT_E_PHASE;
    }
    return STATION_SGT_OK;
}

/* ---- transitions ------------------------------------------------------------ */

static void enter(sgt_t *t, uint8_t state, int64_t now)
{
    t->state = state;
    t->state_ms = now;
    t->save_due = true;
}

static void ask_switch(sgt_t *t, station_sg_t target)
{
    t->target = target;
    t->want_switch = true;
}

static void enter_settling(sgt_t *t, int64_t now)
{
    enter(t, STATION_SGT_SETTLING, now);
    t->settle_until_ms = now + (int64_t)SGT_SETTLE_S * 1000;
    t->quiet_since_ms = NEVER;
    t->have_gaps = false;
}

static void begin_phase(sgt_t *t, uint8_t p, int64_t now)
{
    t->phase = p;
    uint8_t gi = t->order[p];
    t->acc[gi].status = STATION_SGT_RES_MEASURING;
    if (t->cur_known && same(t->cur, t->groups[gi])) {
        enter_settling(t, now);   /* it runs already: no switch */
        return;
    }
    enter(t, STATION_SGT_SWITCHING, now);
    ask_switch(t, t->groups[gi]);
}

static void finish(sgt_t *t, int64_t now, int64_t unix_s)
{
    station_sg_t g = t->cur_known ? t->cur : t->target;
    enter(t, STATION_SGT_IDLE, now);
    t->want_switch = false;
    t->ended_ms = now;
    t->ended_unix = unix_s > 0 ? unix_s : 0;
    note(t, SGT_EV_ENDED, sgt_note_a(SGT_EV_ENDED, g), t->outcome);
}

/* End the test on `target` with `outcome` (one switch unless it runs already). */
static void restore(sgt_t *t, station_sg_t target, uint8_t outcome, int64_t now, int64_t unix_s)
{
    t->outcome = outcome;
    t->target = target;
    enter(t, STATION_SGT_RESTORING, now);
    if (t->cur_known && same(t->cur, target) && !t->switch_queued) {
        finish(t, now, unix_s);
        return;
    }
    ask_switch(t, target);
}

static void append_reason(sgt_t *t, const char *text)
{
    size_t n = strlen(t->reason);
    if (n > 0 && n + 1 < sizeof(t->reason)) {
        t->reason[n++] = ' ';
        t->reason[n] = '\0';
    }
    snprintf(t->reason + n, sizeof(t->reason) - n, "%s", text);
}

/* ---- requests --------------------------------------------------------------- */

int sgt_start(sgt_t *t, const sgt_guard_t *g, const station_sg_t groups[2], uint32_t phase_s, int64_t now_ms)
{
    int e = sgt_check_args(groups, phase_s);
    if (e != STATION_SGT_OK) {
        return e;
    }
    if (sgt_running(t)) {
        return STATION_SGT_E_RUNNING;
    }
    if (!g->settings_ok) {
        return STATION_SGT_E_SETTINGS;
    }
    if (g->first_apply_held) {
        return STATION_SGT_E_FIRST_APPLY_HELD;
    }
    if (!g->autocfg) {
        return STATION_SGT_E_AUTOCFG_OFF;
    }
    if (g->maintenance) {
        return STATION_SGT_E_MAINTENANCE;
    }
    if (g->journal_open) {
        return STATION_SGT_E_JOURNAL_OPEN;
    }
    if (g->safe_mode) {
        return STATION_SGT_E_SAFE_MODE;
    }
    if (g->no_receiver) {
        return STATION_SGT_E_NO_RECEIVER;
    }
    if (!g->supported) {
        return STATION_SGT_E_UNSUPPORTED;
    }
    if (!g->identity_confirmed) {
        return STATION_SGT_E_IDENTITY;
    }
    if (!g->streaming) {
        return STATION_SGT_E_NOT_STREAMING;
    }
    if (g->config_failed) {
        return STATION_SGT_E_CONFIG_FAILED;
    }
    if (!g->position_locked) {
        return STATION_SGT_E_NOT_LOCKED;
    }

    memset(t->acc, 0, sizeof(t->acc));
    t->have_test = true;
    t->outcome = STATION_SGT_OUT_NONE;
    t->kept = -1;
    t->reason[0] = '\0';
    t->ended_unix = 0;
    t->ended_ms = NEVER;
    t->groups[0] = groups[0];
    t->groups[1] = groups[1];
    t->before = g->running;
    t->cur = g->running;
    t->cur_known = true;
    t->phase_s = (uint16_t)phase_s;
    t->decide_until_ms = NEVER;
    t->want_switch = false;
    t->switch_queued = false;
    /* The group running now, if chosen, is measured first: one switch fewer. */
    bool b_first = same(groups[1], g->running);
    t->order[0] = b_first ? 1 : 0;
    t->order[1] = b_first ? 0 : 1;
    note(t, SGT_EV_STARTED, sgt_note_a(SGT_EV_STARTED, groups[t->order[0]]), (int32_t)phase_s);
    begin_phase(t, 0, now_ms);
    return STATION_SGT_OK;
}

int sgt_keep(sgt_t *t, unsigned idx, int64_t now_ms, int64_t unix_s)
{
    if (t->state != STATION_SGT_DECIDING) {
        return STATION_SGT_E_NOT_DECIDING;
    }
    if (idx > 1) {
        return STATION_SGT_E_GROUPS;
    }
    t->kept = (int8_t)idx;
    snprintf(t->reason, sizeof(t->reason), "You kept signal group %u %u.", (unsigned)t->groups[idx].master,
             (unsigned)t->groups[idx].slave);
    restore(t, t->groups[idx], STATION_SGT_OUT_KEPT, now_ms, unix_s);
    return STATION_SGT_OK;
}

int sgt_stop(sgt_t *t, int64_t now_ms, int64_t unix_s)
{
    if (!sgt_running(t)) {
        return STATION_SGT_E_IDLE;
    }
    if (t->state == STATION_SGT_RESTORING && same(t->target, t->before)) {
        return STATION_SGT_OK;   /* already on its way back */
    }
    if (t->state == STATION_SGT_SWITCHING || t->state == STATION_SGT_SETTLING || t->state == STATION_SGT_MEASURING) {
        t->acc[t->order[t->phase]].status =
            t->acc[t->order[t->phase]].samples > 0 ? STATION_SGT_RES_DONE : STATION_SGT_RES_PENDING;
    }
    t->kept = -1;
    snprintf(t->reason, sizeof(t->reason), "Stopped by you: back to signal group %u %u.", (unsigned)t->before.master,
             (unsigned)t->before.slave);
    restore(t, t->before, STATION_SGT_OUT_CANCELLED, now_ms, unix_s);
    return STATION_SGT_OK;
}

void sgt_switch_queued(sgt_t *t)
{
    t->want_switch = false;
    t->switch_queued = true;
    t->applying = t->target;
}

/* ---- the station's results ------------------------------------------------------ */

void sgt_apply_done(sgt_t *t, const sgt_apply_t *a, int64_t now_ms, int64_t unix_s)
{
    if (!t->switch_queued) {
        return;   /* not an apply this test asked for */
    }
    t->switch_queued = false;
    const station_sg_t g = t->applying;
    if (a->ok) {
        t->cur = g;
        t->cur_known = true;
    } else if (a->read_known) {
        t->cur = a->read;
        t->cur_known = true;
    } else {
        t->cur_known = false;
    }
    if (t->want_switch) {
        return;   /* a newer switch replaced this one: wait for that */
    }
    char text[STATION_SGT_REASON_MAX];
    switch (t->state) {
    case STATION_SGT_SWITCHING:
        if (a->ok) {
            note(t, SGT_EV_SWITCHED, sgt_note_a(SGT_EV_SWITCHED, g), t->phase);
            enter_settling(t, now_ms);
            return;
        }
        if (a->rejected) {
            t->acc[t->order[t->phase]].status = STATION_SGT_RES_REJECTED;
            note(t, SGT_EV_REFUSED, sgt_note_a(SGT_EV_REFUSED, g), pair_b(t->before));
            /* The receiver's whole reply, as the receiver card shows it (the
             * reason has room for it and for a failed switch back after it). */
            snprintf(t->reason, sizeof(t->reason), "The receiver refused signal group %u %u (%s).",
                     (unsigned)g.master, (unsigned)g.slave, a->reason[0] != '\0' ? a->reason : "rejected");
            restore(t, t->before, STATION_SGT_OUT_REJECTED, now_ms, unix_s);
            return;
        }
        t->acc[t->order[t->phase]].status = STATION_SGT_RES_PENDING;
        snprintf(t->reason, sizeof(t->reason), "Switching to signal group %u %u failed (%s).", (unsigned)g.master,
                 (unsigned)g.slave, a->reason[0] != '\0' ? a->reason : "failed");
        restore(t, t->before, STATION_SGT_OUT_FAILED, now_ms, unix_s);
        return;
    case STATION_SGT_RESTORING:
        if (!a->ok) {
            snprintf(text, sizeof(text), "Switching back to %u %u failed (%s).", (unsigned)g.master,
                     (unsigned)g.slave, a->reason[0] != '\0' ? a->reason : "failed");
            append_reason(t, text);
        }
        finish(t, now_ms, unix_s);
        return;
    default:
        return;
    }
}

bool sgt_sample_due(const sgt_t *t, int64_t now_ms)
{
    return t->state == STATION_SGT_MEASURING && t->next_sample_ms != NEVER && now_ms >= t->next_sample_ms;
}

static void add_sample(sgt_acc_t *a, const sgt_sample_t *s)
{
    a->samples++;
    for (unsigned sys = 0; sys < STATION_SGT_SYS; sys++) {
        if (sys == GNSS_SYS_SBAS) {
            continue;   /* never listed (see gnss_rtcm3.h) */
        }
        a->sys_sat_sum[sys] += s->sys_sats[sys];
        a->sig_union[sys] |= s->signal_mask[sys];
        for (unsigned b = 0; b < STATION_SGT_BANDS; b++) {
            uint8_t n = s->band_sats[sys][b];
            a->sat_sum[sys][b] += n;
            if (n > 0 && a->band_seen[sys][b] < UINT16_MAX) {
                a->band_seen[sys][b]++;
            }
            if (n > 0 && s->cnr_dbhz[sys][b] != 0 && a->cnr_n[sys][b] < UINT16_MAX) {
                a->cnr_sum[sys][b] += s->cnr_dbhz[sys][b];
                a->cnr_n[sys][b]++;
            }
        }
    }
}

static void take_sample(sgt_t *t, const sgt_sample_t *s, int64_t now)
{
    t->next_sample_ms = now + (int64_t)SGT_SAMPLE_S * 1000;
    sgt_acc_t *a = &t->acc[t->order[t->phase]];
    bool good = s->streaming && s->epoch_age_ms < SGT_FRESH_MS;
    uint64_t gaps = 0;
    if (t->have_gaps && s->epoch_gaps >= t->last_gaps) {
        gaps = s->epoch_gaps - t->last_gaps;   /* a smaller total: the counters started over */
    }
    t->last_gaps = s->epoch_gaps;
    t->have_gaps = true;
    if (!good) {
        return;
    }
    a->epoch_gaps += gaps > UINT32_MAX - a->epoch_gaps ? UINT32_MAX - a->epoch_gaps : (uint32_t)gaps;
    add_sample(a, s);
}

void sgt_tick(sgt_t *t, bool streaming, const sgt_sample_t *sample, int64_t now_ms, int64_t unix_s)
{
    char text[STATION_SGT_REASON_MAX];
    switch (t->state) {
    case STATION_SGT_SWITCHING:
    case STATION_SGT_RESTORING:
        if (now_ms - t->state_ms >= (int64_t)SGT_SWITCH_MAX_S * 1000) {
            station_sg_t g = t->target;
            if (t->state == STATION_SGT_SWITCHING) {
                t->acc[t->order[t->phase]].status = STATION_SGT_RES_PENDING;
                snprintf(t->reason, sizeof(t->reason), "Switching to signal group %u %u did not finish within %u minutes.",
                         (unsigned)g.master, (unsigned)g.slave, (unsigned)(SGT_SWITCH_MAX_S / 60u));
                /* A re-apply may still be queued with that group: the
                 * restore always switches (its profile write replaces it). */
                t->cur_known = false;
                restore(t, t->before, STATION_SGT_OUT_FAILED, now_ms, unix_s);
            } else {
                snprintf(text, sizeof(text), "Switching to %u %u did not finish; the station retries its profile.",
                         (unsigned)g.master, (unsigned)g.slave);
                append_reason(t, text);
                t->switch_queued = false;
                finish(t, now_ms, unix_s);
            }
        }
        return;
    case STATION_SGT_SETTLING:
    case STATION_SGT_MEASURING:
        if (streaming) {
            t->quiet_since_ms = NEVER;
        } else if (t->quiet_since_ms == NEVER) {
            t->quiet_since_ms = now_ms;
        } else if (now_ms - t->quiet_since_ms >= (int64_t)SGT_NOT_STREAMING_S * 1000) {
            station_sg_t g = t->groups[t->order[t->phase]];
            sgt_acc_t *a = &t->acc[t->order[t->phase]];
            a->status = a->samples > 0 ? STATION_SGT_RES_DONE : STATION_SGT_RES_PENDING;
            snprintf(t->reason, sizeof(t->reason), "The receiver did not stream for %u minutes on signal group %u %u.",
                     (unsigned)(SGT_NOT_STREAMING_S / 60u), (unsigned)g.master, (unsigned)g.slave);
            restore(t, t->before, STATION_SGT_OUT_FAILED, now_ms, unix_s);
            return;
        }
        if (t->state == STATION_SGT_SETTLING) {
            if (now_ms >= t->settle_until_ms) {
                enter(t, STATION_SGT_MEASURING, now_ms);
                t->next_sample_ms = now_ms + (int64_t)SGT_SAMPLE_S * 1000;
            }
            return;
        }
        if (sample != NULL && sgt_sample_due(t, now_ms)) {
            take_sample(t, sample, now_ms);
        }
        if ((uint64_t)t->acc[t->order[t->phase]].samples * SGT_SAMPLE_S >= t->phase_s) {
            t->acc[t->order[t->phase]].status = STATION_SGT_RES_DONE;
            if (t->phase == 0) {
                begin_phase(t, 1, now_ms);
            } else {
                enter(t, STATION_SGT_DECIDING, now_ms);
                t->decide_until_ms = now_ms + (int64_t)SGT_DECIDE_S * 1000;
            }
            return;
        }
        if (t->saved_ms == NEVER || now_ms - t->saved_ms >= (int64_t)SGT_SAVE_MEASURE_S * 1000) {
            t->save_due = true;
        }
        return;
    case STATION_SGT_DECIDING:
        if (now_ms >= t->decide_until_ms) {
            snprintf(t->reason, sizeof(t->reason), "No choice within %u hours: back to signal group %u %u.",
                     (unsigned)(SGT_DECIDE_S / 3600u), (unsigned)t->before.master, (unsigned)t->before.slave);
            restore(t, t->before, STATION_SGT_OUT_TIMED_OUT, now_ms, unix_s);
            return;
        }
        if (t->saved_ms == NEVER || now_ms - t->saved_ms >= (int64_t)SGT_SAVE_DECIDE_S * 1000) {
            t->save_due = true;
        }
        return;
    default:
        return;
    }
}

void sgt_saved(sgt_t *t, int64_t now_ms)
{
    t->save_due = false;
    t->saved_ms = now_ms;
}

bool sgt_store_at_shutdown(const sgt_t *t)
{
    return sgt_running(t) || t->save_due;
}

/* ---- record ------------------------------------------------------------------------ */

static uint32_t left_ms_s(int64_t until, int64_t now)
{
    if (until == NEVER || until <= now) {
        return 0;
    }
    int64_t s = (until - now + 999) / 1000;
    return s > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}

static void put_pair(blob_writer_t *w, station_sg_t g)
{
    blob_put_u8(w, g.master);
    blob_put_u8(w, g.slave);
}

static station_sg_t get_pair(blob_reader_t *r)
{
    station_sg_t g;
    g.master = blob_get_u8(r);
    g.slave = blob_get_u8(r);
    return g;
}

size_t sgt_encode(const sgt_t *t, int64_t now_ms, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, SGT_REC_VERSION);
    blob_put_u8(&w, t->state);
    blob_put_u8(&w, t->outcome);
    blob_put_bool(&w, t->have_test);
    blob_put_u8(&w, t->phase);
    blob_put_u8(&w, t->order[0]);
    blob_put_u8(&w, t->order[1]);
    put_pair(&w, t->groups[0]);
    put_pair(&w, t->groups[1]);
    put_pair(&w, t->before);
    blob_put_bool(&w, t->cur_known);
    put_pair(&w, t->cur);
    put_pair(&w, t->target);
    blob_put_u16(&w, t->phase_s);
    blob_put_u8(&w, t->kept < 0 ? 0xFFu : (uint8_t)t->kept);
    blob_put_u32(&w, t->state == STATION_SGT_DECIDING ? left_ms_s(t->decide_until_ms, now_ms) : 0);
    blob_put_i64(&w, t->ended_unix);
    blob_put_str(&w, t->reason);
    for (unsigned i = 0; i < 2; i++) {
        const sgt_acc_t *a = &t->acc[i];
        blob_put_u8(&w, a->status);
        blob_put_u32(&w, a->samples);
        blob_put_u32(&w, a->epoch_gaps);
        for (unsigned s = 0; s < STATION_SGT_SYS; s++) {
            blob_put_u32(&w, a->sys_sat_sum[s]);
            blob_put_u32(&w, a->sig_union[s]);
            for (unsigned b = 0; b < STATION_SGT_BANDS; b++) {
                blob_put_u32(&w, a->sat_sum[s][b]);
                blob_put_u32(&w, a->cnr_sum[s][b]);
                blob_put_u16(&w, a->cnr_n[s][b]);
                blob_put_u16(&w, a->band_seen[s][b]);
            }
        }
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

/* A record that cannot be read leaves the test idle (sgt_decode). */
static bool decode_failed(sgt_t *t)
{
    sgt_init(t);
    return false;
}

bool sgt_decode(sgt_t *t, const uint8_t *buf, size_t len, int64_t now_ms)
{
    /* Straight into *t (the record is large: no second copy anywhere); any
     * failure leaves it as sgt_init() does. */
    sgt_init(t);
    if (buf == NULL || len < 1 || buf[0] != SGT_REC_VERSION) {
        return false;
    }
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    (void)blob_get_u8(&r);
    t->state = blob_get_u8(&r);
    t->outcome = blob_get_u8(&r);
    t->have_test = blob_get_bool(&r);
    t->phase = blob_get_u8(&r);
    t->order[0] = blob_get_u8(&r);
    t->order[1] = blob_get_u8(&r);
    t->groups[0] = get_pair(&r);
    t->groups[1] = get_pair(&r);
    t->before = get_pair(&r);
    t->cur_known = blob_get_bool(&r);
    t->cur = get_pair(&r);
    t->target = get_pair(&r);
    t->phase_s = blob_get_u16(&r);
    uint8_t kept = blob_get_u8(&r);
    uint32_t decide_left = blob_get_u32(&r);
    t->ended_unix = blob_get_i64(&r);
    blob_get_str(&r, t->reason, sizeof(t->reason));
    for (unsigned i = 0; i < 2; i++) {
        sgt_acc_t *a = &t->acc[i];
        a->status = blob_get_u8(&r);
        a->samples = blob_get_u32(&r);
        a->epoch_gaps = blob_get_u32(&r);
        for (unsigned s = 0; s < STATION_SGT_SYS; s++) {
            a->sys_sat_sum[s] = blob_get_u32(&r);
            a->sig_union[s] = blob_get_u32(&r);
            for (unsigned b = 0; b < STATION_SGT_BANDS; b++) {
                a->sat_sum[s][b] = blob_get_u32(&r);
                a->cnr_sum[s][b] = blob_get_u32(&r);
                a->cnr_n[s][b] = blob_get_u16(&r);
                a->band_seen[s][b] = blob_get_u16(&r);
            }
        }
        if (a->status > STATION_SGT_RES_REJECTED) {
            return decode_failed(t);
        }
    }
    if (!blob_reader_ok(&r) || t->state > STATION_SGT_RESTORING || t->outcome > STATION_SGT_OUT_INTERRUPTED ||
        t->phase > 1 || t->order[0] > 1 || t->order[1] != 1u - t->order[0] || (kept != 0xFFu && kept > 1) ||
        decide_left > SGT_DECIDE_S) {
        return decode_failed(t);
    }
    if (t->state != STATION_SGT_IDLE &&
        (!t->have_test || t->phase_s < SGT_PHASE_MIN_S || t->phase_s > SGT_PHASE_MAX_S ||
         same(t->groups[0], t->groups[1]))) {
        return decode_failed(t);
    }
    t->kept = kept == 0xFFu ? -1 : (int8_t)kept;
    t->saved_ms = now_ms;
    t->state_ms = now_ms;
    switch (t->state) {
    case STATION_SGT_SWITCHING:
    case STATION_SGT_RESTORING:
        t->want_switch = true;   /* ask for the switch again */
        break;
    case STATION_SGT_SETTLING:
    case STATION_SGT_MEASURING:
        enter_settling(t, now_ms);   /* settle again; what was measured stays */
        break;
    case STATION_SGT_DECIDING:
        t->decide_until_ms = now_ms + (int64_t)decide_left * 1000;
        break;
    default:
        break;
    }
    return true;
}

void sgt_interrupted(sgt_t *t, int64_t now_ms, int64_t unix_s)
{
    sgt_init(t);
    t->outcome = STATION_SGT_OUT_INTERRUPTED;
    snprintf(t->reason, sizeof(t->reason), "The comparison's stored record could not be read after a restart.");
    t->ended_ms = now_ms;
    t->ended_unix = unix_s > 0 ? unix_s : 0;
    note(t, SGT_EV_ENDED, SGT_EV_ENDED, STATION_SGT_OUT_INTERRUPTED);
}

/* ---- views ------------------------------------------------------------------------- */

static uint16_t mean_x10(uint64_t sum, uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    uint64_t v = (sum * 10u + n / 2u) / n;
    return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

/* Signal IDs of one band bucket (bit id-1). */
static uint32_t band_ids(unsigned sys, unsigned band)
{
    uint32_t m = 0;
    for (unsigned id = 1; id <= 32; id++) {
        if (gnss_rtcm3_msm_band((gnss_sys_t)sys, id) == (gnss_band_t)band) {
            m |= (uint32_t)1 << (id - 1u);
        }
    }
    return m;
}

static void result_view(const sgt_acc_t *a, station_sg_t g, station_sgt_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->group = g;
    r->status = a->status;
    r->samples = a->samples;
    r->measured_s = a->samples * SGT_SAMPLE_S;
    r->epoch_gaps = a->epoch_gaps;
    r->best_sys = 0xFF;
    if (a->samples == 0) {
        return;
    }
    for (unsigned sys = 0; sys < STATION_SGT_SYS; sys++) {
        if (sys == GNSS_SYS_SBAS) {
            continue;
        }
        station_sgt_sys_t *v = &r->sys[r->nsys];
        memset(v, 0, sizeof(*v));
        for (unsigned b = 0; b < STATION_SGT_BANDS; b++) {
            /* A band counts when it was there in at least half of the samples. */
            if (a->band_seen[sys][b] == 0 || (uint32_t)a->band_seen[sys][b] * 2u < a->samples) {
                continue;
            }
            station_sgt_band_t *e = &v->band[v->nband++];
            e->band = (uint8_t)b;
            e->sats_x10 = mean_x10(a->sat_sum[sys][b], a->samples);
            e->cnr_x10 = mean_x10(a->cnr_sum[sys][b], a->cnr_n[sys][b]);
            e->signal_mask = a->sig_union[sys] & band_ids(sys, b);
        }
        if (v->nband == 0) {
            continue;
        }
        v->sys = (uint8_t)sys;
        v->sats_x10 = mean_x10(a->sys_sat_sum[sys], a->samples);
        if (rewarded(sys) && v->nband > r->best_bands) {
            r->best_bands = v->nband;
            r->best_sys = (uint8_t)sys;
        }
        r->nsys++;
    }
}

static int8_t view_phase(const sgt_t *t)
{
    return (t->state == STATION_SGT_SWITCHING || t->state == STATION_SGT_SETTLING || t->state == STATION_SGT_MEASURING)
               ? (int8_t)t->phase
               : -1;
}

static uint32_t phase_left_s(const sgt_t *t, int64_t now)
{
    uint32_t measured = t->acc[t->order[t->phase]].samples * SGT_SAMPLE_S;
    uint32_t rest = measured < t->phase_s ? t->phase_s - measured : 0;
    if (t->state == STATION_SGT_SETTLING) {
        return left_ms_s(t->settle_until_ms, now) + rest;
    }
    if (t->state == STATION_SGT_MEASURING) {
        return rest;
    }
    return UINT32_MAX;
}

static bool view_running(const sgt_t *t, station_sg_t *g)
{
    switch (t->state) {
    case STATION_SGT_SWITCHING:
    case STATION_SGT_SETTLING:
    case STATION_SGT_MEASURING:
        *g = t->groups[t->order[t->phase]];
        return true;
    case STATION_SGT_RESTORING:
        *g = t->target;
        return true;
    case STATION_SGT_DECIDING:
        *g = t->cur;
        return t->cur_known;
    default:
        return false;
    }
}

static uint32_t ended_age(const sgt_t *t, int64_t now)
{
    if (t->ended_ms == NEVER || now < t->ended_ms) {
        return UINT32_MAX;
    }
    int64_t s = (now - t->ended_ms) / 1000;
    return s >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)s;
}

void sgt_view(const sgt_t *t, int64_t now_ms, station_sg_test_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = t->state;
    out->have_test = t->have_test;
    out->groups[0] = t->groups[0];
    out->groups[1] = t->groups[1];
    out->before = t->before;
    out->phase = view_phase(t);
    out->phase_s = t->phase_s;
    out->settle_s = SGT_SETTLE_S;
    out->left_s = phase_left_s(t, now_ms);
    out->decide_left_s = t->state == STATION_SGT_DECIDING ? left_ms_s(t->decide_until_ms, now_ms) : UINT32_MAX;
    out->have_running = view_running(t, &out->running);
    if (t->have_test) {
        for (unsigned i = 0; i < 2; i++) {
            result_view(&t->acc[i], t->groups[i], &out->results[i]);
        }
    }
    /* Decided once the test restores, final once it is idle. */
    out->outcome =
        (t->state == STATION_SGT_IDLE || t->state == STATION_SGT_RESTORING) ? t->outcome : STATION_SGT_OUT_NONE;
    out->have_kept = out->outcome == STATION_SGT_OUT_KEPT && t->kept >= 0;
    if (out->have_kept) {
        out->kept = t->groups[t->kept];
    }
    snprintf(out->reason, sizeof(out->reason), "%s", t->reason);
    out->ended_unix = t->state == STATION_SGT_IDLE ? t->ended_unix : 0;
    out->ended_age_s = t->state == STATION_SGT_IDLE ? ended_age(t, now_ms) : UINT32_MAX;
}

static uint32_t round_up(uint32_t v, uint32_t step)
{
    if (v == UINT32_MAX) {
        return v;
    }
    uint64_t r = ((uint64_t)v + step - 1u) / step * step;
    return r >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)r;
}

void sgt_brief(const sgt_t *t, int64_t now_ms, station_sgt_brief_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = t->state;
    out->phase = view_phase(t);
    out->have_running = view_running(t, &out->running);
    out->left_s = round_up(phase_left_s(t, now_ms), 10u);
    out->decide_left_s =
        t->state == STATION_SGT_DECIDING ? round_up(left_ms_s(t->decide_until_ms, now_ms), 60u) : UINT32_MAX;
    out->outcome = t->state == STATION_SGT_IDLE ? t->outcome : STATION_SGT_OUT_NONE;
    out->ended_age_s = t->state == STATION_SGT_IDLE ? ended_age(t, now_ms) : UINT32_MAX;
}

unsigned sgt_take_notes(sgt_t *t, sgt_note_t *out, unsigned cap)
{
    unsigned n = t->note_n < cap ? t->note_n : cap;
    memcpy(out, t->notes, n * sizeof(out[0]));
    if (n < t->note_n) {
        memmove(&t->notes[0], &t->notes[n], (t->note_n - n) * sizeof(t->notes[0]));
    }
    t->note_n = (uint8_t)(t->note_n - n);
    return n;
}
