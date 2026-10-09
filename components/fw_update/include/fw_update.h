/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Firmware updates from GitHub Releases, for both chips, and everything about
 * which app image runs (install, trial, confirm, revert, rollback).
 *
 * Source of truth is a small manifest attached to every release
 * (CONFIG_FW_UPDATE_MANIFEST_ASSET): app version + asset name + size + SHA-256,
 * and receiver package model + version + asset name + size + SHA-256.
 * Stable channel: https://github.com/<repo>/releases/latest/download/<manifest>
 * (a redirect, not the rate-limited REST API). Pre-release channel: the
 * Releases REST API; the newest few releases are fetched and the highest
 * version >= the latest stable one is picked (creation order is not version
 * order). Responses are size-capped; the manifest ETag is cached.
 * GitHub's asset redirects point at long signed CDN URLs, so every request
 * uses CONFIG_FW_UPDATE_HTTP_BUF_SIZE for both the receive and the transmit
 * buffer and follows up to CONFIG_FW_UPDATE_MAX_REDIRECTS redirects.
 *
 * App install: stream the image into the inactive OTA slot (esp_ota_*),
 * hashing as it goes; activate only if the SHA-256 matches; reboot.
 * Before activating, a baseline of the running station is saved (Wi-Fi had
 * an IP, which caster slots were streaming, hub fed).
 *
 * App trial after an update (two stages):
 *  1. Confirm (bootloader rollback armed): the new image runs
 *     PENDING_VERIFY. After CONFIG_FW_UPDATE_CONFIRM_S with init complete and
 *     no reset, it is marked valid. A crash or hang before that, or any reset
 *     at all (the bootloader treats every reset as a failure here), rolls back
 *     to the previous image. The window is kept short for that reason.
 *  2. Verify (app-managed): within CONFIG_FW_UPDATE_VERIFY_WINDOW_S the
 *     station must reach the saved baseline (receiver problems excepted: an
 *     unplugged antenna must not cause rollback loops). Then, for
 *     CONFIG_FW_UPDATE_CRASH_WATCH_H, CONFIG_FW_UPDATE_CRASH_REVERT_COUNT
 *     crash resets revert it. A failed verify or crash watch marks the
 *     running image invalid (esp_ota_mark_app_invalid_rollback) and reboots
 *     into the previous one. Trial state is persisted (ota/trial), so a power
 *     blip during stage 2 just continues the trial. A reboot inhibit is held
 *     for the verify window.
 *
 * Rollback records: a version reverted by stage 2 or rolled back manually
 * goes to ota/bad_ver and is never installed automatically again (manual
 * install still works). A stage-1 rollback (crash or reset in the first
 * minute) is retried automatically once after CONFIG_FW_UPDATE_RESET_RETRY_H,
 * then treated as bad, since a power blip and a real crash look the same to
 * the bootloader.
 *
 * Authenticity: TLS + the manifest's SHA-256 today. Automatic app installs
 * are compiled in only when the build verifies app signatures on update
 * (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT or secure boot): trust in
 * updates is fixed by the image a unit already runs, so it cannot be added
 * later by an update.
 *
 * Receiver: stream the package into pkg_store (SHA-256 checked), then ask the
 * station supervisor to flash it (STATION_FLASH_AUTO is budget-gated, see
 * station.h). Never automatic unless enabled in settings.
 *
 * Safe mode: checks and app installs keep working (a fix must be able to
 * arrive); receiver flashing is off because the station does not run, and
 * that includes finishing an interrupted one (see station.h).
 *
 * All transfers are HTTPS with the CA bundle; plain HTTP is never followed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(FW_UPDATE_EVENT);

/** FW_UPDATE_EVENT ids; data is fw_update_state_t. */
typedef enum {
    FW_UPDATE_EVENT_STATE_CHANGED,
} fw_update_event_t;

typedef enum {
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_CHECKING,
    FW_UPDATE_DOWNLOADING_APP,
    FW_UPDATE_DOWNLOADING_GNSS,
    FW_UPDATE_REBOOT_PENDING,   /**< new app written, reboot scheduled */
    FW_UPDATE_FAILED,
} fw_update_state_t;

/** Where the running image is in its post-update trial. */
typedef enum {
    FW_TRIAL_NONE = 0,          /**< confirmed and past the crash watch (or never updated) */
    FW_TRIAL_CONFIRMING,        /**< stage 1: bootloader rollback armed */
    FW_TRIAL_VERIFYING,         /**< stage 2: must reach the baseline */
    FW_TRIAL_WATCHING,          /**< stage 2: crash watch */
} fw_update_trial_t;

/** Status snapshot for the web UI. */
typedef struct {
    fw_update_state_t state;
    char running_version[32];
    fw_update_trial_t trial;
    uint32_t trial_left_s;         /**< remaining time of the current trial stage */
    char reverted_version[32];     /**< last version rolled back, "" if none */
    bool rollback_available;       /**< the other slot holds a valid image */
    char latest_version[32];       /**< "" until a check succeeded */
    bool app_update_available;
    bool auto_app_supported;       /**< build verifies app signatures */
    char gnss_running[48];         /**< receiver firmware as detected */
    char gnss_latest[48];
    bool gnss_update_available;
    uint8_t progress_pct;          /**< current download */
    int64_t last_check_unix;       /**< 0 = never */
    esp_err_t last_err;
} fw_update_status_t;

/**
 * Boot-time image bookkeeping (early in init, right after settings and
 * lifecycle): note a rollback that happened during the previous boot
 * (esp_ota_get_last_invalid_partition), count trial crashes, revert a failing
 * trial image. Then load the update policy.
 */
esp_err_t fw_update_init(void);

/** Start the worker task, the trial timers and the periodic check. */
esp_err_t fw_update_start(void);

/** Check for updates now (async). */
esp_err_t fw_update_check(void);

/** Download, verify and install the latest app, then reboot (async). */
esp_err_t fw_update_install_app(void);

/** Download the latest receiver package into pkg_store and hand it to the station (async). */
esp_err_t fw_update_install_gnss(void);

/**
 * Manual rollback: mark the running image invalid, record its version in
 * ota/bad_ver, reboot into the other slot. ESP_ERR_NOT_FOUND if the other
 * slot holds no valid image; ESP_ERR_INVALID_STATE while a reboot is inhibited.
 */
esp_err_t fw_update_rollback(void);

/** Status snapshot. */
esp_err_t fw_update_get_status(fw_update_status_t *out);

/**
 * Compare two version strings "vMAJOR.MINOR.PATCH[-pre]" (leading 'v'
 * optional; pre-releases sort before the release).
 *
 * @param out <0, 0 or >0 like strcmp.
 * @return ESP_ERR_INVALID_ARG if either string is not a version (e.g. a dev
 *         build "v0.3.0-4-g47fc47b-dirty+dev"; dev builds never auto-update).
 */
esp_err_t fw_update_version_compare(const char *a, const char *b, int *out);

#ifdef __cplusplus
}
#endif
