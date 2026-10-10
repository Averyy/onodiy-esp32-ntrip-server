/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "rtcm_hub.h"

#include <string.h>

#include "esp_log.h"
#include "gnss_framing.h"
#include "sdkconfig.h"

static const char *TAG = "rtcm_hub";

_Static_assert(RTCM_HUB_MIN_READ_BUF == GNSS_RTCM3_MAX_FRAME, "hub read minimum must equal one maximum RTCM3 frame");
_Static_assert(CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES >= 2,
               "consumer wake-up uses task notification index 1");

esp_err_t rtcm_hub_init(void)
{
    /* TODO: allocate CONFIG_RTCM_HUB_RING_KB in PSRAM; records, reserve/write
     * offsets, epoch tagging and sticky metadata as described in rtcm_hub.h. */
    ESP_LOGI(TAG, "stub: ring %d KB, %d consumers max (not allocated yet)",
             CONFIG_RTCM_HUB_RING_KB, CONFIG_RTCM_HUB_MAX_CONSUMERS);
    return ESP_OK;
}

esp_err_t rtcm_hub_publish(const uint8_t *frame, size_t len, uint16_t msg_type)
{
    (void)frame;
    (void)msg_type;
    if (len > GNSS_RTCM3_MAX_FRAME) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t rtcm_hub_attach(const char *name, rtcm_hub_consumer_t *out)
{
    (void)name;
    if (out != NULL) {
        *out = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void rtcm_hub_detach(rtcm_hub_consumer_t consumer)
{
    (void)consumer;
}

esp_err_t rtcm_hub_read(rtcm_hub_consumer_t consumer, uint8_t *buf, size_t max_len,
                        size_t *out_len, uint32_t timeout_ms)
{
    (void)consumer;
    (void)buf;
    (void)timeout_ms;
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (max_len < RTCM_HUB_MIN_READ_BUF) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtcm_hub_resync(rtcm_hub_consumer_t consumer)
{
    (void)consumer;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtcm_hub_set_max_lag(rtcm_hub_consumer_t consumer, uint32_t ms)
{
    if (consumer == NULL || ms < 100 || ms > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void rtcm_hub_clear_sticky(void)
{
    /* TODO: drop the sticky copies and mark the next frame as an epoch start. */
}

void rtcm_hub_get_stats(rtcm_hub_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->capacity = (size_t)CONFIG_RTCM_HUB_RING_KB * 1024u;
    out->last_frame_age_ms = UINT32_MAX;
}

esp_err_t rtcm_hub_get_consumer_stats(rtcm_hub_consumer_t consumer, rtcm_hub_consumer_stats_t *out)
{
    (void)consumer;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}
