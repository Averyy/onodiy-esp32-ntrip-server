/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Release manifest: reading it (plain or signed), the optional signature
 * check, the content rules, and the per-asset size and SHA-256 check (no RTOS
 * or driver code; cJSON and PSA Crypto only, so it is host-testable).
 *
 * The manifest:
 *
 *   { "schema": 1,
 *     "version": "v0.3.0",
 *     "app":  { "asset": "onocoydiy-esp-um982.bin", "size": 1234567, "sha256": "<64 hex>" },
 *     "gnss": [ { "model": "UM982", "version": "R4.10Build18975", "build": 18975,
 *                 "asset": "UM982_R4.10Build18975.pkg", "size": 3027628, "sha256": "<64 hex>" } ] }
 *
 * The release asset (manifest.json) holds it in one of two forms:
 *
 *  - plain: the manifest itself (what tools/release_manifest.py writes
 *    without a signing key);
 *  - signed: an envelope
 *      { "payload":   "<base64 of the manifest bytes>",
 *        "signature": "<base64 of the DER ECDSA signature over those bytes>" }
 *    with ECDSA on P-256 (secp256r1) and SHA-256 over the exact decoded
 *    payload bytes, in the DER form `openssl dgst -sha256 -sign key.pem`
 *    writes.
 * A top-level "payload" member makes it an envelope, so a plain manifest
 * never uses that name.
 *
 * Trust:
 *  - A build without a manifest key (the default) accepts either form and
 *    checks no signature: it has nothing to check one against. The release
 *    is trusted because it came over HTTPS, with the certificate checked
 *    against the CA bundle, from GitHub's release hosts for the repository
 *    set at build time (fw_http.c, release_url.h), and every asset it names
 *    must match its size and SHA-256 (fw_asset_check_*, pkg_store).
 *  - A build with a key (CONFIG_FW_UPDATE_MANIFEST_KEY_1/_2, e.g. a fork that
 *    signs its releases) accepts only an envelope whose signature one of its
 *    keys verifies: a plain manifest, or an envelope without a signature, is
 *    refused (FW_MANIFEST_ERR_UNSIGNED), so such a build never falls back to
 *    unsigned releases. Nothing in the payload is read before the signature
 *    has been verified. Two keys, so the signing key can be rotated without
 *    stranding devices.
 *
 * Content rules: schema 1 exactly; "version" is a release version (semver.h:
 * no build metadata, at most 31 characters) and names the release tag the
 * assets are downloaded from; asset names are plain file names
 * ([A-Za-z0-9._-], not starting with '.'); sizes and builds are integers
 * 1..UINT32_MAX; a receiver version that contains "Build" must carry the same
 * number as "build"; at most FW_MANIFEST_MAX_GNSS receiver entries; "gnss" may
 * be missing or empty. Unknown fields are ignored, so later releases can add
 * fields without breaking older devices. Nothing but whitespace may follow
 * the JSON object.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psa/crypto.h"

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
    FW_MANIFEST_ERR_FORMAT,      /**< not a JSON object, or a malformed envelope (fields, base64, signature encoding) */
    FW_MANIFEST_ERR_UNSIGNED,    /**< no signature, and this build requires one */
    FW_MANIFEST_ERR_SIGNATURE,   /**< no key of this build verifies the signature */
    FW_MANIFEST_ERR_CONTENT,     /**< not a manifest this firmware understands */
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
 * Read the release asset as fetched: plain or signed (see the trust rules
 * above), then the content rules.
 * @param keys, key_count the keys of this build; key_count 0 = no signature
 *        required (and none checked).
 * @param key_index optional: which key verified the signature (0-based), -1
 *        when no signature was verified (always -1 without keys).
 */
fw_manifest_err_t fw_manifest_read(const char *json, size_t len, const fw_manifest_key_t *keys, size_t key_count,
                                   fw_manifest_t *out, int *key_index);

/** Parse a manifest's bytes (the plain form, or an envelope's payload) with the content rules. */
fw_manifest_err_t fw_manifest_parse_payload(const char *json, size_t len, fw_manifest_t *out);

/** True if `name` is a plain release asset file name (safe to put in a URL path). */
bool fw_manifest_asset_name_ok(const char *name);

/** Short text for a result (logs and the UI). */
const char *fw_manifest_err_str(fw_manifest_err_t err);

/* ---- per-asset check -------------------------------------------------------------- */

typedef enum {
    FW_ASSET_OK = 0,
    FW_ASSET_ERR_SIZE,       /**< more or fewer bytes than the manifest's size (or such a Content-Length) */
    FW_ASSET_ERR_SHA256,     /**< the size is right, the SHA-256 is not the manifest's */
    FW_ASSET_ERR_CRYPTO,     /**< PSA failed (not the asset's fault) */
} fw_asset_result_t;

/**
 * Streaming check of one downloaded asset against its manifest entry: the
 * bytes are hashed as they arrive, one byte more than the manifest's size is
 * refused at once, and finish() compares the count and the SHA-256.
 */
typedef struct {
    uint32_t size;
    uint8_t sha256[32];
    uint32_t received;
    bool open;
    psa_hash_operation_t op;
} fw_asset_check_t;

/** A Content-Length (-1 = not announced) that cannot be this asset is refused before any byte is read. */
fw_asset_result_t fw_asset_check_length(const fw_manifest_file_t *file, int64_t content_length);

fw_asset_result_t fw_asset_check_begin(fw_asset_check_t *c, const fw_manifest_file_t *file);

/** Add received bytes; FW_ASSET_ERR_SIZE as soon as they exceed the size (the check is then closed). */
fw_asset_result_t fw_asset_check_update(fw_asset_check_t *c, const void *data, size_t len);

/** All bytes received: compare count and SHA-256. Closes the check in every case. */
fw_asset_result_t fw_asset_check_finish(fw_asset_check_t *c);

/** Abandon an open check (no-op on a closed one). */
void fw_asset_check_abort(fw_asset_check_t *c);

/** Short text for a result (logs). */
const char *fw_asset_result_str(fw_asset_result_t r);

#ifdef __cplusplus
}
#endif
