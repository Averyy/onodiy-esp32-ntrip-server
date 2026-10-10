/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTOS glue around the observation core: the clock, the lock and the PSRAM
 * state. Decoding (MSM header and cells, satellite-in-view sentences, 1005)
 * runs in the caller's task before the lock is taken.
 */
#include "gnss_obs.h"

#include <assert.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gnss_obs_core.h"
#include "gnss_obs_parse.h"

static_assert(GNSS_BAND_COUNT <= sizeof(((gnss_obs_sys_t *)0)->cnr_dbhz), "one CNR slot per band");
static_assert(GNSS_OBS_MAX_MSG_TYPES == OBS_MSG_MAX, "message table size");
static_assert(GNSS_OBS_MAX_SKY == OBS_SKY_MAX, "sky table size");
static_assert(OBS_EXPECT_MAX == UM98X_MAX_RTCM_MSGS, "every profile message fits");

/* Everything below is guarded by s_lock. The core is zero-initialised at
 * boot (the reset state without a profile), so getters work before init. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR obs_core_t s_core;
static EXT_RAM_BSS_ATTR obs_msg_stat_t s_msg_tmp[OBS_MSG_MAX];
static EXT_RAM_BSS_ATTR obs_sat_t s_sky_tmp[OBS_SKY_MAX];
static gnss_rtcm3_arp_t s_arp;
static bool s_arp_valid;
static int64_t s_arp_us;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

esp_err_t gnss_obs_init(void)
{
    gnss_obs_reset();
    return ESP_OK;
}

void gnss_obs_feed_rtcm(const uint8_t *frame, size_t len, uint16_t msg_type)
{
    /* GNSS RX task: decode outside the lock, never block, never log. */
    gnss_rtcm3_msm_t h = { 0 };
    obs_cnr_t cnr = { 0 };
    gnss_rtcm3_arp_t arp = { 0 };
    const bool msm = gnss_rtcm3_msm_type(msg_type, NULL, NULL) && gnss_rtcm3_msm_decode(frame, len, &h);
    const bool has_cnr = msm && obs_msm_cnr(frame, len, &h, &cnr, NULL);
    const bool has_arp = (msg_type == 1005 || msg_type == 1006) && gnss_rtcm3_arp_decode(frame, len, &arp);
    const int64_t t_us = esp_timer_get_time();
    const uint64_t t = (uint64_t)t_us / 1000u;

    portENTER_CRITICAL(&s_lock);
    obs_core_feed_msg(&s_core, msg_type, msm, msm ? h.epoch_time : 0, t);
    if (msm) {
        obs_core_feed_msm(&s_core, &h, has_cnr ? &cnr : NULL, t);
    }
    if (has_arp) {
        s_arp = arp;
        s_arp_valid = true;
        s_arp_us = t_us;
    }
    portEXIT_CRITICAL(&s_lock);
}

void gnss_obs_feed_line(const char *line, size_t len)
{
    obs_gsv_t g;
    if (!obs_gsv_parse(line, len, &g)) {
        return;
    }
    uint64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    obs_core_feed_gsv(&s_core, &g, t);
    portEXIT_CRITICAL(&s_lock);
}

void gnss_obs_set_expected(const um98x_rtcm_out_t *rtcm, size_t count)
{
    portENTER_CRITICAL(&s_lock);
    obs_core_set_expected(&s_core, rtcm, count);
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t gnss_obs_get_summary(gnss_obs_summary_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    obs_core_view_t v;
    uint64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    obs_core_view(&s_core, t, &v);
    portEXIT_CRITICAL(&s_lock);

    memset(out, 0, sizeof(*out));
    for (unsigned s = 0; s < GNSS_SYS_COUNT; s++) {
        gnss_obs_sys_t *o = &out->sys[s];
        o->present = v.sys[s].present;
        o->satellites = v.sys[s].satellites;
        o->signal_mask = v.sys[s].signal_mask;
        o->band_mask = v.sys[s].band_mask;
        o->bands = v.sys[s].bands;
        memcpy(o->cnr_dbhz, v.sys[s].cnr_dbhz, sizeof(v.sys[s].cnr_dbhz));
        o->epoch_gaps = v.sys[s].epoch_gaps;
    }
    out->best_sys = v.best_sys;
    out->best_bands = v.best_bands;
    out->e6_lost = v.e6_lost;
    out->epoch_age_ms = v.epoch_age_ms;
    out->epochs = v.epochs;
    out->epoch_gaps = v.epoch_gaps;
    out->epoch_jumps = v.epoch_jumps;
    out->epoch_period_ms = v.epoch_period_ms;
    return ESP_OK;
}

esp_err_t gnss_obs_get_msg_stats(gnss_obs_msg_stat_t *out, size_t *count)
{
    if (count == NULL || (out == NULL && *count != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t cap = (*count < OBS_MSG_MAX) ? *count : OBS_MSG_MAX;
    uint64_t t = now_ms();
    /* The scratch table is shared: fill and copy out under the lock. */
    portENTER_CRITICAL(&s_lock);
    size_t n = obs_core_msgs(&s_core, t, s_msg_tmp, cap);
    for (size_t i = 0; i < n; i++) {
        const obs_msg_stat_t *m = &s_msg_tmp[i];
        out[i] = (gnss_obs_msg_stat_t) {
            .msg = m->msg,
            .expected_period_s = m->expected_period_s,
            .count = m->count,
            .frames = m->frames,
            .period_ms = m->period_ms,
            .age_ms = m->age_ms,
        };
    }
    portEXIT_CRITICAL(&s_lock);
    *count = n;
    return ESP_OK;
}

esp_err_t gnss_obs_get_sky(gnss_obs_sat_t *out, size_t *count)
{
    if (count == NULL || (out == NULL && *count != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t cap = (*count < OBS_SKY_MAX) ? *count : OBS_SKY_MAX;
    uint64_t t = now_ms();
    size_t n = 0;
    portENTER_CRITICAL(&s_lock);
    bool available = obs_core_sky_available(&s_core, t);
    if (available) {
        n = obs_core_sky(&s_core, t, s_sky_tmp, cap);
        for (size_t i = 0; i < n; i++) {
            const obs_sat_t *s = &s_sky_tmp[i];
            out[i] = (gnss_obs_sat_t) {
                .sys = s->sys,
                .prn = s->sat,
                .elevation_deg = s->elevation_deg,
                .azimuth_deg = s->azimuth_deg,
                .cnr_dbhz = s->cnr_dbhz,
            };
        }
    }
    portEXIT_CRITICAL(&s_lock);
    *count = n;
    return available ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t gnss_obs_get_arp(gnss_obs_arp_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->age_ms = UINT32_MAX;

    gnss_rtcm3_arp_t arp;
    bool valid;
    int64_t at_us;
    portENTER_CRITICAL(&s_lock);
    arp = s_arp;
    valid = s_arp_valid;
    at_us = s_arp_us;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_NOT_FOUND;
    }

    out->valid = true;
    out->station_id = arp.station_id;
    memcpy(out->ecef_01mm, arp.ecef_01mm, sizeof(out->ecef_01mm));
    int64_t age_ms = (esp_timer_get_time() - at_us) / 1000;
    if (age_ms < 0) {
        age_ms = 0;
    }
    out->age_ms = (age_ms >= (int64_t)UINT32_MAX) ? UINT32_MAX - 1u : (uint32_t)age_ms;
    return ESP_OK;
}

void gnss_obs_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    obs_core_reset(&s_core);
    memset(&s_arp, 0, sizeof(s_arp));
    s_arp_valid = false;
    s_arp_us = 0;
    portEXIT_CRITICAL(&s_lock);
}
