/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * PROVISIONAL stub. The XMODEM-1K sender goes in its own file behind
 * read/write callbacks so it can be unit tested on the host against a
 * simulated receiver.
 */
#include "um98x_flash.h"

#include <string.h>

#include "esp_log.h"
#include "pkg_store.h"

static const char *TAG = "um98x_flash";

static um98x_flash_progress_t s_progress;

esp_err_t um98x_flash_init(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.phase = UM98X_FLASH_IDLE;
    ESP_LOGI(TAG, "stub");
    return ESP_OK;
}

esp_err_t um98x_flash_check_package(const char *expected_model)
{
    (void)expected_model;
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = pkg_store_verify();
    if (err != ESP_OK) {
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req)
{
    (void)raw_lease;
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}

void um98x_flash_cancel(void)
{
}

void um98x_flash_get_progress(um98x_flash_progress_t *out)
{
    if (out != NULL) {
        *out = s_progress;
    }
}
