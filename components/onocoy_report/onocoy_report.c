/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "onocoy_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network.h"
#include "network_tls.h"
#include "report_core.h"
#include "sdkconfig.h"

_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= CONFIG_NTRIP_MAX_CASTERS + 25 &&
               CONFIG_LWIP_MAX_ACTIVE_TCP >= CONFIG_NTRIP_MAX_CASTERS + 24,
               "the optional report needs room alongside the stream: update the socket limits from sdkconfig.defaults");

#define BODY_MAX 65536u
#define US_PER_S 1000000LL

static const char *TAG = "onocoy_report";
static SemaphoreHandle_t s_mux;
static TaskHandle_t s_task;
static onocoy_report_status_t s_status;
static ntrip_caster_cfg_t s_credential;
static size_t s_slot;
static uint32_t s_generation;
static int64_t s_due_us, s_manual_after_us;
static unsigned s_failures;
static bool s_requested;
static network_tls_verify_t s_verify;

static void wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}

/* Only this fixed HTTPS origin ever receives the opted-in credentials. */
static bool current(ntrip_caster_cfg_t *out, size_t *slot)
{
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        if (ntrip_server_get_config(i, out, true) == ESP_OK && report_eligible(out)) {
            *slot = i;
            return true;
        }
        wipe(out, sizeof(*out));
    }
    return false;
}

/* Called by readers as well as the worker: disabling or changing credentials
 * immediately hides old data, including while an old request is in flight. */
static void sync_config(void)
{
    ntrip_caster_cfg_t c = {0};
    size_t slot = 0;
    bool enabled = current(&c, &slot);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = enabled != s_status.enabled || (enabled &&
        (slot != s_slot || strcmp(c.host, s_credential.host) != 0 ||
         strcmp(c.username, s_credential.username) != 0 || strcmp(c.password, s_credential.password) != 0));
    if (changed) {
        wipe(&s_credential, sizeof(s_credential));
        memset(&s_status, 0, sizeof(s_status));
        s_status.enabled = enabled;
        s_status.state = enabled ? ONOCOY_REPORT_PENDING : ONOCOY_REPORT_OFF;
        if (enabled) s_credential = c;
        s_slot = slot;
        s_generation++;
        s_due_us = 0;
        s_manual_after_us = 0;
        s_failures = 0;
        s_requested = enabled;
    }
    xSemaphoreGive(s_mux);
    wipe(&c, sizeof(c));
}

void onocoy_report_config_changed(void)
{
    if (s_mux == NULL) return;
    sync_config();
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

void onocoy_report_get_status(onocoy_report_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_mux == NULL) return;
    sync_config();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_status;
    int64_t left = s_due_us - esp_timer_get_time();
    out->refresh_in_s = out->enabled && left > 0 ? (uint32_t)((left + US_PER_S - 1) / US_PER_S) : 0;
    left = s_manual_after_us - esp_timer_get_time();
    out->manual_in_s = out->enabled && left > 0 ? (uint32_t)((left + US_PER_S - 1) / US_PER_S) : 0;
    xSemaphoreGive(s_mux);
}

esp_err_t onocoy_report_refresh(void)
{
    if (s_mux == NULL) return ESP_ERR_INVALID_STATE;
    sync_config();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_status.enabled) err = ESP_ERR_INVALID_STATE;
    else if (s_status.busy || s_requested) err = ESP_ERR_NOT_FINISHED;
    else if (!report_manual_allowed(s_status.state)) err = ESP_ERR_NOT_ALLOWED;
    else if (esp_timer_get_time() < s_manual_after_us) err = ESP_ERR_NOT_FINISHED;
    else { s_requested = true; s_status.state = ONOCOY_REPORT_PENDING; }
    xSemaphoreGive(s_mux);
    if (err == ESP_OK && s_task != NULL) xTaskNotifyGive(s_task);
    return err;
}

typedef struct { char *body; size_t len; bool overflow; } response_t;

static esp_err_t event(esp_http_client_event_t *e)
{
    response_t *r = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || e->data_len <= 0) return ESP_OK;
    if ((size_t)e->data_len > BODY_MAX - r->len) {
        r->overflow = true;
        return ESP_FAIL;
    }
    memcpy(r->body + r->len, e->data, (size_t)e->data_len);
    r->len += (size_t)e->data_len;
    r->body[r->len] = '\0';
    return ESP_OK;
}

static esp_err_t attach(void *conf) { return network_tls_bundle_attach(conf, &s_verify); }

static bool download(const ntrip_caster_cfg_t *c, const char *start, const char *end,
                     onocoy_report_day_t *days, size_t *count, int *status)
{
    /* Onocoy usernames are validated as [A-Za-z0-9._-]; no URL escaping is needed. */
    char url[256];
    snprintf(url, sizeof(url), "https://api-hub.onocoy.com/api/v1/my_stats/stations?start_date=%s&end_date=%s&username=%s",
             start, end, c->username);
    response_t r = { .body = heap_caps_malloc(BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) };
    if (r.body == NULL) return false;
    r.body[0] = '\0';
    esp_http_client_config_t conf = {
        .url = url, .username = c->username, .password = c->password,
        .auth_type = HTTP_AUTH_TYPE_BASIC, .max_authorization_retries = -1,
        .disable_auto_redirect = true, .timeout_ms = 10000,
        .crt_bundle_attach = attach, .buffer_size = 2048, .buffer_size_tx = 1024,
        .user_agent = "onodiy", .event_handler = event, .user_data = &r,
    };
    esp_http_client_handle_t client = esp_http_client_init(&conf);
    bool ok = false;
    if (client != NULL) {
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_err_t err = esp_http_client_perform(client);
        *status = esp_http_client_get_status_code(client);
        ok = err == ESP_OK && *status == 200 && !r.overflow &&
             esp_http_client_is_complete_data_received(client) &&
             report_parse(r.body, r.len, start, end, days, count);
        esp_http_client_cleanup(client);
    }
    free(r.body);
    return ok;
}

static bool dates(time_t now, char start[11], char end[11])
{
    struct tm tm;
    if (gmtime_r(&now, &tm) == NULL || strftime(end, 11, "%Y-%m-%d", &tm) != 10) return false;
    now -= (time_t)ONOCOY_REPORT_DAYS * 86400;
    return gmtime_r(&now, &tm) != NULL && strftime(start, 11, "%Y-%m-%d", &tm) == 10;
}

static void worker(void *arg)
{
    (void)arg;
    onocoy_report_day_t *days = heap_caps_calloc(ONOCOY_REPORT_DAYS, sizeof(*days), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        sync_config();
        ntrip_caster_cfg_t c = {0};
        uint32_t gen;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        bool due = s_status.enabled && (s_requested || esp_timer_get_time() >= s_due_us);
        gen = s_generation;
        if (due) c = s_credential;
        xSemaphoreGive(s_mux);
        if (due) {
            if (days == NULL) days = heap_caps_calloc(ONOCOY_REPORT_DAYS, sizeof(*days), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            network_status_t ns;
            time_t now = time(NULL);
            bool online = network_is_online();
            bool clock = network_get_status(&ns) == ESP_OK && ns.time_synced && now >= 1704067200;
            if (!online || !clock) {
                xSemaphoreTake(s_mux, portMAX_DELAY);
                if (gen == s_generation) s_status.state = !online ? ONOCOY_REPORT_WAIT_NETWORK : ONOCOY_REPORT_WAIT_TIME;
                xSemaphoreGive(s_mux);
            } else {
                xSemaphoreTake(s_mux, portMAX_DELAY);
                if (gen == s_generation) { s_status.busy = true; s_requested = false; }
                xSemaphoreGive(s_mux);
                char start[11], end[11];
                size_t count = 0;
                int status = 0;
                bool ok = days != NULL && dates(now, start, end) && download(&c, start, end, days, &count, &status);
                sync_config();
                xSemaphoreTake(s_mux, portMAX_DELAY);
                if (gen == s_generation && s_status.enabled) {
                    s_status.busy = false;
                    s_status.http_status = status;
                    s_status.state = ok ? (count ? ONOCOY_REPORT_OK : ONOCOY_REPORT_EMPTY) :
                                     (status == 401 || status == 403 ? ONOCOY_REPORT_AUTH : ONOCOY_REPORT_ERROR);
                    if (ok) {
                        s_status.checked_unix = (int64_t)time(NULL);
                        s_status.count = count;
                        memcpy(s_status.days, days, count * sizeof(*days));
                    }
                    uint32_t delay = report_retry_s(ok, status, s_failures);
                    s_failures = ok ? 0 : (s_failures < 4 ? s_failures + 1 : 4);
                    int64_t t = esp_timer_get_time();
                    s_due_us = t + (int64_t)delay * US_PER_S;
                    s_manual_after_us = t + (status == 401 || status == 403 ? 300LL : 60LL) * US_PER_S;
                }
                xSemaphoreGive(s_mux);
                ESP_LOGI(TAG, "daily report: %s (HTTP %d)", ok ? "downloaded" : "unavailable", status);
            }
        }
        wipe(&c, sizeof(c));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

esp_err_t onocoy_report_start(void)
{
    if (s_mux != NULL) return ESP_ERR_INVALID_STATE;
    s_mux = xSemaphoreCreateMutex();
    if (s_mux == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(worker, "onocoy_report", 8192, NULL, tskIDLE_PRIORITY + 1,
                                      &s_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(s_mux); s_mux = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
