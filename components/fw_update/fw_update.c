/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fw_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"
#include "semver.h"

static const char *TAG = "fw_update";

ESP_EVENT_DEFINE_BASE(FW_UPDATE_EVENT);

#if CONFIG_FW_UPDATE_AUTO_APP_SUPPORTED
#define AUTO_APP_SUPPORTED true
#else
#define AUTO_APP_SUPPORTED false
#endif

static fw_update_trial_t s_trial = FW_TRIAL_NONE;

esp_err_t fw_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_trial = FW_TRIAL_CONFIRMING;
        ESP_LOGW(TAG, "new app image on trial: confirmation after %d s", CONFIG_FW_UPDATE_CONFIRM_S);
    }
    /* TODO: rollback bookkeeping (last invalid partition -> ota/bad_ver or one
     * retry), trial crash counting from lifecycle boot info, stage-2 revert.
     * An older image in the other slot can also be invalid because it gave
     * way to this one for its NVS (app_main): not a failed trial of ours, and
     * never a reinstall candidate (no auto-downgrade). */
    if (!AUTO_APP_SUPPORTED) {
        ESP_LOGI(TAG, "automatic app installs disabled: this build does not verify app signatures");
    }
    ESP_LOGI(TAG, "stub: releases from github.com/%s, check every %d h", CONFIG_FW_UPDATE_GITHUB_REPO,
             CONFIG_FW_UPDATE_CHECK_INTERVAL_H);
    return ESP_OK;
}

esp_err_t fw_update_start(void)
{
    /* TODO: worker task, stage-1 confirm timer, stage-2 verify window,
     * periodic check, manifest fetch. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_check(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_install_app(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_install_gnss(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_rollback(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_get_status(fw_update_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = FW_UPDATE_IDLE;
    out->trial = s_trial;
    out->auto_app_supported = AUTO_APP_SUPPORTED;
    strlcpy(out->running_version, esp_app_get_description()->version, sizeof(out->running_version));
    return ESP_OK;
}

esp_err_t fw_update_version_compare(const char *a, const char *b, int *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = 0;
    semver_t va;
    semver_t vb;
    if (!semver_parse(a, &va) || !semver_parse(b, &vb)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = semver_compare(&va, &vb);
    return ESP_OK;
}
