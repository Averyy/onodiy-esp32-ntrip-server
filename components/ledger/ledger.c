/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ledger.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ledger";

/* TODO: pure accounting core (injected clock, day rollover, power-off gap)
 * with host tests, RTC copy of the running counters, NVS persistence. Until
 * then nothing is counted and the budget reports as fully available. */

esp_err_t ledger_init(void)
{
    ESP_LOGI(TAG, "stub: target %d.%d %%, %d days of history", CONFIG_LEDGER_TARGET_PERMILLE / 10,
             CONFIG_LEDGER_TARGET_PERMILLE % 10, CONFIG_LEDGER_HISTORY_DAYS);
    return ESP_OK;
}

void ledger_set_slot(size_t slot, bool enabled, bool protected_slot, bool streaming)
{
    (void)slot;
    (void)enabled;
    (void)protected_slot;
    (void)streaming;
}

void ledger_set_planned(bool planned)
{
    (void)planned;
}

void ledger_set_rtcm_ok(bool ok)
{
    (void)ok;
}

void ledger_set_utc(int64_t unix_s, ledger_clock_t source)
{
    (void)unix_s;
    (void)source;
}

void ledger_count_overflow(void)
{
}

void ledger_tick(void)
{
}

esp_err_t ledger_get_today(ledger_day_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ledger_get_history(ledger_day_t *days, size_t *count)
{
    (void)days;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t ledger_protected_down_today_s(void)
{
    return 0;
}

uint32_t ledger_protected_budget_left_s(void)
{
    return (uint32_t)(86400u * (1000u - CONFIG_LEDGER_TARGET_PERMILLE) / 1000u);
}

int32_t ledger_utc_seconds_of_day(void)
{
    return -1;
}

ledger_clock_t ledger_clock_source(void)
{
    return LEDGER_CLOCK_NONE;
}
