/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Public API, worker task, the daily check and the banner state.
 */
#include "fw_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "fw_internal.h"
#include "network.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "semver.h"
#include "settings.h"
#include "station.h"

static const char *TAG = "fw_update";

ESP_EVENT_DEFINE_BASE(FW_UPDATE_EVENT);

fw_ctx_t g_fw;

#define WORKER_STACK     8192
#define WORKER_PRIO      4
#define WORKER_TICK_MS   1000
#define JOB_QUEUE_LEN    4
#define HUB_FED_MS       5000u
#define CHECK_INTERVAL_S ((uint32_t)CONFIG_FW_UPDATE_CHECK_INTERVAL_H * 3600u)
#define MANIFEST_CAP     ((size_t)CONFIG_FW_UPDATE_MANIFEST_MAX_KB * 1024u)
/* A wall clock before this (2026-01-01) is not a trusted one. */
#define MIN_TRUSTED_UNIX 1767225600LL

/* The receiver model this firmware configures and flashes. */
#define SUPPORTED_MODEL "UM982"

/* ---- shared helpers -------------------------------------------------------------- */

void fw_lock(void)
{
    xSemaphoreTake(g_fw.mtx, portMAX_DELAY);
}

void fw_unlock(void)
{
    xSemaphoreGive(g_fw.mtx);
}

uint32_t fw_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

bool fw_clock(int64_t *now)
{
    *now = 0;
    network_status_t ns;
    if (network_get_status(&ns) != ESP_OK || !ns.time_synced) {
        return false;
    }
    time_t t = time(NULL);
    if ((int64_t)t < MIN_TRUSTED_UNIX) {
        return false;
    }
    *now = (int64_t)t;
    return true;
}

void fw_set_state(fw_update_state_t state, esp_err_t err)
{
    fw_lock();
    bool changed = g_fw.state != state;
    g_fw.state = state;
    g_fw.last_err = err;
    if (state != FW_UPDATE_DOWNLOADING_APP && state != FW_UPDATE_DOWNLOADING_GNSS &&
        state != FW_UPDATE_UPLOADING_APP) {
        g_fw.progress_pct = 0;
    }
    fw_unlock();
    if (changed) {
        (void)esp_event_post(FW_UPDATE_EVENT, FW_UPDATE_EVENT_STATE_CHANGED, &state, sizeof(state), 0);
    }
}

void fw_set_progress(uint8_t pct)
{
    fw_lock();
    g_fw.progress_pct = pct > 100 ? 100 : pct;
    fw_unlock();
}

static bool repo_ok(const char *repo)
{
    int slashes = 0;
    size_t n = 0;
    for (const char *p = repo; *p != '\0'; p++, n++) {
        char c = *p;
        if (c == '/') {
            if (n == 0 || p[1] == '\0') {
                return false;
            }
            slashes++;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.')) {
            return false;
        }
    }
    return slashes == 1;
}

void fw_repo(char *out, size_t out_size)
{
    char repo[96];
    if (settings_get_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_REPO, repo, sizeof(repo)) == ESP_OK && repo_ok(repo)) {
        strlcpy(out, repo, out_size);
        return;
    }
    strlcpy(out, CONFIG_FW_UPDATE_GITHUB_REPO, out_size);
}

bool fw_asset_url(const char *tag, const char *asset, char *out, size_t out_size)
{
    char repo[96];
    fw_repo(repo, sizeof(repo));
    int n = snprintf(out, out_size, "https://github.com/%s/releases/download/%s/%s", repo, tag, asset);
    return n > 0 && (size_t)n < out_size;
}

bool fw_hub_fed_now(void)
{
    rtcm_hub_stats_t st;
    rtcm_hub_get_stats(&st);
    return st.last_frame_age_ms <= HUB_FED_MS;
}

fw_update_trial_t fw_trial_public(fw_trial_run_t run)
{
    switch (run) {
    case FW_TRIAL_RUN_CONFIRMING:
        return FW_TRIAL_CONFIRMING;
    case FW_TRIAL_RUN_VERIFYING:
        return FW_TRIAL_VERIFYING;
    case FW_TRIAL_RUN_WATCHING:
        return FW_TRIAL_WATCHING;
    case FW_TRIAL_RUN_NONE:
        break;
    }
    return FW_TRIAL_NONE;
}

static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

/* ---- receiver update gate ------------------------------------------------------------ */

/* Why a receiver update cannot start now (banner, dialog, and the install
 * call itself). The station repeats its own checks when it is asked. */
static fw_update_gnss_block_t gnss_block_reason(const station_status_t *st, bool st_ok)
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
    if (!st_ok || st->flash_journal_open) {
        return FW_GNSS_BLOCK_JOURNAL_OPEN;
    }
    if (st->receiver.valid && strcmp(st->receiver.model, SUPPORTED_MODEL) != 0) {
        return FW_GNSS_BLOCK_UNSUPPORTED_MODEL;
    }
    if (st->state != STATION_STATE_STREAMING) {
        return FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING;
    }
    return FW_GNSS_BLOCK_NONE;
}

/* ---- the update check ---------------------------------------------------------------- */

static bool is_bad_locked(const char *v, void *ctx)
{
    return fw_bad_contains((const fw_bad_versions_t *)ctx, v);
}

static void log_banner(void)
{
    fw_lock();
    fw_app_offer_t offer;
    const char *stage1 = g_fw.rec.stage == FW_TRIAL_REC_ROLLED_BACK ? g_fw.rec.version : "";
    fw_policy_app_offer(running_version(), g_fw.manifest.version, is_bad_locked, &g_fw.bad, stage1, &offer);
    char latest[32];
    strlcpy(latest, g_fw.manifest.version, sizeof(latest));
    fw_unlock();
    if (offer.available) {
        ESP_LOGW(TAG, "app %s available (running %s)%s", latest, running_version(),
                 offer.retry_after_stage1 ? ", rolled back once during its first minute" : "");
    } else if (offer.rolled_back) {
        ESP_LOGW(TAG, "app %s available but rolled back on this station before: not offered", latest);
    } else {
        ESP_LOGI(TAG, "latest release %s, running %s: nothing to offer", latest, running_version());
    }
}

static void run_check(void)
{
    uint32_t started = fw_uptime_s();
    fw_set_state(FW_UPDATE_CHECKING, ESP_OK);

    char repo[96];
    char url[256];
    fw_repo(repo, sizeof(repo));
    snprintf(url, sizeof(url), "https://github.com/%s/releases/latest/download/%s", repo,
             CONFIG_FW_UPDATE_MANIFEST_ASSET);

    char etag_in[sizeof(g_fw.etag)];
    fw_lock();
    bool have = g_fw.have_manifest;
    strlcpy(etag_in, g_fw.etag, sizeof(etag_in));
    fw_unlock();

    char *body = NULL;
    size_t len = 0;
    char etag[sizeof(g_fw.etag)] = "";
    /* "Not modified" is only useful while this boot holds the verified
     * manifest it refers to. */
    fw_http_result_t r = fw_http_fetch(url, (have && etag_in[0] != '\0') ? etag_in : NULL, MANIFEST_CAP, &body, &len,
                                       etag, sizeof(etag));

    fw_check_result_t result;
    esp_err_t err = ESP_OK;
    switch (r) {
    case FW_HTTP_OK: {
        static fw_manifest_t m;   /* worker only; too large for the stack */
        int key = -1;
        fw_manifest_err_t merr = fw_manifest_verify(body, len, g_fw.keys, g_fw.key_count, &m, &key);
        if (merr == FW_MANIFEST_OK) {
            fw_lock();
            g_fw.manifest = m;
            g_fw.have_manifest = true;
            strlcpy(g_fw.etag, etag, sizeof(g_fw.etag));
            fw_unlock();
            result = FW_CHECK_OK;
            ESP_LOGI(TAG, "manifest %s verified (key %d), %u receiver package(s)", m.version, key + 1,
                     (unsigned)m.gnss_count);
            if (strcmp(etag, g_fw.etag_saved) != 0) {
                if (settings_set_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_ETAG, etag) == ESP_OK) {
                    strlcpy(g_fw.etag_saved, etag, sizeof(g_fw.etag_saved));
                }
            }
        } else {
            result = merr == FW_MANIFEST_ERR_NO_MEM ? FW_CHECK_NO_RESPONSE : FW_CHECK_UNUSABLE;
            err = merr == FW_MANIFEST_ERR_SIGNATURE   ? ESP_ERR_INVALID_CRC
                  : merr == FW_MANIFEST_ERR_NO_MEM    ? ESP_ERR_NO_MEM
                  : merr == FW_MANIFEST_ERR_NO_KEY    ? ESP_ERR_NOT_SUPPORTED
                                                      : ESP_ERR_INVALID_RESPONSE;
            ESP_LOGE(TAG, "release manifest rejected: %s", fw_manifest_err_str(merr));
        }
        break;
    }
    case FW_HTTP_NOT_MODIFIED:
        result = FW_CHECK_OK;
        ESP_LOGI(TAG, "manifest unchanged");
        break;
    case FW_HTTP_BAD_ANSWER:
        result = FW_CHECK_UNUSABLE;
        err = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGW(TAG, "no usable release manifest at github.com/%s", repo);
        break;
    case FW_HTTP_LOCAL_ERROR:
        result = FW_CHECK_NO_RESPONSE;
        err = ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "update check failed: out of memory");
        break;
    case FW_HTTP_NO_RESPONSE:
    default:
        result = FW_CHECK_NO_RESPONSE;
        err = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "update check: no answer from GitHub, retry in %u min", (unsigned)(FW_SCHED_RETRY_S / 60u));
        break;
    }
    free(body);

    int64_t now = 0;
    bool trusted = fw_clock(&now);
    fw_lock();
    g_fw.last_result = result;
    g_fw.last_attempt_s = started;
    if (result == FW_CHECK_OK) {
        g_fw.have_success = true;
        g_fw.last_success_s = started;
        if (trusted) {
            g_fw.last_check_unix = now;
        }
    }
    fw_unlock();
    if (result == FW_CHECK_OK && trusted &&
        settings_set_i64(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_LAST_CHECK, now) != ESP_OK) {
        ESP_LOGW(TAG, "time of the update check not stored");
    }
    if (result == FW_CHECK_OK) {
        log_banner();
    }
    fw_set_state(err == ESP_OK ? FW_UPDATE_IDLE : FW_UPDATE_FAILED, err);
}

static bool check_due(void)
{
    int64_t now = 0;
    bool trusted = fw_clock(&now);
    fw_lock();
    fw_sched_t s = {
        .uptime_s = fw_uptime_s(),
        .clock_trusted = trusted,
        .now_unix = now,
        .last_check_unix = g_fw.last_check_unix,
        .interval_s = CHECK_INTERVAL_S,
        .first_check_s = g_fw.first_check_s,
        .last_result = g_fw.last_result,
        .last_attempt_s = g_fw.last_attempt_s,
        .have_success = g_fw.have_success,
        .last_success_s = g_fw.last_success_s,
    };
    bool idle = !g_fw.busy && g_fw.state != FW_UPDATE_REBOOT_PENDING && g_fw.run != FW_TRIAL_RUN_CONFIRMING &&
                g_fw.key_count > 0;
    fw_unlock();
    return idle && fw_sched_wait_s(&s) == 0 && network_is_online();
}

/* ---- worker ---------------------------------------------------------------------- */

static void run_job(const fw_job_t *job)
{
    esp_err_t err = ESP_OK;
    switch (job->kind) {
    case FW_JOB_CHECK:
        run_check();
        break;
    case FW_JOB_INSTALL_APP:
        err = fw_install_app_job();
        break;
    case FW_JOB_INSTALL_GNSS:
        err = fw_install_gnss_job(job->gnss_build, job->cross_layout_ack);
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    }
    fw_lock();
    g_fw.busy = false;
    g_fw.job = 0;
    fw_unlock();
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        fw_job_t job;
        if (xQueueReceive(g_fw.jobs, &job, pdMS_TO_TICKS(WORKER_TICK_MS)) == pdTRUE) {
            run_job(&job);
        }
        fw_trial_tick_worker();
        if (!g_fw.rollback_known) {
            fw_trial_refresh_rollback();
        }
        if (check_due()) {
            fw_lock();
            bool claimed = !g_fw.busy;
            if (claimed) {
                g_fw.busy = true;
                g_fw.job = FW_JOB_CHECK;
            }
            fw_unlock();
            if (claimed) {
                fw_job_t check = { .kind = FW_JOB_CHECK };
                run_job(&check);
            }
        }
    }
}

/* Claim fw_update for one job and queue it. Call with the lock held. */
static esp_err_t queue_job_locked(const fw_job_t *job)
{
    if (g_fw.jobs == NULL || !g_fw.started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_fw.busy || g_fw.state == FW_UPDATE_REBOOT_PENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(g_fw.jobs, job, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    g_fw.busy = true;
    g_fw.job = job->kind;
    return ESP_OK;
}

/* ---- public API ----------------------------------------------------------------------- */

static void load_keys(void)
{
    const char *hex[FW_MANIFEST_MAX_KEYS] = { CONFIG_FW_UPDATE_MANIFEST_KEY_1, CONFIG_FW_UPDATE_MANIFEST_KEY_2 };
    g_fw.key_count = 0;
    for (size_t i = 0; i < FW_MANIFEST_MAX_KEYS; i++) {
        if (hex[i][0] == '\0') {
            continue;
        }
        if (fw_manifest_key_from_hex(hex[i], &g_fw.keys[g_fw.key_count])) {
            g_fw.key_count++;
        } else {
            ESP_LOGE(TAG, "manifest key %u is not a P-256 public key in hex: ignored", (unsigned)(i + 1));
        }
    }
}

esp_err_t fw_update_init(void)
{
    if (g_fw.mtx == NULL) {
        g_fw.mtx = xSemaphoreCreateMutex();
        g_fw.jobs = xQueueCreate(JOB_QUEUE_LEN, sizeof(fw_job_t));
        if (g_fw.mtx == NULL || g_fw.jobs == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_keys();

    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    g_fw.first_check_s = fw_sched_jitter_s(mac);

    int64_t last = 0;
    if (settings_get_i64(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_LAST_CHECK, &last) == ESP_OK) {
        g_fw.last_check_unix = last;
    }
    (void)settings_get_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_ETAG, g_fw.etag_saved, sizeof(g_fw.etag_saved));

    /* Trial and rollback bookkeeping; a failing trial image is reverted here
     * (and this does not return), before the steps that may be what fails. */
    fw_trial_boot();

    char repo[96];
    fw_repo(repo, sizeof(repo));
    if (g_fw.key_count == 0) {
        g_fw.last_err = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "no manifest signing key in this build: update checks are off (manual uploads still work)");
    } else {
        ESP_LOGI(TAG, "releases from github.com/%s, checked every %d h (first not before %u s), installs only on "
                      "request",
                 repo, CONFIG_FW_UPDATE_CHECK_INTERVAL_H, (unsigned)g_fw.first_check_s);
    }
    return ESP_OK;
}

esp_err_t fw_update_start(void)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_fw.task != NULL) {
        return ESP_OK;
    }
    fw_trial_start();
    fw_lock();
    g_fw.started = true;
    fw_unlock();
    if (xTaskCreate(worker, "fwupd", WORKER_STACK, NULL, WORKER_PRIO, &g_fw.task) != pdPASS) {
        fw_lock();
        g_fw.started = false;
        fw_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fw_update_check(void)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_fw.key_count == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    fw_lock();
    esp_err_t err;
    if (g_fw.busy && g_fw.job == FW_JOB_CHECK) {
        err = ESP_OK;   /* already under way */
    } else if (g_fw.run == FW_TRIAL_RUN_CONFIRMING) {
        err = ESP_ERR_INVALID_STATE;   /* nothing may delay the new image's confirmation */
    } else {
        fw_job_t job = { .kind = FW_JOB_CHECK };
        err = queue_job_locked(&job);
    }
    fw_unlock();
    return err;
}

esp_err_t fw_update_install_app(const char *offered_version)
{
    if (offered_version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    fw_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!g_fw.have_manifest || strcmp(offered_version, g_fw.manifest.version) != 0) {
        ESP_LOGW(TAG, "install %s refused: not the release the last check found", offered_version);
    } else if (!fw_policy_app_install_allowed(running_version(), offered_version)) {
        ESP_LOGW(TAG, "install %s refused: not newer than %s (use a manual upload)", offered_version,
                 running_version());
    } else if (fw_trial_blocks_install_locked()) {
        ESP_LOGW(TAG, "install refused: the running image is still on trial");
    } else if (lifecycle_update_active() != LIFECYCLE_UPDATE_NONE) {
        ESP_LOGW(TAG, "install refused: another firmware update runs");
    } else {
        fw_job_t job = { .kind = FW_JOB_INSTALL_APP };
        err = queue_job_locked(&job);
    }
    fw_unlock();
    return err;
}

esp_err_t fw_update_install_gnss(uint32_t offered_build, bool cross_layout_ack)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    station_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool st_ok = station_get_status(st) == ESP_OK;
    fw_update_gnss_block_t block = gnss_block_reason(st, st_ok);
    pkg_store_info_t pkg;
    bool pinned = pkg_store_get_info(&pkg) == ESP_OK && pkg.pinned;

    esp_err_t err = ESP_ERR_INVALID_STATE;
    fw_lock();
    const fw_manifest_gnss_t *entry =
        g_fw.have_manifest ? fw_policy_gnss_offer(&g_fw.manifest, st->receiver.model, st->receiver.build) : NULL;
    if (block != FW_GNSS_BLOCK_NONE) {
        ESP_LOGW(TAG, "receiver update refused (reason %d)", (int)block);
    } else if (entry == NULL || entry->build != offered_build) {
        ESP_LOGW(TAG, "receiver update refused: build %u is not the one offered", (unsigned)offered_build);
    } else if (pinned) {
        ESP_LOGW(TAG, "receiver update refused: a staged package is pinned by an open session");
    } else {
        fw_job_t job = { .kind = FW_JOB_INSTALL_GNSS, .gnss_build = offered_build, .cross_layout_ack = cross_layout_ack };
        err = queue_job_locked(&job);
    }
    fw_unlock();
    free(st);
    return err;
}

esp_err_t fw_update_get_status(fw_update_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->running_version, running_version(), sizeof(out->running_version));

    station_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool st_ok = station_get_status(st) == ESP_OK;
    out->gnss_blocked = gnss_block_reason(st, st_ok);
    if (st_ok && st->receiver.valid) {
        strlcpy(out->gnss_running, st->receiver.firmware, sizeof(out->gnss_running));
    }
    int64_t now = 0;
    bool trusted = fw_clock(&now);

    if (g_fw.mtx == NULL) {
        free(st);
        return ESP_OK;
    }
    fw_lock();
    out->state = g_fw.state;
    out->last_err = g_fw.last_err;
    out->progress_pct = g_fw.progress_pct;
    out->last_check_unix = g_fw.last_check_unix;
    fw_trial_status_locked(out, trusted, now);
    if (g_fw.have_manifest) {
        strlcpy(out->latest_version, g_fw.manifest.version, sizeof(out->latest_version));
        fw_app_offer_t offer;
        const char *stage1 = g_fw.rec.stage == FW_TRIAL_REC_ROLLED_BACK ? g_fw.rec.version : "";
        fw_policy_app_offer(out->running_version, g_fw.manifest.version, is_bad_locked, &g_fw.bad, stage1, &offer);
        out->app_update_available = offer.available;
        out->latest_rolled_back = offer.rolled_back;
        const fw_manifest_gnss_t *g =
            st_ok && st->receiver.valid
                ? fw_policy_gnss_offer(&g_fw.manifest, st->receiver.model, st->receiver.build)
                : NULL;
        if (g != NULL) {
            strlcpy(out->gnss_latest, g->version, sizeof(out->gnss_latest));
            out->gnss_latest_build = g->build;
            out->gnss_update_available = true;
            out->gnss_expected_downtime_s = station_expected_flash_downtime_s(g->file.size);
        }
    }
    fw_unlock();
    free(st);
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
