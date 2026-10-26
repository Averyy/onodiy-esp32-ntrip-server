/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Certificate validity dates for authenticated HTTPS and update transfers (pure logic,
 * host-testable).
 *
 * The build leaves CONFIG_MBEDTLS_HAVE_TIME_DATE off, so Mbed TLS ignores
 * every certificate's "valid from" and "valid to" (casters keep working
 * before the clock is set). HTTPS transfers check them anyway, on every certificate during the
 * handshake, with the station's clock, and a certificate that is expired or
 * not yet valid, or any certificate while the clock is not trusted, fails
 * the handshake.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mbedtls/x509_crt.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Verify flags to add for `crt` at `now_unix` (UTC): 0 when it is valid
 * then (both ends inclusive), MBEDTLS_X509_BADCERT_FUTURE before its
 * "valid from", MBEDTLS_X509_BADCERT_EXPIRED after its "valid to", and
 * MBEDTLS_X509_BADCERT_OTHER without a trusted clock. The CA bundle's
 * stand-in for a bundled root (no DER and no dates: the bundle keeps a
 * root's name and key only) gets 0.
 */
uint32_t network_cert_time_flags(const mbedtls_x509_crt *crt, bool clock_trusted, int64_t now_unix);

/** Per-client verification context, alive until that HTTPS client is cleaned up. */
typedef struct {
    int (*bundle_verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
    void *bundle_ctx;
} network_tls_verify_t;

/** Attach the CA bundle with certificate date checks using the trusted network clock. */
esp_err_t network_tls_bundle_attach(void *conf, network_tls_verify_t *ctx);

#ifdef __cplusplus
}
#endif
