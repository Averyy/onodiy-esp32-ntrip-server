/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * fw_update internals shared by its source files (not part of the API).
 *
 *   fw_update.c   state, public API, worker task, daily check, banner
 *   fw_trial.c    trial and rollback bookkeeping at boot and while running
 *   fw_install.c  app install (download and manual upload) and receiver package job
 *   fw_http.c     HTTPS: manifest fetch, package download, app image download
 *   release_url.c repository name, release URLs, the hosts a transfer may reach (pure)
 *
 * Locking: g_fw.mtx guards every field the status getter reads. It is held
 * only for copies, never across network or flash work.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_trial.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "lifecycle.h"
#include "manifest.h"
#include "update_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Worker jobs. */
typedef enum {
    FW_JOB_CHECK = 1,
    FW_JOB_INSTALL_APP,
    FW_JOB_INSTALL_GNSS,
} fw_job_kind_t;

typedef struct {
    fw_job_kind_t kind;
    uint32_t gnss_build;          /**< FW_JOB_INSTALL_GNSS: the entry the user confirmed */
    bool cross_layout_ack;
} fw_job_t;

typedef struct {
    SemaphoreHandle_t mtx;
    QueueHandle_t jobs;
    TaskHandle_t task;
    bool started;                 /**< fw_update_start ran: every start step is done */

    /* Update state (banner, progress). */
    fw_update_state_t state;
    esp_err_t last_err;
    uint8_t progress_pct;
    bool busy;                    /**< a job or a manual upload runs (one at a time) */
    bool have_manifest;           /**< `manifest` was accepted during this boot */
    fw_manifest_t manifest;
    char etag[64];                /**< ETag of `manifest` */
    int64_t last_check_unix;      /**< ota/last_check */
    fw_check_result_t last_result;
    uint32_t last_attempt_s;
    bool have_success;
    uint32_t last_success_s;
    uint32_t first_check_s;       /**< MAC-jittered */
    fw_manifest_key_t keys[FW_MANIFEST_MAX_KEYS];
    size_t key_count;             /**< keys that parsed */
    bool signature_required;      /**< a manifest key is configured (parsed or not): unsigned manifests are refused */
    bool checks_enabled;          /**< a valid repository, and a usable key when one is configured */
    char repo[FW_UPDATE_REPO_MAX];/**< "owner/name" releases are read from (fixed at init) */

    /* App trial (fw_trial.c). */
    fw_trial_rec_t rec;           /**< ota/trial */
    fw_bad_versions_t bad;        /**< ota/bad_ver */
    fw_trial_run_t run;
    uint32_t stage2_start_s;      /**< uptime at which this boot's stage 2 began */
    uint32_t window_start_s;      /**< uptime at which this boot's verify window began */
    fw_trial_obs_t obs;
    bool trial_inhibit_held;
    lifecycle_inhibit_t trial_inhibit;
    bool rollback_available;      /**< the other slot holds a bootable image (refreshed by the worker) */
    bool rollback_known;
    char etag_saved[64];          /**< ota/etag as stored */
    fw_job_kind_t job;            /**< the job that holds `busy` (0 for a manual upload) */
} fw_ctx_t;

extern fw_ctx_t g_fw;

void fw_lock(void);
void fw_unlock(void);

/** Uptime in seconds. */
uint32_t fw_uptime_s(void);

/** Current unix time; false (and *now = 0) without a trusted clock. */
bool fw_clock(int64_t *now);

/** Set the state, post FW_UPDATE_EVENT_STATE_CHANGED when it changed (call unlocked). */
void fw_set_state(fw_update_state_t state, esp_err_t err);

/** Set the download progress (call unlocked). */
void fw_set_progress(uint8_t pct);

/**
 * "owner/name" of the repository serving releases, chosen at init:
 * CONFIG_FW_UPDATE_GITHUB_REPO. A build that requires signed manifests also
 * honours the ota/repo override, read at boot (its key, not the repository,
 * is what it trusts); a build without a key trusts the repository it was
 * built for and nothing else.
 */
void fw_repo(char *out, size_t out_size);

/** https://github.com/<repo>/releases/download/<tag>/<asset>; false if a part is invalid or it does not fit. */
bool fw_asset_url(const char *tag, const char *asset, char *out, size_t out_size);

/** The RTCM hub received a frame within the last few seconds. */
bool fw_hub_fed_now(void);

/** Map the trial state of this boot to the public enum. */
fw_update_trial_t fw_trial_public(fw_trial_run_t run);

/* ---- fw_trial.c ---------------------------------------------------------------------- */

/** Boot bookkeeping (fw_update_init). May revert and not return. */
void fw_trial_boot(void);

/** Start-time part (fw_update_start): inhibit and windows for this boot's stage. */
void fw_trial_start(void);

/** Once a second on the worker. */
void fw_trial_tick_worker(void);

/** Persist the trial record / the rolled-back list (logs failures). */
void fw_trial_save_rec(const fw_trial_rec_t *rec);
void fw_trial_save_bad(const fw_bad_versions_t *bad);

/** Trial fields of the status, and gnss_blocked_left_s for a trial block (call locked). */
void fw_trial_status_locked(fw_update_status_t *out, bool clock_trusted, int64_t now_unix);

/** Refresh rollback_available (validates the other image: worker only). */
void fw_trial_refresh_rollback(void);

/** A new image was activated: replace the trial record (install path). */
void fw_trial_record_install(const char *version, uint8_t baseline, uint8_t slots);

/** The trial refuses an app install now: stage 1 or the verify window (call locked). */
bool fw_trial_blocks_install_locked(void);

/* ---- fw_install.c -------------------------------------------------------------------- */

/** Worker: download, verify and activate the manifest's app image, then reboot. */
esp_err_t fw_install_app_job(void);

/** Worker: download the receiver package for `build` and hand it to the station. */
esp_err_t fw_install_gnss_job(uint32_t build, bool cross_layout_ack);

/** Baseline of the running station for the next image's trial. */
void fw_capture_baseline(uint8_t *baseline, uint8_t *slots);

/* ---- fw_http.c ----------------------------------------------------------------------- */

typedef enum {
    FW_HTTP_OK = 0,
    FW_HTTP_NOT_MODIFIED,
    FW_HTTP_NO_RESPONSE,     /**< no final HTTP answer (network, DNS, TLS, timeout) */
    FW_HTTP_BAD_ANSWER,      /**< an answer, but not the expected one (status, size, too large) */
    FW_HTTP_LOCAL_ERROR,     /**< out of memory or a local write failed */
    FW_HTTP_UNTRUSTED,       /**< the URL or a redirect left GitHub's release hosts (release_url.h): not connected */
} fw_http_result_t;

/**
 * GET url (following HTTPS redirects between release hosts only) into a new
 * buffer of at most `cap` bytes (PSRAM when available; caller frees *out).
 * With `etag_in` set, sends If-None-Match; `etag_out` gets the answer's ETag.
 */
fw_http_result_t fw_http_fetch(const char *url, const char *etag_in, size_t cap, char **out, size_t *out_len,
                               char *etag_out, size_t etag_size);

/**
 * Download `file` from `url` into pkg_store (size and SHA-256 checked there).
 * ESP_ERR_NOT_ALLOWED when a redirect leaves the release hosts.
 */
esp_err_t fw_http_download_package(const char *url, const fw_manifest_file_t *file, const char *label);

/**
 * Download the app image from `url` into the inactive slot
 * (esp_ota_begin/write, sequential erase), check its size and SHA-256 on the
 * stream, validate it (esp_ota_end), check the SHA-256 again on what is in
 * flash and that its app description names the manifest's version and this
 * project, then activate it. ESP_ERR_NOT_ALLOWED when a redirect leaves the
 * release hosts, ESP_ERR_INVALID_SIZE / ESP_ERR_INVALID_CRC when the bytes
 * are not the manifest's, ESP_ERR_INVALID_VERSION for another release or
 * project.
 */
esp_err_t fw_http_download_app(const char *url, const fw_manifest_file_t *file, const char *version);

#ifdef __cplusplus
}
#endif
