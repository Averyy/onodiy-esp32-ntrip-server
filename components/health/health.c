/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Ladder thresholds and loss accounting are PROVISIONAL until confirmed on
 * hardware. The ladder decision logic goes into a pure core with an injected
 * clock (host-tested across clock wrap and multi-day runs); this file stays
 * the RTOS glue.
 */
#include "health.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lifecycle.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "health";

esp_err_t health_init(void)
{
    /* TODO: event handlers, BOOT button input, ladder state. */
    ESP_LOGI(TAG, "stub: stall after %d s, button %d s / %d s", CONFIG_HEALTH_GNSS_STALL_S,
             CONFIG_HEALTH_BUTTON_ADMIN_S, CONFIG_HEALTH_BUTTON_FACTORY_S);
    return ESP_OK;
}

esp_err_t health_start(void)
{
    /* TODO: 1 Hz monitor task: ledger_tick(), ladders, caster budgets,
     * button, heap watch. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t health_get_status(health_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    out->level = boot.safe_mode ? HEALTH_FAILED : HEALTH_OK;
    if (boot.safe_mode) {
        out->alerts |= HEALTH_ALERT_SAFE_MODE;
    }
    if (boot.brownouts_since_power_on > 0) {
        out->alerts |= HEALTH_ALERT_POWER_SUPPLY;
    }
    if (!settings_is_available()) {
        out->alerts |= HEALTH_ALERT_SETTINGS_DEGRADED;
    }
    out->heap_internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->heap_psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}
