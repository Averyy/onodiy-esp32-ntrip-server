/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * HTTPS transfers from GitHub Releases. Every request uses the CA bundle,
 * CONFIG_FW_UPDATE_HTTP_BUF_SIZE for the receive and the transmit buffer
 * (the asset redirects point at signed CDN URLs longer than 1000
 * characters, and the request line is built in the transmit buffer) and
 * follows at most CONFIG_FW_UPDATE_MAX_REDIRECTS redirects, HTTPS only (the
 * client refuses a redirect from HTTPS to anything else).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "fw_internal.h"
#include "pkg_store.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

static const char *TAG = "fw_update";

#define CHUNK 4096u

typedef struct {
    char *etag;
    size_t etag_size;
} hdr_ctx_t;

static char s_user_agent[64];

/* Large transfer buffers go to PSRAM when there is any. */
static void *buf_alloc(size_t n)
{
#if CONFIG_SPIRAM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        return p;
    }
#endif
    return malloc(n);
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    hdr_ctx_t *ctx = (hdr_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx != NULL && ctx->etag != NULL && evt->header_key != NULL &&
        evt->header_value != NULL && strcasecmp(evt->header_key, "ETag") == 0) {
        strlcpy(ctx->etag, evt->header_value, ctx->etag_size);
    }
    return ESP_OK;
}

static void client_config(esp_http_client_config_t *cfg, const char *url, hdr_ctx_t *ctx)
{
    if (s_user_agent[0] == '\0') {
        snprintf(s_user_agent, sizeof(s_user_agent), "onocoydiy-esp-um982/%s", esp_app_get_description()->version);
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
    cfg->buffer_size = CONFIG_FW_UPDATE_HTTP_BUF_SIZE;
    cfg->buffer_size_tx = CONFIG_FW_UPDATE_HTTP_BUF_SIZE;
    cfg->max_redirection_count = CONFIG_FW_UPDATE_MAX_REDIRECTS;
    cfg->timeout_ms = CONFIG_FW_UPDATE_HTTP_TIMEOUT_MS;
    cfg->user_agent = s_user_agent;
    cfg->event_handler = on_http_event;
    cfg->user_data = ctx;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/*
 * Send the request and follow redirects. On FW_HTTP_OK the final answer's
 * headers are read: *status is its HTTP status, *length its Content-Length
 * (-1 when chunked or absent).
 */
static fw_http_result_t open_follow(esp_http_client_handle_t c, hdr_ctx_t *ctx, int *status, int64_t *length)
{
    for (int redirects = 0;; redirects++) {
        if (ctx != NULL && ctx->etag != NULL) {
            ctx->etag[0] = '\0';
        }
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            return FW_HTTP_NO_RESPONSE;
        }
        int64_t len = esp_http_client_fetch_headers(c);
        if (len < 0) {
            ESP_LOGW(TAG, "no answer");
            return FW_HTTP_NO_RESPONSE;
        }
        int st = esp_http_client_get_status_code(c);
        if (is_redirect(st)) {
            if (redirects >= CONFIG_FW_UPDATE_MAX_REDIRECTS) {
                ESP_LOGW(TAG, "too many redirects");
                return FW_HTTP_BAD_ANSWER;
            }
            int flushed = 0;
            (void)esp_http_client_flush_response(c, &flushed);
            err = esp_http_client_set_redirection(c);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "redirect not followed: %s", esp_err_to_name(err));
                return FW_HTTP_BAD_ANSWER;
            }
            continue;
        }
        *status = st;
        *length = (esp_http_client_is_chunked_response(c) || len == 0) ? -1 : len;
        return FW_HTTP_OK;
    }
}

fw_http_result_t fw_http_fetch(const char *url, const char *etag_in, size_t cap, char **out, size_t *out_len,
                               char *etag_out, size_t etag_size)
{
    *out = NULL;
    *out_len = 0;
    hdr_ctx_t ctx = { .etag = etag_out, .etag_size = etag_size };
    esp_http_client_config_t cfg;
    client_config(&cfg, url, &ctx);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return FW_HTTP_LOCAL_ERROR;
    }
    if (etag_in != NULL) {
        (void)esp_http_client_set_header(c, "If-None-Match", etag_in);
    }
    int status = 0;
    int64_t length = -1;
    char *buf = NULL;
    size_t total = 0;
    fw_http_result_t r = open_follow(c, &ctx, &status, &length);
    if (r == FW_HTTP_OK && status == 304) {
        r = etag_in != NULL ? FW_HTTP_NOT_MODIFIED : FW_HTTP_BAD_ANSWER;
    } else if (r == FW_HTTP_OK && status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        r = FW_HTTP_BAD_ANSWER;
    } else if (r == FW_HTTP_OK && length > (int64_t)cap) {
        ESP_LOGW(TAG, "answer too large (%lld bytes)", (long long)length);
        r = FW_HTTP_BAD_ANSWER;
    }
    if (r == FW_HTTP_OK) {
        buf = buf_alloc(cap + 1u);
        if (buf == NULL) {
            r = FW_HTTP_LOCAL_ERROR;
        }
    }
    while (r == FW_HTTP_OK) {
        if (total == cap) {
            char extra;
            int n = esp_http_client_read(c, &extra, 1);
            if (n > 0) {
                ESP_LOGW(TAG, "answer larger than %u bytes", (unsigned)cap);
                r = FW_HTTP_BAD_ANSWER;
            } else if (n < 0) {
                r = FW_HTTP_NO_RESPONSE;
            }
            break;
        }
        int n = esp_http_client_read(c, buf + total, (int)(cap - total));
        if (n < 0) {
            r = FW_HTTP_NO_RESPONSE;
        } else if (n == 0) {
            break;
        } else {
            total += (size_t)n;
        }
    }
    if (r == FW_HTTP_OK && !esp_http_client_is_complete_data_received(c)) {
        r = FW_HTTP_NO_RESPONSE;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (r != FW_HTTP_OK) {
        free(buf);
        return r;
    }
    buf[total] = '\0';
    *out = buf;
    *out_len = total;
    return FW_HTTP_OK;
}

esp_err_t fw_http_download_package(const char *url, const fw_manifest_file_t *file, const char *label)
{
    esp_http_client_config_t cfg;
    client_config(&cfg, url, NULL);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    int64_t length = -1;
    esp_err_t err = ESP_OK;
    if (open_follow(c, NULL, &status, &length) != FW_HTTP_OK) {
        err = ESP_ERR_TIMEOUT;
    } else if (status != 200) {
        ESP_LOGE(TAG, "receiver package: HTTP %d", status);
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (length >= 0 && length != (int64_t)file->size) {
        ESP_LOGE(TAG, "receiver package is %lld bytes, the manifest says %u", (long long)length,
                 (unsigned)file->size);
        err = ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = NULL;
    pkg_store_writer_t w = NULL;
    if (err == ESP_OK) {
        buf = buf_alloc(CHUNK);
        err = buf == NULL ? ESP_ERR_NO_MEM : pkg_store_begin(file->size, file->sha256, &w);
    }
    uint32_t total = 0;
    while (err == ESP_OK) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            err = ESP_ERR_TIMEOUT;
        } else if (n == 0) {
            break;
        } else if ((uint32_t)n > file->size - total) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = pkg_store_write(w, buf, (size_t)n);
            total += (uint32_t)n;
            fw_set_progress((uint8_t)((uint64_t)total * 100u / file->size));
        }
    }
    if (err == ESP_OK && (total != file->size || !esp_http_client_is_complete_data_received(c))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);
    if (w != NULL) {
        if (err == ESP_OK) {
            pkg_store_info_t info;
            err = pkg_store_finish(w, label, &info);   /* size and SHA-256 against the manifest */
        } else {
            pkg_store_abort(w);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receiver package not staged: %s", esp_err_to_name(err));
    }
    return err;
}

/* SHA-256 of the first `size` bytes of `part`, read back from flash.
 * esp_ota_write writes straight through (it holds back bytes only with flash
 * encryption, and then only below 16; app images are multiples of 16). */
static esp_err_t hash_partition(const esp_partition_t *part, uint32_t size, uint8_t out[32])
{
    uint8_t *buf = buf_alloc(CHUNK);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    psa_hash_operation_t op = psa_hash_operation_init();
    esp_err_t err = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (uint32_t off = 0; err == ESP_OK && off < size; off += CHUNK) {
        size_t n = (size - off) < CHUNK ? (size - off) : CHUNK;
        err = esp_partition_read(part, off, buf, n);
        if (err == ESP_OK && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
    }
    size_t len = 0;
    if (err == ESP_OK && (psa_hash_finish(&op, out, 32, &len) != PSA_SUCCESS || len != 32)) {
        err = ESP_FAIL;
    }
    psa_hash_abort(&op);
    free(buf);
    return err;
}

esp_err_t fw_http_download_app(const char *url, const fw_manifest_file_t *file, const char *version)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (file->size > part->size) {
        ESP_LOGE(TAG, "app image of %u bytes does not fit %s", (unsigned)file->size, part->label);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_config_t http;
    client_config(&http, url, NULL);
    esp_https_ota_config_t ota = {
        .http_config = &http,
        .bulk_flash_erase = false,   /* erase sector by sector while writing */
#if CONFIG_SPIRAM
        .buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
#endif
        .partition.staging = part,
    };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app download did not start: %s", esp_err_to_name(err));
        return err;
    }

    /* The image must be the release the manifest names, built by this project. */
    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(h, &desc);
    if (err == ESP_OK && (strncmp(desc.version, version, sizeof(desc.version)) != 0 ||
                          strncmp(desc.project_name, esp_app_get_description()->project_name,
                                  sizeof(desc.project_name)) != 0)) {
        ESP_LOGE(TAG, "downloaded image is %s %.32s, the manifest names %s", desc.project_name, desc.version,
                 version);
        err = ESP_ERR_INVALID_VERSION;
    }
    int announced = esp_https_ota_get_image_size(h);
    if (err == ESP_OK && announced > 0 && (uint32_t)announced != file->size) {
        ESP_LOGE(TAG, "app image is %d bytes, the manifest says %u", announced, (unsigned)file->size);
        err = ESP_ERR_INVALID_SIZE;
    }
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        int done = esp_https_ota_get_image_len_read(h);
        if (done > 0 && (uint32_t)done > file->size) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (done > 0) {
            fw_set_progress((uint8_t)((uint64_t)done * 100u / file->size));
        }
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
    }
    if (err == ESP_OK && (!esp_https_ota_is_complete_data_received(h) ||
                          (uint32_t)esp_https_ota_get_image_len_read(h) != file->size)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        /* Check what is in flash, before anything is activated. */
        uint8_t digest[32];
        err = hash_partition(part, file->size, digest);
        if (err == ESP_OK && memcmp(digest, file->sha256, sizeof(digest)) != 0) {
            ESP_LOGE(TAG, "app image SHA-256 does not match the manifest");
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err != ESP_OK) {
        (void)esp_https_ota_abort(h);
        ESP_LOGE(TAG, "app download failed: %s", esp_err_to_name(err));
        return err;
    }
    /* esp_ota_end (image validation) and esp_ota_set_boot_partition. */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app image not activated: %s", esp_err_to_name(err));
    }
    return err;
}
