/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_obs_core.h"

#include <string.h>

static unsigned popcount64(uint64_t v)
{
    unsigned n = 0;
    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}

/* Seen within the window ending at now (a stamp from the future counts as now). */
static bool recent(uint64_t seen_ms, uint64_t now_ms)
{
    return now_ms <= seen_ms || now_ms - seen_ms < OBS_SIGNAL_WINDOW_MS;
}

void obs_core_reset(obs_core_t *c)
{
    memset(c, 0, sizeof(*c));
}

void obs_core_feed_msm(obs_core_t *c, const gnss_rtcm3_msm_t *h, uint64_t now_ms)
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

    if (!(c->msm_ever & bit) || c->epoch_time[s] != h->epoch_time) {
        c->epoch_time[s] = h->epoch_time;
        c->epoch_sats[s] = h->sat_mask;
    } else {
        c->epoch_sats[s] |= h->sat_mask;
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
    }
}

void obs_core_view(const obs_core_t *c, uint64_t now_ms, obs_core_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->best_sys = GNSS_SYS_GPS;

    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        obs_core_sys_t *v = &out->sys[s];
        if (!(c->msm_ever & (1u << s)) || !recent(c->msm_seen_ms[s], now_ms)) {
            continue;
        }
        v->present = true;
        v->satellites = (uint8_t)popcount64(c->epoch_sats[s]);
        for (unsigned i = 0; i < 32; i++) {
            if ((c->sig_ever[s] & ((uint32_t)1 << i)) && recent(c->sig_seen_ms[s][i], now_ms)) {
                v->signal_mask |= (uint32_t)1 << i;
            }
        }
        v->band_mask = gnss_rtcm3_msm_band_mask((gnss_sys_t)s, v->signal_mask);
        v->bands = (uint8_t)popcount64(v->band_mask);
        if (v->bands > out->best_bands) {
            out->best_bands = v->bands;
            out->best_sys = (gnss_sys_t)s;
        }
    }

    bool e6_now = (out->sys[GNSS_SYS_GALILEO].band_mask & (1u << GNSS_BAND_L6)) != 0;
    out->e6_lost = c->e6_seen && !e6_now;

    out->epochs = c->epochs;
    if (!c->epoch_ever) {
        out->epoch_age_ms = UINT32_MAX;
    } else if (now_ms <= c->epoch_end_ms) {
        out->epoch_age_ms = 0;
    } else {
        uint64_t age = now_ms - c->epoch_end_ms;
        out->epoch_age_ms = (age >= UINT32_MAX) ? UINT32_MAX - 1u : (uint32_t)age;
    }
}
