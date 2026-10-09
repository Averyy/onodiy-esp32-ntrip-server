/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "network.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "network";

ESP_EVENT_DEFINE_BASE(NETWORK_EVENT);

esp_err_t network_init(void)
{
    /* TODO: esp_netif_create_default_wifi_sta/ap, esp_wifi_init with
     * WIFI_STORAGE_RAM, event handlers, hostname from settings, country code,
     * power save off, all-channel scan sorted by signal. */
    ESP_LOGI(TAG, "stub");
    return ESP_OK;
}

esp_err_t network_start(void)
{
    /* TODO: STA if credentials stored, else setup AP + captive DNS; no AP at
     * all while settings are unavailable. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_get_status(network_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

bool network_is_online(void)
{
    return false;
}

esp_err_t network_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_scan_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_get_scan_results(network_scan_entry_t *entries, size_t max_entries, size_t *out_count)
{
    (void)entries;
    (void)max_entries;
    if (out_count != NULL) {
        *out_count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_start_setup_ap(bool recovery)
{
    (void)recovery;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_request_recovery(network_recovery_t action)
{
    (void)action;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t network_probe_upstream(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
