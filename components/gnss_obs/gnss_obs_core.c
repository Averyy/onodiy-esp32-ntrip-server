/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_obs_core.h"

#include <string.h>

#define WEEK_MS 604800000u
#define DAY_MS  86400000u

static unsigned popcount64(uint64_t v)
{
    unsigned n = 0;
    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}

/* Seen within `window_ms` before now (a stamp from the future counts as now). */
static bool within(uint64_t seen_ms, uint64_t now_ms, uint32_t window_ms)
{
    return now_ms <= seen_ms || now_ms - seen_ms < window_ms;
}

/* Age for a view: 0 for a stamp from the future, capped below OBS_NEVER. */
static uint32_t age_ms(uint64_t then_ms, uint64_t now_ms)
{
    if (now_ms <= then_ms) {
        return 0;
    }
    uint64_t age = now_ms - then_ms;
    return (age >= OBS_NEVER) ? OBS_NEVER - 1u : (uint32_t)age;
}

/* ---- epoch gaps ------------------------------------------------------------ */

/*
 * Advance from epoch time a to epoch time b of one constellation in ms,
 * modulo its week (GLONASS with an unknown day of week: modulo a day);
 * UINT64_MAX when either value is out of range.
 */
static uint64_t epoch_advance_ms(gnss_sys_t sys, uint32_t a, uint32_t b)
{
    uint64_t ta = a;
    uint64_t tb = b;
    uint64_t wrap = WEEK_MS;
    if (sys == GNSS_SYS_GLONASS) {
        uint32_t da = a >> 27;
        uint32_t db = b >> 27;
        ta = a & 0x07FFFFFFu;
        tb = b & 0x07FFFFFFu;
        if (da == 7 || db == 7) {
            wrap = DAY_MS;
        } else {
            ta += (uint64_t)da * DAY_MS;
            tb += (uint64_t)db * DAY_MS;
        }
    }
    if (ta >= wrap || tb >= wrap) {
        return UINT64_MAX;
    }
    return (tb + wrap - ta) % wrap;
}

/* Gaps found on constellation s count for the station only while no lower
 * constellation with a configured period is present. */
static bool is_reference(const obs_core_t *c, unsigned s, uint64_t now_ms)
{
    for (unsigned r = 0; r < s; r++) {
        if (c->cfg.period_ms[r] != 0 && (c->msm_ever & (1u << r)) &&
            within(c->msm_seen_ms[r], now_ms, OBS_SIGNAL_WINDOW_MS)) {
            return false;
        }
    }
    return true;
}

static void count_gaps(obs_core_t *c, unsigned s, uint32_t from, uint32_t to, uint64_t now_ms)
{
    const uint64_t period = c->cfg.period_ms[s];
    if (period == 0) {
        return;
    }
    const bool ref = is_reference(c, s, now_ms);
    const uint64_t limit = (4u * period > OBS_EPOCH_GAP_MAX_MS) ? 4u * period : OBS_EPOCH_GAP_MAX_MS;
    uint64_t d = epoch_advance_ms((gnss_sys_t)s, from, to);
    if (d > limit) {
        c->sys_jumps[s]++;
        if (ref) {
            c->epoch_jumps++;
        }
        return;
    }
    uint64_t n = (d + period / 2u) / period;   /* epochs advanced, rounded */
    if (n < 2) {
        return;
    }
    c->sys_gaps[s] += n - 1;
    if (ref) {
        c->epoch_gaps += n - 1;
    }
}

/* ---- CNR -------------------------------------------------------------------- */

/* Close constellation s's epoch in progress: its per-band means become the shown ones. */
static void cnr_finish(obs_core_t *c, unsigned s, uint64_t now_ms)
{
    const uint8_t bit = (uint8_t)(1u << s);
    if (!(c->cnr_open & bit)) {
        return;
    }
    for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
        uint32_t n = c->cnr_acc_n[s][b];
        if (n != 0) {
            c->cnr_q4[s][b] = (uint16_t)((c->cnr_acc_q4[s][b] + n / 2u) / n);
            c->cnr_ms[s][b] = now_ms;
            c->cnr_valid[s] |= (uint8_t)(1u << b);
        }
        c->cnr_acc_q4[s][b] = 0;
        c->cnr_acc_n[s][b] = 0;
    }
    c->cnr_open &= (uint8_t)~bit;
}

static void cnr_add(obs_core_t *c, unsigned s, const obs_cnr_t *cnr)
{
    for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
        uint32_t n = cnr->cells[b];
        if (n == 0 || c->cnr_acc_n[s][b] + n > UINT16_MAX) {
            continue;   /* nothing, or an epoch time that never moves on: keep the mean as it is */
        }
        c->cnr_acc_q4[s][b] += cnr->sum_q4[b];
        c->cnr_acc_n[s][b] = (uint16_t)(c->cnr_acc_n[s][b] + n);
        c->cnr_open |= (uint8_t)(1u << s);
    }
}

static uint8_t q4_to_dbhz(uint16_t q4)
{
    uint32_t v = ((uint32_t)q4 + 8u) / 16u;
    if (v == 0) {
        v = 1;   /* a measured value never reads as "not available" */
    }
    return (v > UINT8_MAX) ? UINT8_MAX : (uint8_t)v;
}

/* ---- message table ------------------------------------------------------- */

static obs_msg_t *msg_find(obs_core_t *c, uint16_t msg)
{
    for (unsigned i = 0; i < c->msg_count; i++) {
        if (c->msg[i].msg == msg) {
            return &c->msg[i];
        }
    }
    return NULL;
}

/*
 * A fresh entry for msg: a free one, or, with the table full, the entry of
 * the number not in the profile that was seen least recently; that one
 * only when it was not seen for OBS_MSG_STALE_MS, unless `force`.
 */
static obs_msg_t *msg_take(obs_core_t *c, uint16_t msg, uint64_t now_ms, bool force)
{
    obs_msg_t *e = NULL;
    if (c->msg_count < OBS_MSG_MAX) {
        e = &c->msg[c->msg_count++];
    } else {
        for (unsigned i = 0; i < OBS_MSG_MAX; i++) {
            obs_msg_t *x = &c->msg[i];
            if (x->expected_period_s != 0) {
                continue;
            }
            uint64_t tx = x->frames ? x->last_ms : 0;
            if (e == NULL || tx < (e->frames ? e->last_ms : 0)) {
                e = x;
            }
        }
        if (e == NULL || (!force && e->frames != 0 && within(e->last_ms, now_ms, OBS_MSG_STALE_MS))) {
            return NULL;
        }
    }
    memset(e, 0, sizeof(*e));
    e->msg = msg;
    return e;
}

/* Mark the profile's numbers in the table, adding the missing ones. */
static void pin_expected(obs_core_t *c)
{
    for (unsigned i = 0; i < c->msg_count; i++) {
        obs_msg_t *e = &c->msg[i];
        e->expected_period_s = 0;
        for (unsigned k = 0; k < c->cfg.expect_count; k++) {
            if (c->cfg.expect[k].msg == e->msg) {
                e->expected_period_s = c->cfg.expect[k].period_s;
                break;
            }
        }
    }
    for (unsigned k = 0; k < c->cfg.expect_count; k++) {
        if (msg_find(c, c->cfg.expect[k].msg) != NULL) {
            continue;
        }
        /* At most OBS_EXPECT_MAX (< OBS_MSG_MAX) entries are pinned, so a forced take always succeeds. */
        obs_msg_t *e = msg_take(c, c->cfg.expect[k].msg, 0, true);
        if (e != NULL) {
            e->expected_period_s = c->cfg.expect[k].period_s;
        }
    }
}

/* ---- sky table ------------------------------------------------------------ */

static obs_sky_entry_t *sky_find(obs_core_t *c, gnss_sys_t sys, uint8_t sat)
{
    for (unsigned i = 0; i < c->sky_count; i++) {
        if (c->sky[i].sys == (uint8_t)sys && c->sky[i].sat == sat) {
            return &c->sky[i];
        }
    }
    return NULL;
}

/* A free entry, or the least recently reported one if it has expired. */
static obs_sky_entry_t *sky_take(obs_core_t *c, uint64_t now_ms)
{
    if (c->sky_count < OBS_SKY_MAX) {
        return &c->sky[c->sky_count++];
    }
    obs_sky_entry_t *e = &c->sky[0];
    for (unsigned i = 1; i < OBS_SKY_MAX; i++) {
        if (c->sky[i].seen_ms < e->seen_ms) {
            e = &c->sky[i];
        }
    }
    return within(e->seen_ms, now_ms, OBS_SKY_WINDOW_MS) ? NULL : e;
}

/* ---- feeds ---------------------------------------------------------------- */

void obs_core_init(obs_core_t *c)
{
    memset(c, 0, sizeof(*c));
}

void obs_core_reset(obs_core_t *c)
{
    obs_core_cfg_t cfg = c->cfg;
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    pin_expected(c);
}

void obs_core_set_expected(obs_core_t *c, const um98x_rtcm_out_t *rtcm, size_t count)
{
    obs_core_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (size_t i = 0; rtcm != NULL && i < count; i++) {
        const uint16_t msg = rtcm[i].msg;
        const uint16_t period_s = rtcm[i].period_s;
        if (msg == 0 || period_s == 0) {
            continue;
        }
        unsigned k = 0;
        while (k < cfg.expect_count && cfg.expect[k].msg != msg) {
            k++;
        }
        if (k < cfg.expect_count) {
            if (period_s < cfg.expect[k].period_s) {
                cfg.expect[k].period_s = period_s;
            }
        } else if (cfg.expect_count < OBS_EXPECT_MAX) {
            cfg.expect[cfg.expect_count++] = rtcm[i];
        } else {
            continue;
        }
        gnss_sys_t sys;
        if (gnss_rtcm3_msm_type(msg, &sys, NULL)) {
            uint32_t p = (uint32_t)period_s * 1000u;
            if (cfg.period_ms[sys] == 0 || p < cfg.period_ms[sys]) {
                cfg.period_ms[sys] = p;
            }
        }
    }
    c->cfg = cfg;
    pin_expected(c);
}

void obs_core_feed_msm(obs_core_t *c, const gnss_rtcm3_msm_t *h, const obs_cnr_t *cnr, uint64_t now_ms)
{
    if ((unsigned)h->sys >= GNSS_SYS_COUNT) {
        return;
    }
    const unsigned s = (unsigned)h->sys;
    const uint8_t bit = (uint8_t)(1u << s);

    for (unsigned i = 0; i < 32; i++) {
        if (h->sig_mask & ((uint32_t)1 << i)) {
            c->sig_seen_ms[s][i] = now_ms;
        }
    }
    c->sig_ever[s] |= h->sig_mask;

    const bool first = !(c->msm_ever & bit);
    if (first || c->epoch_time[s] != h->epoch_time) {
        cnr_finish(c, s, now_ms);
        if (!first) {
            count_gaps(c, s, c->epoch_time[s], h->epoch_time, now_ms);
        }
        c->epoch_time[s] = h->epoch_time;
        c->epoch_sats[s] = h->sat_mask;
    } else {
        c->epoch_sats[s] |= h->sat_mask;
    }
    if (cnr != NULL) {
        cnr_add(c, s, cnr);
    }
    c->msm_seen_ms[s] = now_ms;
    c->msm_ever |= bit;

    if (h->sys == GNSS_SYS_GALILEO &&
        (gnss_rtcm3_msm_band_mask(GNSS_SYS_GALILEO, h->sig_mask) & (1u << GNSS_BAND_L6))) {
        c->e6_seen = true;
    }
    if (!h->multiple_message) {
        c->epochs++;
        c->epoch_end_ms = now_ms;
        c->epoch_ever = true;
        for (unsigned x = 0; x < GNSS_SYS_COUNT; x++) {
            cnr_finish(c, x, now_ms);
        }
    }
}

void obs_core_feed_msg(obs_core_t *c, uint16_t msg, bool has_epoch, uint32_t epoch_time, uint64_t now_ms)
{
    if (msg == 0) {
        return;
    }
    obs_msg_t *e = msg_find(c, msg);
    if (e == NULL) {
        e = msg_take(c, msg, now_ms, false);
        if (e == NULL) {
            if (c->msg_dropped < UINT32_MAX) {
                c->msg_dropped++;
            }
            return;
        }
    }
    e->frames++;
    e->last_ms = now_ms;
    if (has_epoch) {
        if (e->epoch_valid && e->epoch == epoch_time) {
            return;   /* another frame of an epoch already counted */
        }
        e->epoch_valid = true;
        e->epoch = epoch_time;
    }
    if (e->count > 0) {
        uint64_t d = (now_ms > e->counted_ms) ? now_ms - e->counted_ms : 0;
        uint32_t iv = (d > OBS_MSG_INTERVAL_MAX) ? OBS_MSG_INTERVAL_MAX : (uint32_t)d;
        if (e->iv_n == OBS_MSG_INTERVALS) {
            e->iv_sum -= e->iv[e->iv_next];
        } else {
            e->iv_n++;
        }
        e->iv[e->iv_next] = iv;
        e->iv_sum += iv;
        e->iv_next = (uint8_t)((e->iv_next + 1u) % OBS_MSG_INTERVALS);
    }
    e->count++;
    e->counted_ms = now_ms;
}

void obs_core_feed_gsv(obs_core_t *c, const obs_gsv_t *g, uint64_t now_ms)
{
    c->gsv_ever = true;
    c->gsv_ms = now_ms;
    for (unsigned i = 0; i < g->count && i < OBS_GSV_MAX_SATS; i++) {
        const obs_gsv_sat_t *s = &g->sat[i];
        obs_sky_entry_t *e = sky_find(c, s->sys, s->sat);
        if (e == NULL) {
            if (!s->has_pos) {
                continue;   /* nowhere to draw it */
            }
            e = sky_take(c, now_ms);
            if (e == NULL) {
                if (c->sky_dropped < UINT32_MAX) {
                    c->sky_dropped++;
                }
                continue;
            }
            memset(e, 0, sizeof(*e));
            e->sys = (uint8_t)s->sys;
            e->sat = s->sat;
            e->cnr_ms = now_ms;
        }
        if (s->has_pos) {
            e->elevation_deg = s->elevation_deg;
            e->azimuth_deg = s->azimuth_deg;
            e->seen_ms = now_ms;
        }
        if (within(e->cnr_ms, now_ms, OBS_SKY_BURST_MS)) {
            if (s->cnr_dbhz > e->cnr_dbhz) {
                e->cnr_dbhz = s->cnr_dbhz;   /* another signal of the same burst */
            }
        } else {
            e->cnr_dbhz = s->cnr_dbhz;
            e->cnr_ms = now_ms;
        }
    }
}

/* ---- views ---------------------------------------------------------------- */

void obs_core_view(const obs_core_t *c, uint64_t now_ms, obs_core_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->best_sys = GNSS_SYS_GPS;

    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        obs_core_sys_t *v = &out->sys[s];
        v->epoch_gaps = c->sys_gaps[s];
        if (c->cfg.period_ms[s] != 0 && (out->epoch_period_ms == 0 || c->cfg.period_ms[s] < out->epoch_period_ms)) {
            out->epoch_period_ms = c->cfg.period_ms[s];
        }
        if (!(c->msm_ever & (1u << s)) || !within(c->msm_seen_ms[s], now_ms, OBS_SIGNAL_WINDOW_MS)) {
            continue;
        }
        v->present = true;
        v->satellites = (uint8_t)popcount64(c->epoch_sats[s]);
        for (unsigned i = 0; i < 32; i++) {
            if ((c->sig_ever[s] & ((uint32_t)1 << i)) && within(c->sig_seen_ms[s][i], now_ms, OBS_SIGNAL_WINDOW_MS)) {
                v->signal_mask |= (uint32_t)1 << i;
            }
        }
        v->band_mask = gnss_rtcm3_msm_band_mask((gnss_sys_t)s, v->signal_mask);
        v->bands = (uint8_t)popcount64(v->band_mask);
        for (unsigned b = 0; b < GNSS_BAND_COUNT; b++) {
            const uint8_t bb = (uint8_t)(1u << b);
            if ((v->band_mask & bb) && (c->cnr_valid[s] & bb) &&
                within(c->cnr_ms[s][b], now_ms, OBS_SIGNAL_WINDOW_MS)) {
                v->cnr_dbhz[b] = q4_to_dbhz(c->cnr_q4[s][b]);
            }
        }
        if (v->bands > out->best_bands) {
            out->best_bands = v->bands;
            out->best_sys = (gnss_sys_t)s;
        }
    }

    bool e6_now = (out->sys[GNSS_SYS_GALILEO].band_mask & (1u << GNSS_BAND_L6)) != 0;
    out->e6_lost = c->e6_seen && !e6_now;

    out->epochs = c->epochs;
    out->epoch_age_ms = c->epoch_ever ? age_ms(c->epoch_end_ms, now_ms) : OBS_NEVER;
    out->epoch_gaps = c->epoch_gaps;
    out->epoch_jumps = c->epoch_jumps;
    out->msg_dropped = c->msg_dropped;
}

/* Insertion sort of table indices (small tables; only the index bytes move). */
static void msg_sort(const obs_core_t *c, uint8_t *idx, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint8_t x = idx[i];
        size_t j = i;
        while (j > 0 && c->msg[idx[j - 1]].msg > c->msg[x].msg) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = x;
    }
}

/* Sky order: constellation, then satellite number. */
static unsigned sky_key(const obs_sky_entry_t *e)
{
    return ((unsigned)e->sys << 8) | e->sat;
}

static void sky_sort(const obs_core_t *c, uint8_t *idx, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint8_t x = idx[i];
        size_t j = i;
        while (j > 0 && sky_key(&c->sky[idx[j - 1]]) > sky_key(&c->sky[x])) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = x;
    }
}

size_t obs_core_msgs(const obs_core_t *c, uint64_t now_ms, obs_msg_stat_t *out, size_t cap)
{
    uint8_t idx[OBS_MSG_MAX];
    size_t n = 0;
    for (unsigned i = 0; i < c->msg_count && i < OBS_MSG_MAX; i++) {
        idx[n++] = (uint8_t)i;
    }
    msg_sort(c, idx, n);
    if (n > cap) {
        n = cap;
    }
    for (size_t k = 0; k < n; k++) {
        const obs_msg_t *e = &c->msg[idx[k]];
        out[k] = (obs_msg_stat_t) {
            .msg = e->msg,
            .expected_period_s = e->expected_period_s,
            .count = e->count,
            .frames = e->frames,
            .period_ms = e->iv_n ? (e->iv_sum + e->iv_n / 2u) / e->iv_n : OBS_NEVER,
            .age_ms = e->frames ? age_ms(e->last_ms, now_ms) : OBS_NEVER,
        };
    }
    return n;
}

bool obs_core_sky_available(const obs_core_t *c, uint64_t now_ms)
{
    return c->gsv_ever && within(c->gsv_ms, now_ms, OBS_SKY_WINDOW_MS);
}

size_t obs_core_sky(const obs_core_t *c, uint64_t now_ms, obs_sat_t *out, size_t cap)
{
    uint8_t idx[OBS_SKY_MAX];
    size_t n = 0;
    for (unsigned i = 0; i < c->sky_count && i < OBS_SKY_MAX; i++) {
        if (within(c->sky[i].seen_ms, now_ms, OBS_SKY_WINDOW_MS)) {
            idx[n++] = (uint8_t)i;
        }
    }
    sky_sort(c, idx, n);
    if (n > cap) {
        n = cap;
    }
    for (size_t k = 0; k < n; k++) {
        const obs_sky_entry_t *e = &c->sky[idx[k]];
        out[k] = (obs_sat_t) {
            .sys = (gnss_sys_t)e->sys,
            .sat = e->sat,
            .elevation_deg = e->elevation_deg,
            .azimuth_deg = e->azimuth_deg,
            .cnr_dbhz = e->cnr_dbhz,
        };
    }
    return n;
}
