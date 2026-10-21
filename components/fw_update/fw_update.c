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
#include "release_url.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "semver.h"
#include "settings.h"
#include "station.h"
#include "um98x_flash.h"

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
#define MANIFEST_KEEP    4000u   /* a larger manifest (with its repository) is not kept over a restart */
/* A wall clock before this (2026-01-01) is not a trusted one. */
#define MIN_TRUSTED_UNIX 1767225600LL

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

/* Pick the repository once, at init (after load_trust). */
static void select_repo(void)
{
    char repo[FW_UPDATE_REPO_MAX];
    strlcpy(g_fw.repo, CONFIG_FW_UPDATE_GITHUB_REPO, sizeof(g_fw.repo));
    if (settings_get_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_REPO, repo, sizeof(repo)) != ESP_OK || repo[0] == '\0' ||
        strcmp(repo, g_fw.repo) == 0) {
        return;
    }
    /* Without a key the repository is the trust anchor: only the one this
     * image was built for. A signed-manifest build trusts its key instead. */
    if (!g_fw.signature_required) {
        ESP_LOGW(TAG, "ota/repo \"%s\" ignored: without a manifest key releases come from %s only", repo, g_fw.repo);
    } else if (!fw_release_repo_ok(repo)) {
        ESP_LOGW(TAG, "ota/repo \"%s\" ignored: not owner/name", repo);
    } else {
        strlcpy(g_fw.repo, repo, sizeof(g_fw.repo));
    }
}

void fw_repo(char *out, size_t out_size)
{
    strlcpy(out, g_fw.repo, out_size);
}

bool fw_asset_url(const char *tag, const char *asset, char *out, size_t out_size)
{
    return fw_release_asset_url(out, out_size, g_fw.repo, tag, asset);
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
    if (st->receiver.valid && !um98x_flash_model_supported(st->receiver.model)) {
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

/* The accepted manifest in NVS (ota/manifest): the repository it came from and its ETag, each ended by a NUL, then
 * the body as fetched. The ETag travels with the body, so a restored manifest is never asked about with another
 * one's ETag (a "not modified" would then keep an old release). */
static void save_manifest(const char *repo, const char *etag, const char *body, size_t len)
{
    g_fw.manifest_saved = false;
    size_t rl = strlen(repo) + 1u;
    size_t el = strlen(etag) + 1u;
    if (len > MANIFEST_KEEP - rl - el) {
        ESP_LOGW(TAG, "manifest of %u bytes: not kept over a restart", (unsigned)len);
        return;
    }
    char *buf = malloc(rl + el + len);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, repo, rl);
    memcpy(buf + rl, etag, el);
    memcpy(buf + rl + el, body, len);
    esp_err_t err = settings_set_state_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_MANIFEST, buf, rl + el + len);
    free(buf);
    if (err == ESP_OK) {
        g_fw.manifest_saved = true;
    } else {
        ESP_LOGW(TAG, "manifest not stored: %s", esp_err_to_name(err));
    }
}

/* At boot: the manifest the last check accepted, read again as a fetched one is (its signature too), and only
 * if it came from this build's repository, so the update card knows the latest release before the next check. */
static void load_manifest(void)
{
    char repo[FW_UPDATE_REPO_MAX];
    fw_repo(repo, sizeof(repo));
    size_t len = MANIFEST_KEEP;
    char *buf = malloc(len);
    if (buf == NULL) {
        return;
    }
    if (settings_get_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_MANIFEST, buf, &len) == ESP_OK) {
        size_t rl = strnlen(buf, len);
        size_t el = rl < len ? strnlen(buf + rl + 1u, len - rl - 1u) : 0;
        if (rl < len && el < len - rl - 1u && el < sizeof(g_fw.etag) && strcmp(buf, repo) == 0) {
            const char *etag = buf + rl + 1u;
            const char *body = etag + el + 1u;
            int key = -1;
            fw_lock();
            bool ok = fw_manifest_read(body, len - rl - el - 2u, g_fw.keys, g_fw.key_count, &g_fw.manifest, &key) ==
                      FW_MANIFEST_OK;
            g_fw.have_manifest = ok;
            g_fw.manifest_saved = ok;
            if (ok) {
                strlcpy(g_fw.etag, etag, sizeof(g_fw.etag));
            }
            fw_unlock();
            if (ok) {
                ESP_LOGI(TAG, "latest release %s, as the last check found", g_fw.manifest.version);
            } else {
                ESP_LOGW(TAG, "stored manifest not accepted: the next check fetches it again");
            }
        }
    }
    free(buf);
}

static void run_check(void)
{
    uint32_t started = fw_uptime_s();
    fw_set_state(FW_UPDATE_CHECKING, ESP_OK);

    char repo[FW_UPDATE_REPO_MAX];
    char url[384];
    fw_repo(repo, sizeof(repo));

    char etag_in[sizeof(g_fw.etag)];
    fw_lock();
    bool have = g_fw.have_manifest;
    strlcpy(etag_in, g_fw.etag, sizeof(etag_in));
    fw_unlock();

    char *body = NULL;
    size_t len = 0;
    char etag[sizeof(g_fw.etag)] = "";
    int http_status = 0;
    fw_http_result_t r;
    if (fw_release_latest_url(url, sizeof(url), repo, CONFIG_FW_UPDATE_MANIFEST_ASSET)) {
        /* "Not modified" is only useful while this boot holds the accepted
         * manifest it refers to (fetched now, or kept from the last check). */
        r = fw_http_fetch(url, (have && etag_in[0] != '\0') ? etag_in : NULL, MANIFEST_CAP, &body, &len, etag,
                          sizeof(etag), &http_status);
    } else {
        ESP_LOGE(TAG, "no release URL for %s and %s", repo, CONFIG_FW_UPDATE_MANIFEST_ASSET);
        r = FW_HTTP_BAD_ANSWER;   /* unusable: waits for the next daily check */
    }

    fw_check_result_t result;
    esp_err_t err = ESP_OK;
    bool manifest_ok = false;
    switch (r) {
    case FW_HTTP_OK: {
        static fw_manifest_t m;   /* worker only; too large for the stack */
        int key = -1;
        fw_manifest_err_t merr = fw_manifest_read(body, len, g_fw.keys, g_fw.key_count, &m, &key);
        if (merr == FW_MANIFEST_OK) {
            fw_lock();
            g_fw.manifest = m;
            g_fw.have_manifest = true;
            strlcpy(g_fw.etag, etag, sizeof(g_fw.etag));
            fw_unlock();
            result = FW_CHECK_OK;
            manifest_ok = true;
            if (key >= 0) {
                ESP_LOGI(TAG, "manifest %s signed (key %d), %u receiver package(s)", m.version, key + 1,
                         (unsigned)m.gnss_count);
            } else {
                ESP_LOGI(TAG, "manifest %s from github.com/%s over HTTPS, %u receiver package(s)", m.version, repo,
                         (unsigned)m.gnss_count);
            }
            bool changed = strcmp(etag, g_fw.etag_saved) != 0;
            if (changed) {
                if (settings_set_state_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_ETAG, etag) == ESP_OK) {
                    strlcpy(g_fw.etag_saved, etag, sizeof(g_fw.etag_saved));
                }
            }
            /* Without an ETag a changed manifest cannot be told from the stored one: stored each time. */
            if (changed || etag[0] == '\0' || !g_fw.manifest_saved) {
                save_manifest(repo, etag, body, len);
            }
        } else {
            result = merr == FW_MANIFEST_ERR_NO_MEM ? FW_CHECK_NO_RESPONSE : FW_CHECK_UNUSABLE;
            err = (merr == FW_MANIFEST_ERR_SIGNATURE || merr == FW_MANIFEST_ERR_UNSIGNED) ? ESP_ERR_INVALID_CRC
                  : merr == FW_MANIFEST_ERR_NO_MEM                                       ? ESP_ERR_NO_MEM
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
        if (http_status == 404) {
            ESP_LOGW(TAG, "no release found at github.com/%s (HTTP 404): private, or no release yet", repo);
        } else {
            ESP_LOGW(TAG, "no usable release manifest at github.com/%s (HTTP %d)", repo, http_status);
        }
        break;
    case FW_HTTP_UNTRUSTED:
        /* An answer, but one that would leave the repository's releases on
         * GitHub's release hosts: the next daily check, like any unusable
         * answer. */
        result = FW_CHECK_UNUSABLE;
        err = ESP_ERR_NOT_ALLOWED;
        ESP_LOGE(TAG, "update check: GitHub redirected somewhere this firmware does not trust");
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
    fw_check_outcome_t outcome = fw_policy_check_outcome(r, http_status, manifest_ok);
    fw_lock();
    g_fw.last_result = result;
    g_fw.last_attempt_s = started;
    g_fw.last_outcome = outcome;
    g_fw.last_http_status = (http_status > 0 && http_status < 1000) ? (uint16_t)http_status : 0;
    g_fw.last_attempt_unix = trusted ? now : 0;
    if (result == FW_CHECK_OK) {
        g_fw.have_success = true;
        g_fw.last_success_s = started;
        if (trusted) {
            g_fw.last_check_unix = now;
        }
    }
    fw_unlock();
    if (result == FW_CHECK_OK && trusted &&
        settings_set_state_i64(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_LAST_CHECK, now) != ESP_OK) {
        ESP_LOGW(TAG, "time of the update check not stored");
    }
    if (result == FW_CHECK_OK) {
        log_banner();
    }
    fw_set_state(err == ESP_OK ? FW_UPDATE_IDLE : FW_UPDATE_FAILED, err);
}

_Static_assert(FW_OUTCOME_NONE == (int)FW_CHECK_RESULT_NONE && FW_OUTCOME_OK == (int)FW_CHECK_RESULT_OK &&
                   FW_OUTCOME_NO_RELEASE == (int)FW_CHECK_RESULT_NO_RELEASE &&
                   FW_OUTCOME_HTTP_ERROR == (int)FW_CHECK_RESULT_HTTP_ERROR &&
                   FW_OUTCOME_NO_RESPONSE == (int)FW_CHECK_RESULT_NO_RESPONSE &&
                   FW_OUTCOME_UNTRUSTED_REDIRECT == (int)FW_CHECK_RESULT_UNTRUSTED_REDIRECT &&
                   FW_OUTCOME_BAD_MANIFEST == (int)FW_CHECK_RESULT_BAD_MANIFEST,
               "check outcomes mirror fw_update_check_result_t");

/* The daily schedule's inputs now (call locked). */
static fw_sched_t sched_locked(bool trusted, int64_t now)
{
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
    return s;
}

static bool check_due(void)
{
    int64_t now = 0;
    bool trusted = fw_clock(&now);
    fw_lock();
    fw_sched_t s = sched_locked(trusted, now);
    bool idle = !g_fw.busy && g_fw.state != FW_UPDATE_REBOOT_PENDING && g_fw.run != FW_TRIAL_RUN_CONFIRMING &&
                g_fw.checks_enabled;
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
    case FW_JOB_RESTAGE_GNSS:
        err = fw_install_gnss_restage_job();
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

/* The trust settings of this build: keys, whether a signature is required,
 * and whether update checks can run at all. */
static void load_trust(void)
{
    const char *hex[FW_MANIFEST_MAX_KEYS] = { CONFIG_FW_UPDATE_MANIFEST_KEY_1, CONFIG_FW_UPDATE_MANIFEST_KEY_2 };
    g_fw.key_count = 0;
    g_fw.signature_required = false;
    for (size_t i = 0; i < FW_MANIFEST_MAX_KEYS; i++) {
        if (hex[i][0] == '\0') {
            continue;
        }
        /* Configured is enough to require signatures: a key that does not
         * parse must not turn this build into one that takes unsigned
         * manifests. */
        g_fw.signature_required = true;
        if (fw_manifest_key_from_hex(hex[i], &g_fw.keys[g_fw.key_count])) {
            g_fw.key_count++;
        } else {
            ESP_LOGE(TAG, "manifest key %u is not a P-256 public key in hex: ignored", (unsigned)(i + 1));
        }
    }
    g_fw.checks_enabled = fw_release_repo_ok(CONFIG_FW_UPDATE_GITHUB_REPO) &&
                          fw_manifest_asset_name_ok(CONFIG_FW_UPDATE_MANIFEST_ASSET) &&
                          (!g_fw.signature_required || g_fw.key_count > 0);
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
    load_trust();
    select_repo();

    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    g_fw.first_check_s = fw_sched_jitter_s(mac);

    int64_t last = 0;
    if (settings_get_i64(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_LAST_CHECK, &last) == ESP_OK) {
        g_fw.last_check_unix = last;
    }
    (void)settings_get_str(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_ETAG, g_fw.etag_saved, sizeof(g_fw.etag_saved));
    load_manifest();

    /* Trial and rollback bookkeeping; a failing trial image is reverted here
     * (and this does not return), before the steps that may be what fails. */
    fw_trial_boot();

    char repo[FW_UPDATE_REPO_MAX];
    fw_repo(repo, sizeof(repo));
    bool names_ok =
        fw_release_repo_ok(CONFIG_FW_UPDATE_GITHUB_REPO) && fw_manifest_asset_name_ok(CONFIG_FW_UPDATE_MANIFEST_ASSET);
    if (!names_ok) {
        g_fw.last_err = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "no valid release repository or manifest name in this build (\"%s\", \"%s\"): update checks "
                      "are off (manual uploads still work)",
                 CONFIG_FW_UPDATE_GITHUB_REPO, CONFIG_FW_UPDATE_MANIFEST_ASSET);
    } else if (!g_fw.checks_enabled) {
        g_fw.last_err = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGE(TAG, "this build requires signed manifests but none of its keys is usable: update checks are off "
                      "(manual uploads still work)");
    } else {
        ESP_LOGI(TAG, "releases from github.com/%s (%s), checked every %d h (first not before %u s), installs only "
                      "on request",
                 repo, g_fw.signature_required ? "signed manifests only" : "HTTPS to GitHub, size and SHA-256 per file",
                 CONFIG_FW_UPDATE_CHECK_INTERVAL_H, (unsigned)g_fw.first_check_s);
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

bool fw_update_checks_enabled(void)
{
    return g_fw.mtx != NULL && g_fw.checks_enabled;
}

bool fw_update_signature_required(void)
{
    return g_fw.signature_required;
}

const char *fw_update_release_repo(void)
{
    return g_fw.repo;
}

esp_err_t fw_update_check(void)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_fw.checks_enabled) {
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
        g_fw.have_manifest && !pkg_store_absent()   /* no package partition on this board: nothing to offer */
            ? fw_policy_gnss_offer(&g_fw.manifest, running_version(), st->receiver.model, st->receiver.build)
            : NULL;
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

bool fw_manifest_entry_for_journal(const um98x_flash_journal_t *j, fw_manifest_gnss_t *out)
{
    if (!g_fw.have_manifest) {
        return false;
    }
    for (unsigned i = 0; i < g_fw.manifest.gnss_count; i++) {
        const fw_manifest_gnss_t *g = &g_fw.manifest.gnss[i];
        if (g->file.size == j->size && memcmp(g->file.sha256, j->sha256, sizeof(g->file.sha256)) == 0 &&
            strcmp(g->model, j->pre_model) == 0) {
            *out = *g;
            return true;
        }
    }
    return false;
}

bool fw_update_has_journal_package(void)
{
    um98x_flash_journal_t j;
    if (g_fw.mtx == NULL || !station_flash_waits_for_package(&j)) {
        return false;
    }
    fw_manifest_gnss_t entry;
    fw_lock();
    bool have = fw_manifest_entry_for_journal(&j, &entry);
    fw_unlock();
    return have;
}

esp_err_t fw_update_restage_gnss(void)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    um98x_flash_journal_t j;
    if (!station_flash_waits_for_package(&j)) {
        return ESP_ERR_INVALID_STATE;
    }
    fw_manifest_gnss_t entry;
    esp_err_t err;
    fw_lock();
    if (!fw_manifest_entry_for_journal(&j, &entry)) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        fw_job_t job = { .kind = FW_JOB_RESTAGE_GNSS };
        err = queue_job_locked(&job);
    }
    fw_unlock();
    return err;
}

esp_err_t fw_update_get_status(fw_update_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->next_check_in_s = UINT32_MAX;
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
    out->last_attempt_unix = g_fw.last_attempt_unix;
    out->last_check_result = (fw_update_check_result_t)g_fw.last_outcome;
    out->last_http_status = g_fw.last_http_status;
    fw_sched_t sched = sched_locked(trusted, now);
    out->next_check_in_s = g_fw.checks_enabled ? fw_sched_wait_s(&sched) : UINT32_MAX;
    fw_trial_status_locked(out, trusted, now);
    if (g_fw.have_manifest) {
        strlcpy(out->latest_version, g_fw.manifest.version, sizeof(out->latest_version));
        fw_app_offer_t offer;
        const char *stage1 = g_fw.rec.stage == FW_TRIAL_REC_ROLLED_BACK ? g_fw.rec.version : "";
        fw_policy_app_offer(out->running_version, g_fw.manifest.version, is_bad_locked, &g_fw.bad, stage1, &offer);
        out->app_update_available = offer.available;
        out->latest_rolled_back = offer.rolled_back;
        const fw_manifest_gnss_t *g =
            st_ok && st->receiver.valid && !pkg_store_absent()
                ? fw_policy_gnss_offer(&g_fw.manifest, out->running_version, st->receiver.model, st->receiver.build)
                : NULL;
        if (g != NULL) {
            strlcpy(out->gnss_latest, g->version, sizeof(out->gnss_latest));
            out->gnss_latest_build = g->build;
            out->gnss_update_available = true;
            out->gnss_expected_downtime_s = station_expected_flash_downtime_s(g->file.size);
        }
        const fw_manifest_gnss_t *rel =
            st_ok && st->receiver.valid ? fw_policy_gnss_release(&g_fw.manifest, st->receiver.model) : NULL;
        if (rel != NULL) {
            strlcpy(out->gnss_release, rel->version, sizeof(out->gnss_release));
            out->gnss_release_build = rel->build;
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
