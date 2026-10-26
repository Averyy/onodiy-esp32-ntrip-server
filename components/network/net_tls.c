/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "network_tls.h"

#include <time.h>
#include "esp_crt_bundle.h"
#include "mbedtls/ssl.h"
#include "network.h"

static int verify_cert(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    network_tls_verify_t *ctx = arg;
    network_status_t ns;
    time_t now = time(NULL);
    bool trusted = network_get_status(&ns) == ESP_OK && ns.time_synced && now >= 1704067200;
    *flags |= network_cert_time_flags(crt, trusted, (int64_t)now);
    return ctx->bundle_verify != NULL ? ctx->bundle_verify(ctx->bundle_ctx, crt, depth, flags) :
                                       MBEDTLS_ERR_X509_FATAL_ERROR;
}

esp_err_t network_tls_bundle_attach(void *conf, network_tls_verify_t *ctx)
{
    if (conf == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err != ESP_OK) {
        return err;
    }
    mbedtls_ssl_config *ssl = conf;
    ctx->bundle_verify = ssl->MBEDTLS_PRIVATE(f_vrfy);
    ctx->bundle_ctx = ssl->MBEDTLS_PRIVATE(p_vrfy);
    mbedtls_ssl_conf_verify(ssl, verify_cert, ctx);
    return ESP_OK;
}
