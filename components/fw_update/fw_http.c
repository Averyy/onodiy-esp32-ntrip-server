/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * HTTPS transfers from GitHub Releases: the manifest fetch, the receiver
 * package download and the app image download all go through one request
 * path (open_follow):
 *  - the server certificate is checked against the CA bundle (and its name
 *    against the host), never skipped, and so are the dates of every
 *    certificate in the chain, against the station's clock (cert_time.h;
 *    Mbed TLS itself ignores them in this build): no transfer starts before
 *    the clock is trusted;
 *  - before every connection, the first and each redirect, the URL the
 *    client is about to use must be https on port 443 to one of GitHub's
 *    release hosts, without user info, and on github.com one of this
 *    repository's release paths (release_url.h); anything else, a redirect
 *    off https included, ends the transfer before it connects
 *    (FW_HTTP_UNTRUSTED);
 *  - at most CONFIG_FW_UPDATE_MAX_REDIRECTS redirects;
 *  - CONFIG_FW_UPDATE_HTTP_BUF_SIZE for the receive and the transmit buffer
 *    (the asset redirects point at signed CDN URLs longer than 1000
 *    characters, and the request line is built in the transmit buffer).
 * Redirects are followed here, never inside the HTTP client or by a helper
 * that follows them on its own (esp_https_ota does), so no hop escapes the
 * host check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cert_time.h"
#include "fw_internal.h"
#include "pkg_store.h"
#include "psa/crypto.h"
#include "release_url.h"
#include "sdkconfig.h"

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "update transfers check the server certificate against the CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)"
#endif
#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY || CONFIG_ESP_TLS_INSECURE
#error "update transfers need server certificate verification (CONFIG_ESP_TLS_INSECURE must stay off)"
#endif

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

/*
 * The CA bundle's verify callback, saved when it is attached to a
 * connection: it is the same function every time, and only the update worker
 * opens these connections.
 */
static int (*s_bundle_verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
static void *s_bundle_verify_ctx;

/* Every certificate of the chain, from the top: its dates, then the bundle. */
static int verify_cert(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    int64_t now = 0;
    bool clock_ok = fw_clock(&now);
    uint32_t bad = fw_cert_time_flags(crt, clock_ok, now);
    if (bad != 0) {
        ESP_LOGE(TAG, "server certificate %d of the chain refused: %s", depth,
                 (bad & MBEDTLS_X509_BADCERT_OTHER)     ? "no trusted clock to check its dates"
                 : (bad & MBEDTLS_X509_BADCERT_EXPIRED) ? "expired"
                                                        : "not valid yet");
        *flags |= bad;   /* the bundle leaves a flagged certificate failed */
    }
    return s_bundle_verify != NULL ? s_bundle_verify(s_bundle_verify_ctx, crt, depth, flags) : 0;
}

/* esp_crt_bundle_attach, with the date check in front of its verify callback. */
static esp_err_t attach_bundle(void *conf)
{
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err != ESP_OK || conf == NULL) {
        return err;
    }
    mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
    s_bundle_verify = ssl_conf->MBEDTLS_PRIVATE(f_vrfy);
    s_bundle_verify_ctx = ssl_conf->MBEDTLS_PRIVATE(p_vrfy);
    mbedtls_ssl_conf_verify(ssl_conf, verify_cert, NULL);
    return ESP_OK;
}

static void client_config(esp_http_client_config_t *cfg, const char *url, hdr_ctx_t *ctx)
{
    if (s_user_agent[0] == '\0') {
        snprintf(s_user_agent, sizeof(s_user_agent), "onodiy/%s", esp_app_get_description()->version);
    }
    memset(cfg, 0, sizeof(*cfg));   /* skip_cert_common_name_check stays false */
    cfg->url = url;
    cfg->crt_bundle_attach = attach_bundle;
    cfg->buffer_size = CONFIG_FW_UPDATE_HTTP_BUF_SIZE;
    cfg->buffer_size_tx = CONFIG_FW_UPDATE_HTTP_BUF_SIZE;
    cfg->max_redirection_count = CONFIG_FW_UPDATE_MAX_REDIRECTS;
    cfg->disable_auto_redirect = true;   /* open_follow decides about every redirect */
    cfg->timeout_ms = CONFIG_FW_UPDATE_HTTP_TIMEOUT_MS;
    cfg->user_agent = s_user_agent;
    cfg->event_handler = on_http_event;
    cfg->user_data = ctx;
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* The URL the client connects to next is one of the repository's releases
 * on a release host (release_url.h). */
static bool target_trusted(esp_http_client_handle_t c)
{
    char url[FW_RELEASE_HOP_CHECK_LEN];
    char repo[FW_UPDATE_REPO_MAX];
    fw_repo(repo, sizeof(repo));
    if (esp_http_client_get_url(c, url, sizeof(url)) != ESP_OK) {
        return false;
    }
    url[sizeof(url) - 1] = '\0';
    /* The client keeps a URL's user info apart from the URL it prints (and
     * keeps it for later hops), so it is checked on its own. */
    char *user = NULL;
    char *pass = NULL;
    if (esp_http_client_get_username(c, &user) != ESP_OK || esp_http_client_get_password(c, &pass) != ESP_OK ||
        user != NULL || pass != NULL) {
        ESP_LOGE(TAG, "refusing to connect: the URL carries user info");
        return false;
    }
    if (fw_release_hop_ok(url, repo)) {
        return true;
    }
    if (fw_release_url_ok(url)) {
        /* github.com, outside this repository's releases (a renamed or moved
         * repository redirects there). The printed URL has no query. */
        ESP_LOGE(TAG, "refusing to connect to %s: not a release of %s", url, repo);
        return false;
    }
    /* Log the scheme and host only: the asset CDN's paths are not ours to log. */
    const char *host = strstr(url, "://");
    host = host != NULL ? host + 3 : url;
    ESP_LOGE(TAG, "refusing to connect to %.*s: not a GitHub release host", (int)strcspn(host, "/?#"), host);
    return false;
}

/*
 * Send the request and follow redirects. On FW_HTTP_OK the final answer's
 * headers are read: *status is its HTTP status, *length its Content-Length
 * (-1 when chunked or absent).
 */
static fw_http_result_t open_follow(esp_http_client_handle_t c, hdr_ctx_t *ctx, int *status, int64_t *length)
{
    int64_t now = 0;
    if (!fw_clock(&now)) {
        /* The handshake would fail anyway (verify_cert): say why. */
        ESP_LOGW(TAG, "clock not set yet: server certificates cannot be checked, not connecting");
        return FW_HTTP_NO_RESPONSE;
    }
    for (int redirects = 0;; redirects++) {
        if (!target_trusted(c)) {
            return FW_HTTP_UNTRUSTED;
        }
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
                *status = st;
                return FW_HTTP_BAD_ANSWER;
            }
            int flushed = 0;
            (void)esp_http_client_flush_response(c, &flushed);
            /* Only sets the next URL; the check above runs before it connects. */
            err = esp_http_client_set_redirection(c);
            if (err == ESP_ERR_HTTP_REDIRECT_DOWNGRADE) {
                ESP_LOGE(TAG, "refusing a redirect off https");
                return FW_HTTP_UNTRUSTED;
            }
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
                               char *etag_out, size_t etag_size, int *http_status)
{
    *out = NULL;
    *out_len = 0;
    if (http_status != NULL) {
        *http_status = 0;
    }
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
    if (http_status != NULL) {
        *http_status = status;
    }
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

/* Open an asset download: redirects, status 200 and a Content-Length that
 * fits the manifest. `what` names it in the logs. */
static esp_err_t open_asset(esp_http_client_handle_t c, const fw_manifest_file_t *file, const char *what)
{
    int status = 0;
    int64_t length = -1;
    fw_http_result_t r = open_follow(c, NULL, &status, &length);
    if (r == FW_HTTP_UNTRUSTED) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (r != FW_HTTP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "%s: HTTP %d", what, status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (fw_asset_check_length(file, length) != FW_ASSET_OK) {
        ESP_LOGE(TAG, "%s is %lld bytes, the manifest says %u", what, (long long)length, (unsigned)file->size);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t asset_err(fw_asset_result_t r)
{
    switch (r) {
    case FW_ASSET_OK:
        return ESP_OK;
    case FW_ASSET_ERR_SIZE:
        return ESP_ERR_INVALID_SIZE;
    case FW_ASSET_ERR_SHA256:
        return ESP_ERR_INVALID_CRC;
    case FW_ASSET_ERR_CRYPTO:
        break;
    }
    return ESP_FAIL;
}

esp_err_t fw_http_download_package(const char *url, const fw_manifest_file_t *file, const char *label)
{
    esp_http_client_config_t cfg;
    client_config(&cfg, url, NULL);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = open_asset(c, file, "receiver package");
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
    esp_http_client_config_t cfg;
    client_config(&cfg, url, NULL);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fw_asset_check_t check;
    memset(&check, 0, sizeof(check));
    esp_ota_handle_t ota = 0;
    uint8_t *buf = NULL;
    esp_err_t err = open_asset(c, file, "app image");
    if (err == ESP_OK) {
        buf = buf_alloc(CHUNK);
        err = buf == NULL ? ESP_ERR_NO_MEM : asset_err(fw_asset_check_begin(&check, file));
    }
    if (err == ESP_OK) {
        /* Erases sector by sector ahead of the writes. */
        err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    }
    while (err == ESP_OK) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            err = ESP_ERR_TIMEOUT;
        } else if (n == 0) {
            break;
        } else {
            err = asset_err(fw_asset_check_update(&check, buf, (size_t)n));   /* refuses a byte too many */
            if (err == ESP_OK) {
                err = esp_ota_write(ota, buf, (size_t)n);
            }
            if (err == ESP_OK) {
                fw_set_progress((uint8_t)((uint64_t)check.received * 100u / file->size));
            }
        }
    }
    if (err == ESP_OK && !esp_http_client_is_complete_data_received(c)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(buf);
    if (err == ESP_OK) {
        /* What arrived: exactly the manifest's size and SHA-256. */
        err = asset_err(fw_asset_check_finish(&check));
        if (err == ESP_ERR_INVALID_CRC) {
            ESP_LOGE(TAG, "app image SHA-256 does not match the manifest");
        }
    }
    fw_asset_check_abort(&check);
    if (err == ESP_OK) {
        /* Validation (image structure, chip, the image's own digest); frees the handle. */
        err = esp_ota_end(ota);
        ota = 0;
    }
    if (err == ESP_OK) {
        /* What is in flash, before anything is activated. */
        uint8_t digest[32];
        err = hash_partition(part, file->size, digest);
        if (err == ESP_OK && memcmp(digest, file->sha256, sizeof(digest)) != 0) {
            ESP_LOGE(TAG, "app image in flash does not match the manifest's SHA-256");
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        /* The image must be the release the manifest names, built by this project. */
        esp_app_desc_t desc;
        err = esp_ota_get_partition_description(part, &desc);
        if (err == ESP_OK && (strncmp(desc.version, version, sizeof(desc.version)) != 0 ||
                              strncmp(desc.project_name, esp_app_get_description()->project_name,
                                      sizeof(desc.project_name)) != 0)) {
            ESP_LOGE(TAG, "downloaded image is %.32s %.32s, the manifest names %s", desc.project_name, desc.version,
                     version);
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (ota != 0) {
        (void)esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "app image not activated: %s", esp_err_to_name(err));
        }
        return err;
    }
    ESP_LOGE(TAG, "app download failed: %s", esp_err_to_name(err));
    return err;
}
