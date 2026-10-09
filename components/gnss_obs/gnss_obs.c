/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_obs.h"

#include <string.h>

/* TODO: MSM header/cell decoder (public RTCM 10403) as a pure core with host
 * tests, message-rate tracking, satellite-in-view parsing. */

esp_err_t gnss_obs_init(void)
{
    return ESP_OK;
}

void gnss_obs_feed_rtcm(const uint8_t *frame, size_t len, uint16_t msg_type)
{
    (void)frame;
    (void)len;
    (void)msg_type;
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
    memset(out, 0, sizeof(*out));
    out->epoch_age_ms = UINT32_MAX;
    return ESP_ERR_NOT_SUPPORTED;
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

void gnss_obs_reset(void)
{
}
