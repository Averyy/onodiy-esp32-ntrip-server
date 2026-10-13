/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTOS wrapper around the hub core (rtcm_hub_core.c): PSRAM buffers, the
 * spinlock, the clock and consumer wake-ups. The publish path never blocks,
 * allocates or logs.
 */
#include "rtcm_hub.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_framing.h"
#include "rtcm_hub_core.h"
#include "sdkconfig.h"

static const char *TAG = "rtcm_hub";

/* Consumer wake-up; index 0 belongs to IDF APIs. */
#define HUB_NOTIFY_INDEX 1

_Static_assert(RTCM_HUB_MIN_READ_BUF == GNSS_RTCM3_MAX_FRAME, "hub read minimum must equal one maximum RTCM3 frame");
_Static_assert(CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES > HUB_NOTIFY_INDEX,
               "consumer wake-up uses task notification index 1");
_Static_assert(CONFIG_RTCM_HUB_MAX_CONSUMERS <= RTCM_CORE_MAX_CONSUMERS, "too many hub consumers");
_Static_assert((size_t)CONFIG_RTCM_HUB_RING_KB * 1024u >= RTCM_CORE_MIN_RING, "hub ring too small");
_Static_assert(CONFIG_RTCM_HUB_MAX_LAG_MS >= RTCM_CORE_LAG_MIN_MS && CONFIG_RTCM_HUB_MAX_LAG_MS <= RTCM_CORE_LAG_MAX_MS,
               "default lag limit out of range");

/* Public handles map 1:1 onto the core's consumer slots. */
struct rtcm_hub_consumer {
    rtcm_core_consumer_t *core;
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static rtcm_core_t *s_core;                 /* set once (under s_lock) by rtcm_hub_init */
static uint64_t s_early_failures;           /* under s_lock: publishes before init */
static struct rtcm_hub_consumer s_handles[CONFIG_RTCM_HUB_MAX_CONSUMERS];

static void hub_lock(void *ctx)
{
    portENTER_CRITICAL((portMUX_TYPE *)ctx);
}

static void hub_unlock(void *ctx)
{
    portEXIT_CRITICAL((portMUX_TYPE *)ctx);
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static rtcm_core_consumer_t *lookup(rtcm_hub_consumer_t h)
{
    if (s_core == NULL || h == NULL) {
        return NULL;
    }
    uintptr_t base = (uintptr_t)&s_handles[0];
    uintptr_t p = (uintptr_t)h;
    if (p < base || p >= base + sizeof(s_handles) || (p - base) % sizeof(s_handles[0]) != 0) {
        return NULL;
    }
    return h->core;
}

esp_err_t rtcm_hub_init(void)
{
    if (s_core != NULL) {
        return ESP_OK;
    }
    const size_t ring_len = (size_t)CONFIG_RTCM_HUB_RING_KB * 1024u;
    uint8_t *ring = heap_caps_malloc(ring_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rtcm_core_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ring == NULL || c == NULL) {
        heap_caps_free(ring);
        heap_caps_free(c);
        ESP_LOGE(TAG, "no PSRAM for the %d KB ring", CONFIG_RTCM_HUB_RING_KB);
        return ESP_ERR_NO_MEM;
    }
    const rtcm_core_cfg_t cfg = {
        .gap_ms = CONFIG_RTCM_HUB_BURST_GAP_MS,
        .default_lag_ms = CONFIG_RTCM_HUB_MAX_LAG_MS,
        .max_consumers = CONFIG_RTCM_HUB_MAX_CONSUMERS,
        .first_epoch = 1,
    };
    const rtcm_core_lock_t lock = { .lock = hub_lock, .unlock = hub_unlock, .ctx = &s_lock };
    if (!rtcm_core_init(c, ring, ring_len, &cfg, &lock)) {
        heap_caps_free(ring);
        heap_caps_free(c);
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < CONFIG_RTCM_HUB_MAX_CONSUMERS; i++) {
        s_handles[i].core = &c->cons[i];
    }
    portENTER_CRITICAL(&s_lock);
    c->publish_failures = s_early_failures;
    s_core = c;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "ring %d KB in PSRAM, %d consumers max, lag limit %d ms, burst gap %d ms",
             CONFIG_RTCM_HUB_RING_KB, CONFIG_RTCM_HUB_MAX_CONSUMERS, CONFIG_RTCM_HUB_MAX_LAG_MS,
             CONFIG_RTCM_HUB_BURST_GAP_MS);
    return ESP_OK;
}

esp_err_t rtcm_hub_publish(const uint8_t *frame, size_t len, uint16_t msg_type, int64_t arrival_us,
                           bool arrival_exact)
{
    rtcm_core_t *c = s_core;
    if (c == NULL) {
        portENTER_CRITICAL(&s_lock);
        if (s_core != NULL) {
            s_core->publish_failures++;
        } else {
            s_early_failures++;
        }
        portEXIT_CRITICAL(&s_lock);
        return len > GNSS_RTCM3_MAX_FRAME ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    void *wake[RTCM_CORE_MAX_CONSUMERS];
    size_t n = 0;
    uint64_t arrival_ms = arrival_us > 0 ? (uint64_t)(arrival_us / 1000) : 0;
    if (!rtcm_core_publish(c, frame, len, msg_type, arrival_ms, arrival_exact, wake, &n)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n > 0) {
        /* Outside the lock; a consumer detaching meanwhile waits for
         * rtcm_core_wake_done() before its task may go away. */
        for (size_t i = 0; i < n; i++) {
            xTaskNotifyGiveIndexed((TaskHandle_t)wake[i], HUB_NOTIFY_INDEX);
        }
        rtcm_core_wake_done(c);
    }
    return ESP_OK;
}

esp_err_t rtcm_hub_attach(const char *name, rtcm_hub_consumer_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    rtcm_core_t *c = s_core;
    if (c == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    rtcm_core_consumer_t *k = rtcm_core_attach(c, name != NULL ? name : "?");
    if (k == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out = &s_handles[k - c->cons];
    ESP_LOGI(TAG, "consumer \"%s\" attached", k->name);
    return ESP_OK;
}

void rtcm_hub_detach(rtcm_hub_consumer_t consumer)
{
    rtcm_core_consumer_t *k = lookup(consumer);
    if (k == NULL) {
        return;
    }
    char name[RTCM_CORE_NAME_LEN];
    memcpy(name, k->name, sizeof(name));
    uint32_t gen;
    if (rtcm_core_detach(s_core, k, &gen)) {
        /* The producer may be notifying a copy of our task handle right now. */
        while (!rtcm_core_wake_passed(s_core, gen)) {
            vTaskDelay(1);
        }
    }
    ESP_LOGI(TAG, "consumer \"%.*s\" detached", (int)sizeof(name), name);
}

esp_err_t rtcm_hub_read(rtcm_hub_consumer_t consumer, uint8_t *buf, size_t max_len,
                        size_t *out_len, uint32_t timeout_ms)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (max_len < RTCM_HUB_MIN_READ_BUF) {
        return ESP_ERR_INVALID_SIZE;
    }
    rtcm_core_consumer_t *k = lookup(consumer);
    if (k == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rtcm_core_t *c = s_core;
    const int64_t start_us = esp_timer_get_time();
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (;;) {
        size_t n = rtcm_core_read(c, k, buf, max_len, now_ms());
        if (n > 0) {
            *out_len = n;
            return ESP_OK;
        }
        uint64_t elapsed_ms = (uint64_t)(esp_timer_get_time() - start_us) / 1000u;
        if (elapsed_ms >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        if (!rtcm_core_prepare_wait(c, k, self)) {
            continue;   /* something arrived since the read */
        }
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms - (uint32_t)elapsed_ms);
        (void)ulTaskNotifyTakeIndexed(HUB_NOTIFY_INDEX, pdTRUE, ticks > 0 ? ticks : 1);
        rtcm_core_cancel_wait(c, k);
    }
}

esp_err_t rtcm_hub_resync(rtcm_hub_consumer_t consumer)
{
    rtcm_core_consumer_t *k = lookup(consumer);
    if (k == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rtcm_core_resync(s_core, k);
    return ESP_OK;
}

esp_err_t rtcm_hub_set_max_lag(rtcm_hub_consumer_t consumer, uint32_t ms)
{
    rtcm_core_consumer_t *k = lookup(consumer);
    if (k == NULL || !rtcm_core_set_max_lag(s_core, k, ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void rtcm_hub_clear_sticky(void)
{
    rtcm_core_t *c = s_core;
    if (c != NULL) {
        rtcm_core_clear_sticky(c);
    }
}

void rtcm_hub_get_stats(rtcm_hub_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    rtcm_core_t *c = s_core;
    if (c == NULL) {
        out->capacity = (size_t)CONFIG_RTCM_HUB_RING_KB * 1024u;
        out->last_frame_age_ms = UINT32_MAX;
        portENTER_CRITICAL(&s_lock);
        out->publish_failures = s_early_failures;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    rtcm_core_stats_t st;
    rtcm_core_get_stats(c, now_ms(), &st);
    out->frames_in = st.frames_in;
    out->bytes_in = st.bytes_in;
    out->epochs_in = st.epochs_in;
    out->epochs_by_mmb = st.epochs_by_mmb;
    out->epochs_by_time = st.epochs_by_time;
    out->epochs_by_gap = st.epochs_by_gap;
    out->publish_failures = st.publish_failures;
    out->consumers = st.consumers;
    out->capacity = st.capacity;
    out->last_frame_age_ms = st.last_frame_age_ms;
}

bool rtcm_hub_epoch_in_flight(uint32_t quiet_ms, uint32_t *age_ms)
{
    rtcm_core_t *c = s_core;
    if (c == NULL) {
        if (age_ms != NULL) {
            *age_ms = UINT32_MAX;
        }
        return false;
    }
    return rtcm_core_epoch_in_flight(c, now_ms(), quiet_ms, age_ms);
}

esp_err_t rtcm_hub_get_consumer_stats(rtcm_hub_consumer_t consumer, rtcm_hub_consumer_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    rtcm_core_consumer_t *k = lookup(consumer);
    if (k == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    rtcm_core_consumer_stats_t st;
    rtcm_core_get_consumer_stats(s_core, k, now_ms(), &st);
    out->frames_out = st.c.frames_out;
    out->bytes_out = st.c.bytes_out;
    out->frames_dropped = st.c.frames_dropped;
    out->bytes_dropped = st.c.bytes_dropped;
    out->frames_skipped_idle = st.c.frames_skipped_idle;
    out->epochs_dropped = st.c.epochs_dropped;
    out->epochs_out = st.c.epochs_out;
    out->max_lag_ms = st.max_lag_ms;
    out->lag_drops = st.c.lag_drops;
    out->laps = st.c.laps;
    out->lag_ms = st.lag_ms;
    return ESP_OK;
}
