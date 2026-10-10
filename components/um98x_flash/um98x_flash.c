/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * PROVISIONAL stub. The journal (codec in flash_journal.c, persistence here)
 * is implemented; validation, loader entry and the XMODEM-1K sender are not.
 * The pre-flight rule checks and the sender go in their own pure files
 * behind callbacks so they can be unit tested on the host (mutated copies of
 * a known-good package; a simulated receiver).
 */
#include "um98x_flash.h"

#include <string.h>

#include "esp_log.h"
#include "flash_journal.h"
#include "pkg_store.h"
#include "settings.h"

static const char *TAG = "um98x_flash";

_Static_assert(UM98X_FLASH_JOURNAL_VERSION == FLASH_JOURNAL_VERSION, "journal version mismatch");

static um98x_flash_progress_t s_progress;

esp_err_t um98x_flash_init(void)
{
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.phase = UM98X_FLASH_IDLE;
    ESP_LOGI(TAG, "stub");
    return ESP_OK;
}

esp_err_t um98x_flash_prepare(const um98x_flash_expect_t *expect, um98x_flash_pkg_facts_t *out)
{
    if (expect == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->layout = -1;
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    /* TODO: one PSRAM buffer (refuse on allocation failure), read the
     * package into it, run every pre-flight rule on that copy. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_flash_inspect_staged(um98x_flash_pkg_facts_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->layout = -1;
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    out->size = info.size;
    memcpy(out->sha256, info.sha256, sizeof(out->sha256));
    /* TODO: model string, partition layout and date stamps from the header blocks. */
    return ESP_ERR_NOT_SUPPORTED;
}

void um98x_flash_discard(void)
{
    /* Nothing is allocated until prepare is implemented. */
}

esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req)
{
    (void)raw_lease;
    (void)req;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_flash_race(gnss_link_lease_t raw_lease, uint32_t max_ms)
{
    (void)raw_lease;
    (void)max_ms;
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

esp_err_t um98x_flash_journal_get(um98x_flash_journal_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!settings_is_available()) {
        /* Degraded settings: whether a session is open is unknown. */
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[FLASH_JOURNAL_READ_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        /* A read error or a record larger than any this firmware knows: it
         * may hide an interrupted flash. */
        return ESP_ERR_INVALID_SIZE;
    }
    switch (flash_journal_decode(buf, len, out)) {
    case FLASH_JOURNAL_OK:
        return ESP_OK;
    case FLASH_JOURNAL_UNKNOWN_VERSION:
        return ESP_ERR_INVALID_VERSION;
    default:
        return ESP_ERR_INVALID_SIZE;
    }
}

esp_err_t um98x_flash_journal_put(const um98x_flash_journal_t *j)
{
    if (j == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[FLASH_JOURNAL_V1_MAX];
    size_t len = flash_journal_encode(j, buf, sizeof(buf));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return settings_set_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, buf, len);
}

esp_err_t um98x_flash_journal_close(void)
{
    esp_err_t err = settings_erase_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

bool um98x_flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset)
{
    return flash_journal_needs_race(j, receiver_was_reset);
}
