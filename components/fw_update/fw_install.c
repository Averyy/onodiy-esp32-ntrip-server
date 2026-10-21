/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Installs started by the user: the app image from the release (download)
 * or from a manual upload, and the receiver package download that ends in
 * the station's flash request.
 */
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "fw_internal.h"
#include "network.h"
#include "ntrip_server.h"
#include "pkg_store.h"
#include "psa/crypto.h"
#include "station.h"

static const char *TAG = "fw_update";

void fw_capture_baseline(uint8_t *baseline, uint8_t *slots)
{
    *baseline = 0;
    *slots = 0;
    if (network_is_online()) {
        *baseline |= FW_BASELINE_STA_IP;
    }
    if (fw_hub_fed_now()) {
        *baseline |= FW_BASELINE_HUB_FED;
    }
    ntrip_caster_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return;
    }
    size_t n = ntrip_server_slot_count();
    for (size_t i = 0; i < n && i < 8; i++) {
        if (ntrip_server_get_status(i, st) == ESP_OK && st->state == NTRIP_STATE_STREAMING) {
            *slots |= (uint8_t)(1u << i);
        }
    }
    free(st);
}

/* The new image is active: record its trial, then reboot into it. The update
 * slot stays claimed until the reboot, so no receiver flash starts meanwhile. */
static void activated(const char *version, uint8_t baseline, uint8_t slots, lifecycle_inhibit_t inhibit, bool same_image)
{
    fw_trial_record_install(version, baseline, slots, same_image);
    (void)diag_journal_append(DIAG_EV_APP_UPDATE, ESP_OK, 0);
    fw_set_state(FW_UPDATE_REBOOT_PENDING, ESP_OK);
    ESP_LOGW(TAG, "app %s installed: rebooting into it", version);
    /* Release only now that the boot partition is set (the reboot queued
     * behind it must not cut the install short), then reboot. */
    lifecycle_inhibit_release(inhibit);
    (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_APP_UPDATE);
}

/* The inactive slot may have been written (it held the rollback image):
 * have the worker look at it again before rollback is offered. */
static void slot_touched(void)
{
    fw_lock();
    g_fw.rollback_known = false;
    fw_unlock();
}

static void failed(esp_err_t err, lifecycle_inhibit_t inhibit)
{
    slot_touched();
    (void)diag_journal_append(DIAG_EV_APP_UPDATE, (int32_t)err, 0);
    lifecycle_inhibit_release(inhibit);
    lifecycle_update_release(LIFECYCLE_UPDATE_APP);
    fw_set_state(FW_UPDATE_FAILED, err);
}

/* Claim the update slot and take the reboot inhibit, before the first image byte. */
static esp_err_t claim(lifecycle_inhibit_t *inhibit)
{
    if (lifecycle_update_claim(LIFECYCLE_UPDATE_APP) != ESP_OK) {
        ESP_LOGW(TAG, "app install refused: a receiver update holds the update slot");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lifecycle_inhibit_acquire("app update", inhibit);
    if (err != ESP_OK) {
        lifecycle_update_release(LIFECYCLE_UPDATE_APP);
    }
    return err;
}

/* ---- release download --------------------------------------------------------------- */

esp_err_t fw_install_app_job(void)
{
    static fw_manifest_t m;   /* worker only */
    fw_lock();
    bool have = g_fw.have_manifest;
    m = g_fw.manifest;
    bool blocked = fw_trial_blocks_install_locked();
    fw_unlock();
    if (!have || blocked) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    lifecycle_inhibit_t inhibit;
    esp_err_t err = claim(&inhibit);
    if (err != ESP_OK) {
        fw_set_state(FW_UPDATE_FAILED, err);
        return err;
    }
    uint8_t baseline;
    uint8_t slots;
    fw_capture_baseline(&baseline, &slots);

    char url[384];
    if (!fw_asset_url(m.version, m.app.asset, url, sizeof(url))) {
        failed(ESP_ERR_INVALID_SIZE, inhibit);
        return ESP_ERR_INVALID_SIZE;
    }
    fw_set_state(FW_UPDATE_DOWNLOADING_APP, ESP_OK);
    ESP_LOGI(TAG, "downloading app %s (%u bytes)", m.version, (unsigned)m.app.size);
    err = fw_http_download_app(url, &m.app, m.version);
    if (err != ESP_OK) {
        failed(err, inhibit);
        return err;
    }
    activated(m.version, baseline, slots, inhibit, false);
    return ESP_OK;
}

/* ---- manual upload ---------------------------------------------------------------- */

struct fw_update_upload {
    bool open;
    const esp_partition_t *part;
    esp_ota_handle_t ota;
    psa_hash_operation_t hash;
    uint32_t size;
    uint32_t written;
    lifecycle_inhibit_t inhibit;
    uint8_t baseline;
    uint8_t slots;
};

static struct fw_update_upload s_up;

/* Close the upload; an OTA handle still open (not ended) is aborted. */
static void upload_close(void)
{
    if (s_up.ota != 0) {
        (void)esp_ota_abort(s_up.ota);
    }
    psa_hash_abort(&s_up.hash);
    memset(&s_up, 0, sizeof(s_up));
    fw_lock();
    g_fw.busy = false;
    fw_unlock();
}

esp_err_t fw_update_upload_app_begin(uint32_t size, fw_update_upload_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    fw_lock();
    bool refuse = g_fw.busy || g_fw.state == FW_UPDATE_REBOOT_PENDING || fw_trial_blocks_install_locked();
    if (!refuse) {
        g_fw.busy = true;
        g_fw.job = 0;
    }
    fw_unlock();
    if (refuse) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_up, 0, sizeof(s_up));
    s_up.hash = psa_hash_operation_init();
    esp_err_t err = claim(&s_up.inhibit);
    if (err != ESP_OK) {
        upload_close();
        return err;
    }
    s_up.part = esp_ota_get_next_update_partition(NULL);
    if (s_up.part == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (size > s_up.part->size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        fw_capture_baseline(&s_up.baseline, &s_up.slots);
        err = esp_ota_begin(s_up.part, OTA_WITH_SEQUENTIAL_WRITES, &s_up.ota);
    }
    if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&s_up.hash, PSA_ALG_SHA_256) != PSA_SUCCESS)) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        lifecycle_inhibit_t inhibit = s_up.inhibit;
        upload_close();
        failed(err, inhibit);
        return err;
    }
    s_up.open = true;
    s_up.size = size;
    fw_set_state(FW_UPDATE_UPLOADING_APP, ESP_OK);
    ESP_LOGI(TAG, "receiving an app image (%u bytes) into %s", (unsigned)size, s_up.part->label);
    *out = &s_up;
    return ESP_OK;
}

esp_err_t fw_update_upload_app_write(fw_update_upload_t u, const void *data, size_t len)
{
    if (u != &s_up || !s_up.open || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_up.size > 0 && len > s_up.size - s_up.written) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(s_up.ota, data, len);
    if (err == ESP_OK && psa_hash_update(&s_up.hash, data, len) != PSA_SUCCESS) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        s_up.written += (uint32_t)len;
        if (s_up.size > 0) {
            fw_set_progress((uint8_t)((uint64_t)s_up.written * 100u / s_up.size));
        }
    }
    return err;
}

esp_err_t fw_update_upload_app_finish(fw_update_upload_t u, uint8_t sha256_out[32])
{
    if (u != &s_up || !s_up.open) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    uint8_t digest[32];
    size_t digest_len = 0;
    if (s_up.size > 0 && s_up.written != s_up.size) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (psa_hash_finish(&s_up.hash, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
               digest_len != sizeof(digest)) {
        err = ESP_FAIL;
    }
    lifecycle_inhibit_t inhibit = s_up.inhibit;
    uint8_t baseline = s_up.baseline;
    uint8_t slots = s_up.slots;
    const esp_partition_t *part = s_up.part;
    if (err != ESP_OK) {
        upload_close();
        failed(err, inhibit);
        return err;
    }
    /* esp_ota_end validates the image (and its signature, if app signing is
     * ever enabled) and frees the handle in every case. */
    err = esp_ota_end(s_up.ota);
    s_up.ota = 0;
    esp_app_desc_t desc;
    if (err == ESP_OK) {
        err = esp_ota_get_partition_description(part, &desc);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        upload_close();
        ESP_LOGE(TAG, "uploaded app image rejected: %s", esp_err_to_name(err));
        failed(err, inhibit);
        return err;
    }
    if (sha256_out != NULL) {
        memcpy(sha256_out, digest, sizeof(digest));
    }
    fw_lock();
    bool bad = fw_bad_contains(&g_fw.bad, desc.version);
    fw_unlock();
    if (bad) {
        ESP_LOGW(TAG, "uploaded %s was rolled back on this station before: installing it anyway", desc.version);
    }
    /* The image running now, again (the web asks the user first): identical
     * code needs only the bootloader's first-minute check, no verify window
     * and no crash watch, so receiver updates are not held back for a day. */
    bool same = fw_update_same_as_running(desc.app_elf_sha256);
    if (same) {
        ESP_LOGW(TAG, "uploaded %s is the image already running: reinstalled at the user's request", desc.version);
    }
    /* REBOOT_PENDING before fw_update is free again: nothing else starts now. */
    activated(desc.version, baseline, slots, inhibit, same);
    upload_close();
    return ESP_OK;
}

void fw_update_upload_app_abort(fw_update_upload_t u)
{
    if (u != &s_up || !s_up.open) {
        return;
    }
    lifecycle_inhibit_t inhibit = s_up.inhibit;
    upload_close();
    slot_touched();
    lifecycle_inhibit_release(inhibit);
    lifecycle_update_release(LIFECYCLE_UPDATE_APP);
    fw_set_state(FW_UPDATE_IDLE, ESP_OK);
    ESP_LOGW(TAG, "app upload abandoned");
}

bool fw_update_same_as_running(const uint8_t elf_sha256[32])
{
    if (elf_sha256 == NULL) {
        return false;
    }
    const esp_app_desc_t *run = esp_app_get_description();
    static const uint8_t zero[32] = { 0 };
    return memcmp(run->app_elf_sha256, zero, sizeof(zero)) != 0 &&
           memcmp(run->app_elf_sha256, elf_sha256, sizeof(run->app_elf_sha256)) == 0;
}

/* ---- receiver package ------------------------------------------------------------- */

esp_err_t fw_install_gnss_restage_job(void)
{
    static fw_manifest_gnss_t entry;   /* worker only */
    um98x_flash_journal_t j;
    bool waits = station_flash_waits_for_package(&j);
    fw_lock();
    bool have = waits && fw_manifest_entry_for_journal(&j, &entry);
    char tag[sizeof(g_fw.manifest.version)];
    strlcpy(tag, g_fw.manifest.version, sizeof(tag));
    fw_unlock();
    if (!have) {
        /* Something changed since the user asked (the journal, the manifest). */
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    char url[384];
    if (!fw_asset_url(tag, entry.file.asset, url, sizeof(url))) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    fw_set_state(FW_UPDATE_DOWNLOADING_GNSS, ESP_OK);
    ESP_LOGW(TAG, "downloading receiver firmware %s again for the interrupted update (%u bytes)", entry.version,
             (unsigned)entry.file.size);
    esp_err_t err = fw_http_download_package(url, &entry.file, entry.version);
    if (err == ESP_OK) {
        err = pkg_store_verify();
    }
    if (err == ESP_OK) {
        err = station_flash_retry_restaged();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the interrupted receiver update was not retried: %s", esp_err_to_name(err));
        pkg_store_info_t pkg;
        if (pkg_store_get_info(&pkg) == ESP_OK && !pkg.pinned) {
            (void)pkg_store_release();   /* nothing stays staged without a session that uses it */
        }
    }
    fw_set_state(err == ESP_OK ? FW_UPDATE_IDLE : FW_UPDATE_FAILED, err);
    return err;
}

esp_err_t fw_install_gnss_job(uint32_t build, bool cross_layout_ack)
{
    static fw_manifest_gnss_t entry;   /* worker only */
    station_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    bool ok = station_get_status(st) == ESP_OK && st->state == STATION_STATE_STREAMING &&
              !st->flash_journal_open && !lifecycle_app_trial_active() && !lifecycle_in_safe_mode() &&
              lifecycle_update_active() == LIFECYCLE_UPDATE_NONE && !pkg_store_absent();
    fw_lock();
    const fw_manifest_gnss_t *g =
        (ok && g_fw.have_manifest)
            ? fw_policy_gnss_offer(&g_fw.manifest, esp_app_get_description()->version, st->receiver.model,
                                   st->receiver.build)
            : NULL;
    if (g != NULL) {
        entry = *g;
    }
    fw_unlock();
    free(st);
    if (g == NULL || entry.build != build) {
        /* Something changed since the user confirmed. */
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    pkg_store_info_t pkg;
    if (pkg_store_get_info(&pkg) == ESP_OK && pkg.pinned) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (entry.file.size > pkg_store_capacity()) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    char tag[sizeof(g_fw.manifest.version)];
    fw_lock();
    strlcpy(tag, g_fw.manifest.version, sizeof(tag));
    fw_unlock();
    char url[384];
    if (!fw_asset_url(tag, entry.file.asset, url, sizeof(url))) {
        fw_set_state(FW_UPDATE_FAILED, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    fw_set_state(FW_UPDATE_DOWNLOADING_GNSS, ESP_OK);
    ESP_LOGI(TAG, "downloading receiver firmware %s (%u bytes)", entry.version, (unsigned)entry.file.size);
    esp_err_t err = fw_http_download_package(url, &entry.file, entry.version);
    if (err == ESP_OK) {
        /* The digest was checked on the stream; check what is in flash too. */
        err = pkg_store_verify();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "staged receiver package does not read back intact: %s", esp_err_to_name(err));
            (void)pkg_store_release();
        }
    }
    if (err == ESP_OK) {
        station_flash_request_t req;
        memset(&req, 0, sizeof(req));
        memcpy(req.sha256, entry.file.sha256, sizeof(req.sha256));
        req.size = entry.file.size;
        strlcpy(req.expected_fw, entry.version, sizeof(req.expected_fw));
        req.expected_build = entry.build;
        req.cross_layout_confirmed = cross_layout_ack;
        if (station_sg_test_running()) {
            /* Started while the package downloaded: the comparison owns the
             * receiver until it ends (the web refuses an update during one). */
            ESP_LOGE(TAG, "a signal-group comparison is running: receiver update not started (start it again "
                          "when the comparison has ended)");
        }
        err = station_request_flash(&req);
        bool checking = false;
        if (err == ESP_ERR_NOT_SUPPORTED) {
            /* Receiver flashing is not enabled in this build: the station
             * checks the package against every rule instead (nothing is sent
             * to the receiver) and releases it afterwards. */
            checking = station_request_package_check(&req) == ESP_OK;
            ESP_LOGW(TAG, "receiver flashing is not enabled in this build: %s",
                     checking ? "the downloaded package is checked, then released" : "package released");
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "the station did not take the receiver update (%s): package released",
                     esp_err_to_name(err));
        }
        if (err != ESP_OK && !checking) {
            /* Nothing stays staged without a session that uses it. */
            esp_err_t rel = pkg_store_release();
            if (rel != ESP_OK) {
                ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
            }
        }
    }
    fw_set_state(err == ESP_OK ? FW_UPDATE_IDLE : FW_UPDATE_FAILED, err);
    return err;
}
