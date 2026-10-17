/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Admin password hash (no RTOS or driver includes, host-testable):
 * PBKDF2-HMAC-SHA256 through PSA key derivation, the stored record and a
 * constant-time check; SHA-256 of a session token.
 *
 * Record sys/admin_hash, v1 (settings_codec.h, little-endian):
 *   u8 version (1), u32 iterations, salt[16], PBKDF2-HMAC-SHA256[32].
 * The iteration count is stored with each hash, so a later build can change
 * it without a migration: a check always uses the stored count.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_PW_VERSION     1u
#define WEB_PW_SALT_LEN    16u
#define WEB_PW_HASH_LEN    32u
#define WEB_PW_BLOB_LEN    (1u + 4u + WEB_PW_SALT_LEN + WEB_PW_HASH_LEN)
#define WEB_PW_MIN_ITER    1000u
#define WEB_PW_MAX_ITER    1000000u

typedef struct {
    uint32_t iterations;
    uint8_t salt[WEB_PW_SALT_LEN];
    uint8_t hash[WEB_PW_HASH_LEN];
} web_pw_record_t;

/**
 * PBKDF2-HMAC-SHA256 (PSA key derivation, one 32-byte block).
 * @return false if PSA refuses (not initialised, algorithm missing).
 */
bool web_pbkdf2_sha256(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iterations,
                       uint8_t out[WEB_PW_HASH_LEN]);

/** SHA-256 through PSA (a session's key: the digest of its token). @return false if PSA refuses. */
bool web_sha256(const uint8_t *data, size_t len, uint8_t out[WEB_PW_HASH_LEN]);

/** Make a record for a password with the given salt and cost. */
bool web_pw_make(const char *pw, const uint8_t salt[WEB_PW_SALT_LEN], uint32_t iterations, web_pw_record_t *out);

/** Check a password against a record (constant-time compare of the digests). */
bool web_pw_check(const web_pw_record_t *rec, const char *pw);

/** Encode a record; returns WEB_PW_BLOB_LEN, or 0 if cap is too small. */
size_t web_pw_encode(const web_pw_record_t *rec, uint8_t *buf, size_t cap);

/** Decode a v1 record; false for any other layout or an iteration count out of range. */
bool web_pw_decode(const uint8_t *buf, size_t len, web_pw_record_t *out);

/** Constant-time equality of two byte strings of equal length. */
bool web_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif
