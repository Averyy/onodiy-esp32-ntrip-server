/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "pkg_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"

static const char *TAG = "pkg_store";

/* Custom partition type from partitions.csv (0x40-0xFE is application-defined). */
#define PKG_STORE_PARTITION_TYPE    ((esp_partition_type_t)0x40)
#define PKG_STORE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)
#define PKG_STORE_PARTITION_LABEL   "gnss_pkg"
#define PKG_STORE_HEADER_SIZE       4096u

/* Hashing uses PSA Crypto (Mbed TLS 4 / TF-PSA-Crypto in IDF v6.x); the
 * legacy mbedtls_sha256_* API is private there. */
_Static_assert(PSA_HASH_LENGTH(PSA_ALG_SHA_256) == sizeof(((pkg_store_info_t *)0)->sha256),
               "pkg_store digest size must match SHA-256");

static const esp_partition_t *s_part;

esp_err_t pkg_store_init(void)
{
    s_part = esp_partition_find_first(PKG_STORE_PARTITION_TYPE, PKG_STORE_PARTITION_SUBTYPE,
                                      PKG_STORE_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", PKG_STORE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    /* TODO: read and validate the header sector (state, pin). */
    ESP_LOGI(TAG, "partition '%s' at 0x%06x, %u KB payload capacity", s_part->label,
             (unsigned)s_part->address, (unsigned)(pkg_store_capacity() / 1024));
    return ESP_OK;
}

size_t pkg_store_capacity(void)
{
    if (s_part == NULL || s_part->size <= PKG_STORE_HEADER_SIZE) {
        return 0;
    }
    return s_part->size - PKG_STORE_HEADER_SIZE;
}

esp_err_t pkg_store_get_info(pkg_store_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pkg_store_begin(uint32_t size, const uint8_t expected_sha256[32], pkg_store_writer_t *out)
{
    (void)expected_sha256;
    if (out != NULL) {
        *out = NULL;
    }
    if (size > pkg_store_capacity()) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t pkg_store_write(pkg_store_writer_t w, const void *data, size_t len)
{
    (void)w;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t pkg_store_finish(pkg_store_writer_t w, const char *version, pkg_store_info_t *out)
{
    (void)w;
    (void)version;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void pkg_store_abort(pkg_store_writer_t w)
{
    (void)w;
}

esp_err_t pkg_store_read(uint32_t offset, void *buf, size_t len)
{
    (void)offset;
    (void)buf;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t pkg_store_verify(void)
{
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pkg_store_set_pinned(bool pinned)
{
    (void)pinned;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t pkg_store_invalidate(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
