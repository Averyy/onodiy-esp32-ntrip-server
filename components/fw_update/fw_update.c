/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fw_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lifecycle.h"
#include "sdkconfig.h"
#include "semver.h"
#include "station.h"

static const char *TAG = "fw_update";

ESP_EVENT_DEFINE_BASE(FW_UPDATE_EVENT);

static fw_update_trial_t s_trial = FW_TRIAL_NONE;

/* Why a receiver update cannot start now (banner, dialog, and the install
 * call itself). The station repeats its own checks when it is asked. */
static fw_update_gnss_block_t gnss_block_reason(void)
{
    if (lifecycle_in_safe_mode()) {
        return FW_GNSS_BLOCK_SAFE_MODE;
    }
    if (lifecycle_app_trial_active()) {
        return FW_GNSS_BLOCK_APP_TRIAL;
    }
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_APP) {
        return FW_GNSS_BLOCK_APP_UPDATE;
    }
    station_status_t st;
    if (station_get_status(&st) != ESP_OK || st.flash_journal_open) {
        return FW_GNSS_BLOCK_JOURNAL_OPEN;
    }
    if (st.receiver.valid && strcmp(st.receiver.model, "UM982") != 0) {
        return FW_GNSS_BLOCK_UNSUPPORTED_MODEL;
    }
    if (st.state != STATION_STATE_STREAMING) {
        return FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING;
    }
    return FW_GNSS_BLOCK_NONE;
}

esp_err_t fw_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_trial = FW_TRIAL_CONFIRMING;
        ESP_LOGW(TAG, "new app image on trial: confirmation after %d s", CONFIG_FW_UPDATE_CONFIRM_S);
    }
    /* TODO: rollback bookkeeping (last invalid partition -> ota/bad_ver, or a
     * stage-1 rollback the banner offers again once), stage 2 and crash watch
     * from ota/trial with crash counting from lifecycle boot info, stage-2
     * revert. An older image in the other slot can also be invalid because it
     * gave way to this one for its NVS (app_main): not a failed trial of
     * ours, and never offered (no downgrades). */
    lifecycle_set_app_trial(s_trial != FW_TRIAL_NONE);
    ESP_LOGI(TAG, "stub: releases from github.com/%s, update check every %d h, installs only on request",
             CONFIG_FW_UPDATE_GITHUB_REPO, CONFIG_FW_UPDATE_CHECK_INTERVAL_H);
    return ESP_OK;
}

esp_err_t fw_update_start(void)
{
    /* TODO: worker task, stage-1 confirm timer, stage-2 verify window and
     * crash watch (clear the lifecycle trial flag when it ends), the daily
     * manifest check (due 24 h after ota/last_check; the first of a boot not
     * before 5-15 min after boot, MAC-jittered; without a trusted clock it
     * waits for one, at most 1 h). */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_check(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_install_app(const char *offered_version)
{
    if (offered_version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lifecycle_update_active() != LIFECYCLE_UPDATE_NONE) {
        return ESP_ERR_INVALID_STATE;   /* a receiver flash or another app update holds the slot */
    }
    /* TODO: lifecycle_update_claim(LIFECYCLE_UPDATE_APP) before the first
     * image byte; release it on failure (success reboots). */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fw_update_install_gnss(uint32_t offered_build, bool cross_layout_ack)
{
    (void)offered_build;
    (void)cross_layout_ack;
    if (gnss_block_reason() != FW_GNSS_BLOCK_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
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
    out->gnss_blocked = gnss_block_reason();
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
