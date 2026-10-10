/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signed release manifest: envelope, signature check and parsing (no RTOS or
 * driver code; cJSON and PSA Crypto only, so it is host-testable).
 *
 * The release asset (manifest.json) is an envelope:
 *
 *   { "payload":   "<base64 of the manifest bytes>",
 *     "signature": "<base64 of the DER ECDSA signature over those bytes>" }
 *
 * The signature is ECDSA on P-256 (secp256r1) with SHA-256 over the exact
 * decoded payload bytes, in the DER form `openssl dgst -sha256 -sign key.pem`
 * writes. A release is accepted when any compiled-in public key verifies it
 * (two keys, so the signing key can be rotated without stranding devices).
 * Nothing in the payload is read before the signature has been verified.
 *
 * The payload is the manifest itself:
 *
 *   { "schema": 1,
 *     "version": "v0.3.0",
 *     "app":  { "asset": "onocoydiy-esp-um982.bin", "size": 1234567, "sha256": "<64 hex>" },
 *     "gnss": [ { "model": "UM982", "version": "R4.10Build18975", "build": 18975,
 *                 "asset": "UM982_R4.10Build18975.pkg", "size": 3027628, "sha256": "<64 hex>" } ] }
 *
 * Rules: schema 1 exactly; "version" is a release version (semver.h: no
 * build metadata, at most 31 characters) and names the release tag the assets
 * are downloaded from; asset names are plain file names ([A-Za-z0-9._-],
 * not starting with '.'); sizes and builds are integers 1..UINT32_MAX; a
 * receiver version that contains "Build" must carry the same number as
 * "build"; at most FW_MANIFEST_MAX_GNSS receiver entries; "gnss" may be
 * missing or empty. Unknown fields are ignored, so later releases can add
 * fields without breaking older devices.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_MANIFEST_SCHEMA      1
#define FW_MANIFEST_MAX_GNSS    4
#define FW_MANIFEST_KEY_LEN     65   /**< uncompressed P-256 point: 0x04 || X || Y */
#define FW_MANIFEST_MAX_KEYS    2

typedef struct {
    char asset[96];          /**< release asset file name */
    uint32_t size;           /**< exact size in bytes */
    uint8_t sha256[32];
} fw_manifest_file_t;

typedef struct {
    char model[16];          /**< receiver model, matched exactly (e.g. "UM982") */
    char version[48];        /**< firmware string the receiver reports after the update */
    uint32_t build;          /**< build number, compared with the receiver's */
    fw_manifest_file_t file;
} fw_manifest_gnss_t;

typedef struct {
    char version[32];        /**< release version = tag of the release holding the assets */
    fw_manifest_file_t app;
    uint8_t gnss_count;
    fw_manifest_gnss_t gnss[FW_MANIFEST_MAX_GNSS];
} fw_manifest_t;

typedef enum {
    FW_MANIFEST_OK = 0,
    FW_MANIFEST_ERR_NO_KEY,      /**< no usable public key compiled in: nothing can be verified */
    FW_MANIFEST_ERR_ENVELOPE,    /**< not a signed manifest (JSON, fields, base64 or signature encoding) */
    FW_MANIFEST_ERR_SIGNATURE,   /**< no compiled-in key verifies the signature */
    FW_MANIFEST_ERR_CONTENT,     /**< correctly signed, but not a manifest this firmware understands */
    FW_MANIFEST_ERR_NO_MEM,
} fw_manifest_err_t;

/** A public key: uncompressed P-256 point. */
typedef struct {
    uint8_t point[FW_MANIFEST_KEY_LEN];
} fw_manifest_key_t;

/**
 * Parse a key given as 130 hex digits (the uncompressed point 04 || X || Y,
 * as printed by `openssl ec -pubin -text` without the colons). Whitespace and
 * ':' separators are ignored. The point is checked by PSA when used.
 * @return false for anything else (an empty string = no key configured).
 */
bool fw_manifest_key_from_hex(const char *hex, fw_manifest_key_t *out);

/**
 * Verify the envelope's signature with the given keys, then parse the payload.
 * @param key_index optional: which key verified it (0-based), -1 if none.
 */
fw_manifest_err_t fw_manifest_verify(const char *json, size_t len, const fw_manifest_key_t *keys, size_t key_count,
                                     fw_manifest_t *out, int *key_index);

/** Parse an already verified payload (also used by the tests). */
fw_manifest_err_t fw_manifest_parse_payload(const char *json, size_t len, fw_manifest_t *out);

/** True if `name` is a plain release asset file name (safe to put in a URL path). */
bool fw_manifest_asset_name_ok(const char *name);

/** Short text for a result (logs and the UI). */
const char *fw_manifest_err_str(fw_manifest_err_t err);

#ifdef __cplusplus
}
#endif
