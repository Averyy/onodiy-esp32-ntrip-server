/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * PROVISIONAL stub. Driver buffer sizes, event handling, RX task timing, loss
 * counters and the RAW-mode details are still to be confirmed on hardware.
 */
#include "gnss_link.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "gnss_link";

static gnss_link_sinks_t s_sinks;

esp_err_t gnss_link_init(void)
{
    if (!board_is_valid()) {
        /* A rejected pin map may point the UART at flash or PSRAM lines. */
        ESP_LOGE(TAG, "board pin map not validated: GNSS UART left closed");
        return ESP_ERR_INVALID_STATE;
    }
    const board_desc_t *b = board_get();
    /* TODO: uart_driver_install + uart_param_config + uart_set_pin with an
     * event queue for overflow/buffer-full/error accounting. */
    ESP_LOGI(TAG, "stub: UART%d RX GPIO%d TX GPIO%d @ %d baud (not opened yet)", b->gnss_uart_num,
             b->gnss_rx_gpio, b->gnss_tx_gpio, CONFIG_GNSS_LINK_DEFAULT_BAUD);
    return ESP_OK;
}

esp_err_t gnss_link_set_sinks(const gnss_link_sinks_t *sinks)
{
    if (sinks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_sinks = *sinks;
    return ESP_OK;
}

esp_err_t gnss_link_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

gnss_link_mode_t gnss_link_get_mode(void)
{
    return GNSS_LINK_MODE_OFF;
}

esp_err_t gnss_link_acquire(gnss_link_mode_t mode, const char *owner, uint32_t timeout_ms,
                            gnss_link_lease_t *out)
{
    (void)owner;
    (void)timeout_ms;
    if (out != NULL) {
        *out = NULL;
    }
    if (mode != GNSS_LINK_MODE_COMMAND && mode != GNSS_LINK_MODE_RAW) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_release(gnss_link_lease_t lease)
{
    (void)lease;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_set_baud(gnss_link_lease_t lease, uint32_t baud)
{
    (void)lease;
    (void)baud;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_send_break(gnss_link_lease_t lease, uint32_t duration_ms)
{
    (void)duration_ms;
    if (lease == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_restart(gnss_link_lease_t lease)
{
    (void)lease;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t gnss_link_get_baud(void)
{
    return CONFIG_GNSS_LINK_DEFAULT_BAUD;
}

esp_err_t gnss_link_write(gnss_link_lease_t lease, const void *data, size_t len, uint32_t timeout_ms)
{
    (void)lease;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_raw_read(gnss_link_lease_t lease, void *buf, size_t len, size_t *out_len,
                             uint32_t timeout_ms)
{
    (void)lease;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    if (out_len != NULL) {
        *out_len = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_raw_flush(gnss_link_lease_t lease)
{
    (void)lease;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_link_get_stats(gnss_link_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->mode = GNSS_LINK_MODE_OFF;
    out->baud = CONFIG_GNSS_LINK_DEFAULT_BAUD;
    out->last_rx_age_ms = UINT32_MAX;
    out->last_rtcm_age_ms = UINT32_MAX;
    return ESP_OK;
}
