/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "station.h"

#include <string.h>

#include "esp_log.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "um98x_ctl.h"
#include "um98x_flash.h"

static const char *TAG = "station";

/* Largest receiver-flash journal blob (gnss/flash_job). The journal codec
 * lands here with the flasher and must keep its records within this size. */
#define FLASH_JOB_BLOB_MAX 64

ESP_EVENT_DEFINE_BASE(STATION_EVENT);

static station_state_t s_state = STATION_STATE_IDLE;
static um98x_base_profile_t s_profile;
static bool s_journal_open;

static void on_rtcm_frame(const uint8_t *frame, size_t len, uint16_t msg_type, void *ctx)
{
    (void)ctx;
    /* Runs in the GNSS RX task: never block. If the hub is not ready the frame
     * is lost; the link counters still show it as received, so health sees
     * the mismatch. */
    (void)rtcm_hub_publish(frame, len, msg_type);
    gnss_obs_feed_rtcm(frame, len, msg_type);
}

static void on_text_line(const char *line, size_t len, void *ctx)
{
    um98x_ctl_on_line(line, len, ctx);
    gnss_obs_feed_line(line, len);
}

static void load_profile(void)
{
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = sizeof(buf);
    if (settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &len) == ESP_OK &&
        um98x_profile_decode(buf, len, &s_profile)) {
        return;
    }
    um98x_profile_default(&s_profile);
}

esp_err_t station_init(void)
{
    const gnss_link_sinks_t sinks = {
        .on_rtcm = on_rtcm_frame,
        .on_line = on_text_line,
        .ctx = NULL,
    };
    esp_err_t err = gnss_link_set_sinks(&sinks);
    if (err != ESP_OK) {
        return err;
    }
    load_profile();

    /* Only "not found" means no journal. Anything else (degraded settings, a
     * larger record from a newer app, a read error) may hide an interrupted
     * flash, and a receiver left in its loader needs recovery, not a scan. */
    uint8_t job[FLASH_JOB_BLOB_MAX];
    size_t job_len = sizeof(job);
    esp_err_t jerr = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, job, &job_len);
    s_journal_open = (jerr != ESP_ERR_NVS_NOT_FOUND);
    if (jerr == ESP_OK) {
        ESP_LOGW(TAG, "receiver flash journal open: recovery runs first");
    } else if (s_journal_open) {
        ESP_LOGW(TAG, "receiver flash journal unreadable (%s): treated as open, recovery checks first",
                 esp_err_to_name(jerr));
    }
    ESP_LOGI(TAG, "stub: sinks wired, profile hash %08x", (unsigned)um98x_profile_hash(&s_profile));
    return ESP_OK;
}

esp_err_t station_start(void)
{
    /* TODO: supervisor task + request queue; FLASH_RECOVERY when the journal
     * is open, otherwise stream-first bring-up. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_get_status(station_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->flash_journal_open = s_journal_open;
    out->position_locked = (s_profile.base_mode == UM98X_BASE_FIXED);
    return ESP_OK;
}

esp_err_t station_request_reconfigure(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_request_flash(station_flash_reason_t reason)
{
    (void)reason;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t station_expected_flash_downtime_s(void)
{
    /* Transfer at the link rate (10 bits per byte) + 10 % protocol overhead,
     * plus receiver restart, reacquisition and survey-in. Refined from
     * measured history once flashing works. */
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return 0;
    }
    uint32_t baud = gnss_link_get_baud();
    uint64_t transfer_s = ((uint64_t)info.size * 10u * 11u) / ((uint64_t)baud * 10u);
    uint32_t survey_s = (s_profile.base_mode == UM98X_BASE_SURVEY_IN) ? s_profile.survey_in_s : 0;
    return (uint32_t)transfer_s + CONFIG_STATION_FLASH_RESTART_ALLOWANCE_S + survey_s;
}

esp_err_t station_request_recovery(station_recovery_t action)
{
    (void)action;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_lock_position(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_request_resurvey(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
