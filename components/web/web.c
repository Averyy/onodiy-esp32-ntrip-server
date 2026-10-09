/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web.h"

#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "web";

/* The admin password hash uses PSA key derivation (Mbed TLS 4 in IDF v6.x;
 * the legacy mbedtls_pkcs5_* API is private there). Fail the build early if
 * the crypto configuration ever drops it. */
#if !defined(PSA_WANT_ALG_PBKDF2_HMAC) || !defined(PSA_WANT_ALG_SHA_256)
#error "PBKDF2-HMAC-SHA256 must be available through PSA Crypto"
#endif

esp_err_t web_init(void)
{
    ESP_LOGI(TAG, "stub");
    return ESP_OK;
}

esp_err_t web_start(void)
{
    /* TODO: httpd_start, URI table, Host check, WebSocket, captive-portal probes. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t web_stop(void)
{
    return ESP_OK;
}

esp_err_t web_ws_publish(const char *topic, const char *json)
{
    (void)topic;
    (void)json;
    return ESP_ERR_NOT_SUPPORTED;
}
