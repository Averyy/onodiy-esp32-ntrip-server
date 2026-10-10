/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_obs.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gnss_obs_core.h"

/* TODO: per-band CNR from MSM7 cells, epoch gaps against the configured
 * period, message-rate tracking and satellite-in-view parsing (pure core,
 * host-tested like gnss_obs_core). */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static obs_core_t s_core;            /* zero = reset state */
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
    if (gnss_rtcm3_msm_type(msg_type, NULL, NULL)) {
        gnss_rtcm3_msm_t h;
        if (gnss_rtcm3_msm_decode(frame, len, &h)) {
            uint64_t t = now_ms();
            portENTER_CRITICAL(&s_lock);
            obs_core_feed_msm(&s_core, &h, t);
            portEXIT_CRITICAL(&s_lock);
        }
    } else if (msg_type == 1005 || msg_type == 1006) {
        gnss_rtcm3_arp_t arp;
        if (gnss_rtcm3_arp_decode(frame, len, &arp)) {
            int64_t t = esp_timer_get_time();
            portENTER_CRITICAL(&s_lock);
            s_arp = arp;
            s_arp_valid = true;
            s_arp_us = t;
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

void gnss_obs_feed_line(const char *line, size_t len)
{
    (void)line;
    (void)len;
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
        out->sys[s].present = v.sys[s].present;
        out->sys[s].satellites = v.sys[s].satellites;
        out->sys[s].signal_mask = v.sys[s].signal_mask;
        out->sys[s].band_mask = v.sys[s].band_mask;
        out->sys[s].bands = v.sys[s].bands;
    }
    out->best_sys = v.best_sys;
    out->best_bands = v.best_bands;
    out->e6_lost = v.e6_lost;
    out->epoch_age_ms = v.epoch_age_ms;
    out->epochs = v.epochs;
    out->epoch_gaps = 0;
    return ESP_OK;
}

esp_err_t gnss_obs_get_msg_stats(gnss_obs_msg_stat_t *out, size_t *count)
{
    (void)out;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_obs_get_sky(gnss_obs_sat_t *out, size_t *count)
{
    (void)out;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
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
