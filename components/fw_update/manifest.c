/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "manifest.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"
#include "semver.h"

#if !defined(PSA_WANT_ALG_ECDSA) || !defined(PSA_WANT_ECC_SECP_R1_256) || !defined(PSA_WANT_ALG_SHA_256)
#error "signed manifests need ECDSA on P-256 with SHA-256 (CONFIG_MBEDTLS_ECDSA_C, CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED)"
#endif

#define SIG_RAW_LEN      64      /* r || s, 32 bytes each */
#define SIG_DER_MAX      80      /* DER of two 33-byte integers plus headers is at most 72 */
#define PAYLOAD_MAX      (64u * 1024u)

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    return (uint8_t)(c - 'A' + 10);
}

bool fw_manifest_key_from_hex(const char *hex, fw_manifest_key_t *out)
{
    if (hex == NULL || out == NULL) {
        return false;
    }
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; *p != '\0'; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':') {
            continue;
        }
        if (!is_hex(c) || n >= sizeof(out->point)) {
            return false;
        }
        if (hi < 0) {
            hi = hex_val(c);
        } else {
            out->point[n++] = (uint8_t)((hi << 4) | hex_val(c));
            hi = -1;
        }
    }
    return n == sizeof(out->point) && hi < 0 && out->point[0] == 0x04;
}

bool fw_manifest_asset_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

const char *fw_manifest_err_str(fw_manifest_err_t err)
{
    switch (err) {
    case FW_MANIFEST_OK:
        return "ok";
    case FW_MANIFEST_ERR_NO_KEY:
        return "no manifest key in this build";
    case FW_MANIFEST_ERR_ENVELOPE:
        return "not a signed manifest";
    case FW_MANIFEST_ERR_SIGNATURE:
        return "signature not valid for any known key";
    case FW_MANIFEST_ERR_CONTENT:
        return "manifest content not understood";
    case FW_MANIFEST_ERR_NO_MEM:
        return "out of memory";
    }
    return "?";
}

/* ---- payload ------------------------------------------------------------------- */

/* Copy a required string member; false if missing, not a string, empty or too long. */
static bool get_str(const cJSON *obj, const char *name, char *out, size_t out_size)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsString(f) || f->valuestring == NULL) {
        return false;
    }
    size_t n = strlen(f->valuestring);
    if (n == 0 || n >= out_size) {
        return false;
    }
    memcpy(out, f->valuestring, n + 1);
    return true;
}

/* A required integer member in 1..UINT32_MAX. */
static bool get_u32(const cJSON *obj, const char *name, uint32_t *out)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(f)) {
        return false;
    }
    double v = f->valuedouble;
    if (!(v >= 1.0 && v <= (double)UINT32_MAX) || v != (double)(uint32_t)v) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool get_sha256(const cJSON *obj, uint8_t out[32])
{
    char hex[65];
    if (!get_str(obj, "sha256", hex, sizeof(hex)) || strlen(hex) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        if (!is_hex(hex[2 * i]) || !is_hex(hex[2 * i + 1])) {
            return false;
        }
        out[i] = (uint8_t)((hex_val(hex[2 * i]) << 4) | hex_val(hex[2 * i + 1]));
    }
    return true;
}

static bool get_file(const cJSON *obj, fw_manifest_file_t *out)
{
    return cJSON_IsObject(obj) && get_str(obj, "asset", out->asset, sizeof(out->asset)) &&
           fw_manifest_asset_name_ok(out->asset) && get_u32(obj, "size", &out->size) && get_sha256(obj, out->sha256);
}

/* A firmware string that names a build ("R4.10Build18975") must name this one. */
static bool build_matches(const char *version, uint32_t build)
{
    const char *p = strstr(version, "Build");
    if (p == NULL) {
        return true;
    }
    p += 5;
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) {
            return false;
        }
        p++;
    }
    return v == build;
}

static bool printable(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) {
            return false;
        }
    }
    return true;
}

fw_manifest_err_t fw_manifest_parse_payload(const char *json, size_t len, fw_manifest_t *out)
{
    if (json == NULL || out == NULL) {
        return FW_MANIFEST_ERR_CONTENT;
    }
    memset(out, 0, sizeof(*out));
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    if (root == NULL) {
        return FW_MANIFEST_ERR_CONTENT;
    }
    fw_manifest_err_t err = FW_MANIFEST_ERR_CONTENT;
    /* Nothing but whitespace may follow the object. */
    for (const char *p = end; p != NULL && p < json + len; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\0') {
            goto done;
        }
    }
    if (!cJSON_IsObject(root)) {
        goto done;
    }
    uint32_t schema = 0;
    if (!get_u32(root, "schema", &schema) || schema != FW_MANIFEST_SCHEMA) {
        goto done;
    }
    semver_t v;
    if (!get_str(root, "version", out->version, sizeof(out->version)) || !semver_parse(out->version, &v)) {
        goto done;
    }
    if (!get_file(cJSON_GetObjectItemCaseSensitive(root, "app"), &out->app)) {
        goto done;
    }
    const cJSON *gnss = cJSON_GetObjectItemCaseSensitive(root, "gnss");
    if (gnss != NULL && !cJSON_IsNull(gnss)) {
        if (!cJSON_IsArray(gnss) || cJSON_GetArraySize(gnss) > FW_MANIFEST_MAX_GNSS) {
            goto done;
        }
        const cJSON *e;
        cJSON_ArrayForEach(e, gnss)
        {
            fw_manifest_gnss_t *g = &out->gnss[out->gnss_count];
            if (!cJSON_IsObject(e) || !get_str(e, "model", g->model, sizeof(g->model)) || !printable(g->model) ||
                !get_str(e, "version", g->version, sizeof(g->version)) || !printable(g->version) ||
                !get_u32(e, "build", &g->build) || !build_matches(g->version, g->build) || !get_file(e, &g->file)) {
                goto done;
            }
            out->gnss_count++;
        }
    }
    err = FW_MANIFEST_OK;

done:
    cJSON_Delete(root);
    if (err != FW_MANIFEST_OK) {
        memset(out, 0, sizeof(*out));
    }
    return err;
}

/* ---- envelope -------------------------------------------------------------------- */

/* Decode a base64 string member into a new buffer (NUL-terminated for convenience). */
static fw_manifest_err_t b64_member(const cJSON *root, const char *name, size_t max, uint8_t **out, size_t *out_len)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(f) || f->valuestring == NULL || f->valuestring[0] == '\0') {
        return FW_MANIFEST_ERR_ENVELOPE;
    }
    const unsigned char *src = (const unsigned char *)f->valuestring;
    size_t src_len = strlen(f->valuestring);
    size_t cap = (src_len / 4u) * 3u + 3u;
    if (cap > max + 3u) {
        return FW_MANIFEST_ERR_ENVELOPE;
    }
    uint8_t *buf = malloc(cap + 1u);
    if (buf == NULL) {
        return FW_MANIFEST_ERR_NO_MEM;
    }
    size_t n = 0;
    if (mbedtls_base64_decode(buf, cap, &n, src, src_len) != 0 || n == 0 || n > max) {
        free(buf);
        return FW_MANIFEST_ERR_ENVELOPE;
    }
    buf[n] = 0;
    *out = buf;
    *out_len = n;
    return FW_MANIFEST_OK;
}

static bool verify_with(const fw_manifest_key_t *key, const uint8_t *msg, size_t len, const uint8_t sig[SIG_RAW_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_import_key(&attr, key->point, sizeof(key->point), &id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        return false;
    }
    st = psa_verify_message(id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), msg, len, sig, SIG_RAW_LEN);
    psa_destroy_key(id);
    return st == PSA_SUCCESS;
}

fw_manifest_err_t fw_manifest_verify(const char *json, size_t len, const fw_manifest_key_t *keys, size_t key_count,
                                     fw_manifest_t *out, int *key_index)
{
    if (key_index != NULL) {
        *key_index = -1;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (keys == NULL || key_count == 0) {
        return FW_MANIFEST_ERR_NO_KEY;
    }
    if (json == NULL || out == NULL) {
        return FW_MANIFEST_ERR_ENVELOPE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return FW_MANIFEST_ERR_NO_MEM;
    }

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return FW_MANIFEST_ERR_ENVELOPE;
    }
    uint8_t *payload = NULL;
    uint8_t *der = NULL;
    size_t payload_len = 0;
    size_t der_len = 0;
    fw_manifest_err_t err = FW_MANIFEST_ERR_ENVELOPE;
    if (cJSON_IsObject(root)) {
        err = b64_member(root, "payload", PAYLOAD_MAX, &payload, &payload_len);
        if (err == FW_MANIFEST_OK) {
            err = b64_member(root, "signature", SIG_DER_MAX, &der, &der_len);
        }
    }
    cJSON_Delete(root);

    uint8_t sig[SIG_RAW_LEN];
    size_t sig_len = 0;
    if (err == FW_MANIFEST_OK &&
        (mbedtls_ecdsa_der_to_raw(256, der, der_len, sig, sizeof(sig), &sig_len) != 0 || sig_len != SIG_RAW_LEN)) {
        err = FW_MANIFEST_ERR_ENVELOPE;
    }
    if (err == FW_MANIFEST_OK) {
        err = FW_MANIFEST_ERR_SIGNATURE;
        for (size_t i = 0; i < key_count; i++) {
            if (verify_with(&keys[i], payload, payload_len, sig)) {
                if (key_index != NULL) {
                    *key_index = (int)i;
                }
                err = FW_MANIFEST_OK;
                break;
            }
        }
    }
    if (err == FW_MANIFEST_OK) {
        err = fw_manifest_parse_payload((const char *)payload, payload_len, out);
    }
    free(payload);
    free(der);
    return err;
}
