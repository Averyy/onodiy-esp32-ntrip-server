/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_pw.h"

#include <string.h>

#include "psa/crypto.h"
#include "settings_codec.h"

/* The admin password hash uses PSA key derivation (Mbed TLS 4 in IDF v6.x;
 * the legacy mbedtls_pkcs5_* API is private there). Fail the build early if
 * the crypto configuration ever drops it. */
#if !defined(PSA_WANT_ALG_PBKDF2_HMAC) || !defined(PSA_WANT_ALG_SHA_256)
#error "PBKDF2-HMAC-SHA256 must be available through PSA Crypto"
#endif

bool web_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool web_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                       uint8_t out[WEB_PW_HASH_LEN])
{
    if (pw == NULL || salt == NULL || out == NULL || iterations == 0) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    /* PBKDF2 inputs in the order PSA requires: cost, salt, password. */
    bool ok = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
              psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, pw, pw_len) == PSA_SUCCESS &&
              psa_key_derivation_output_bytes(&op, out, WEB_PW_HASH_LEN) == PSA_SUCCESS;
    psa_key_derivation_abort(&op);
    if (!ok) {
        memset(out, 0, WEB_PW_HASH_LEN);
    }
    return ok;
}

bool web_sha256(const uint8_t *data, size_t len, uint8_t out[WEB_PW_HASH_LEN])
{
    size_t n = 0;
    if (data == NULL || out == NULL || psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, data, len, out, WEB_PW_HASH_LEN, &n) != PSA_SUCCESS || n != WEB_PW_HASH_LEN) {
        if (out != NULL) {
            memset(out, 0, WEB_PW_HASH_LEN);
        }
        return false;
    }
    return true;
}

bool web_pw_make(const char *pw, const uint8_t salt[WEB_PW_SALT_LEN], uint32_t iterations, web_pw_record_t *out)
{
    if (pw == NULL || out == NULL || iterations < WEB_PW_MIN_ITER || iterations > WEB_PW_MAX_ITER) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->iterations = iterations;
    memcpy(out->salt, salt, WEB_PW_SALT_LEN);
    return web_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), out->salt, WEB_PW_SALT_LEN, iterations, out->hash);
}

bool web_pw_check(const web_pw_record_t *rec, const char *pw)
{
    if (rec == NULL || pw == NULL) {
        return false;
    }
    uint8_t got[WEB_PW_HASH_LEN];
    bool ok = web_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), rec->salt, WEB_PW_SALT_LEN, rec->iterations, got) &&
              web_ct_equal(got, rec->hash, WEB_PW_HASH_LEN);
    memset(got, 0, sizeof(got));
    return ok;
}

size_t web_pw_encode(const web_pw_record_t *rec, uint8_t *buf, size_t cap)
{
    if (rec == NULL || buf == NULL) {
        return 0;
    }
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, WEB_PW_VERSION);
    blob_put_u32(&w, rec->iterations);
    for (size_t i = 0; i < WEB_PW_SALT_LEN; i++) {
        blob_put_u8(&w, rec->salt[i]);
    }
    for (size_t i = 0; i < WEB_PW_HASH_LEN; i++) {
        blob_put_u8(&w, rec->hash[i]);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

bool web_pw_decode(const uint8_t *buf, size_t len, web_pw_record_t *out)
{
    if (buf == NULL || out == NULL || len != WEB_PW_BLOB_LEN) {
        return false;
    }
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    web_pw_record_t rec;
    uint8_t version = blob_get_u8(&r);
    rec.iterations = blob_get_u32(&r);
    for (size_t i = 0; i < WEB_PW_SALT_LEN; i++) {
        rec.salt[i] = blob_get_u8(&r);
    }
    for (size_t i = 0; i < WEB_PW_HASH_LEN; i++) {
        rec.hash[i] = blob_get_u8(&r);
    }
    if (!blob_reader_ok(&r) || version != WEB_PW_VERSION || rec.iterations < WEB_PW_MIN_ITER ||
        rec.iterations > WEB_PW_MAX_ITER) {
        return false;
    }
    *out = rec;
    return true;
}
