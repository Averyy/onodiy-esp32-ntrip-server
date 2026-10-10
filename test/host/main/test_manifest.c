/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Release manifest: plain and signed forms, the trust rule (a build with a
 * key takes signed manifests only, a build without one takes either), the
 * signature (PSA), the content rules and the per-asset size and SHA-256
 * check.
 *
 * The fixture was made the way tools/release_manifest.py signs a manifest:
 *   openssl ecparam -name prime256v1 -genkey -noout -out key.pem
 *   openssl dgst -sha256 -sign key.pem -out payload.sig payload.json
 * and the key below is `openssl ec -pubin -in key.pub -text -noout`, pasted
 * as printed (a throwaway test key; its private half was not kept).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "manifest.h"
#include "mbedtls/base64.h"
#include "mbedtls/psa_util.h"
#include "psa/crypto.h"
#include "unity.h"

static const char FIXTURE_KEY_TEXT[] = "04:e3:28:91:89:2c:a3:19:c1:6a:f3:4a:3f:da:4c:\n"
                                       "    dc:13:7a:45:59:1d:50:e5:a8:b3:a3:ef:72:c3:f1:\n"
                                       "    31:05:48:60:0f:0b:11:78:36:78:91:fc:f9:ed:9d:\n"
                                       "    6a:0c:9e:4a:d2:53:09:81:f5:cd:83:e4:81:6f:86:\n"
                                       "    d9:1d:8d:46:af";

/* payload.json (with its trailing newline), base64 */
static const char FIXTURE_PAYLOAD_B64[] =
    "eyJzY2hlbWEiOjEsInZlcnNpb24iOiJ2MC4zLjAiLCJhcHAiOnsiYXNzZXQiOiJvbm9jb3lkaXktZXNwLXVtOTgyLmJpbiIsInNpemUiOjEy"
    "MzQ1NjcsInNoYTI1NiI6IjAwMTEyMjMzNDQ1NTY2Nzc4ODk5YWFiYmNjZGRlZWZmMDAxMTIyMzM0NDU1NjY3Nzg4OTlhYWJiY2NkZGVlZmYi"
    "fSwiZ25zcyI6W3sibW9kZWwiOiJVTTk4MiIsInZlcnNpb24iOiJSNC4xMEJ1aWxkMTg5NzUiLCJidWlsZCI6MTg5NzUsImFzc2V0IjoiVU05"
    "ODJfUjQuMTBCdWlsZDE4OTc1LnBrZyIsInNpemUiOjMwMjc2MjgsInNoYTI1NiI6IjlhYzI1ZDFkMzU5ZTU4MTViMTMyOTFmNjE0MDVmYWVm"
    "NTNkNzFlMzA2YTYwMjE4NDU0NmIzMjU4NGIzNWVjNzEifV19Cg==";

/* payload.sig (DER), base64 */
static const char FIXTURE_SIG_B64[] =
    "MEYCIQC1o6Tgb/AFZcz3l//DXO0XUgurg8ygnMj0MVGbGIInlQIhAJKTNP9Yep3zu1YYoSHcnqAWhX79TGIRx+5wgdHikiQK";

static char *make_envelope(const char *payload_b64, const char *sig_b64)
{
    size_t n = strlen(payload_b64) + strlen(sig_b64) + 64;
    char *s = malloc(n);
    TEST_ASSERT_NOT_NULL(s);
    snprintf(s, n, "{\"payload\": \"%s\",\n \"signature\": \"%s\"}", payload_b64, sig_b64);
    return s;
}

static fw_manifest_key_t fixture_key(void)
{
    fw_manifest_key_t k;
    TEST_ASSERT_TRUE(fw_manifest_key_from_hex(FIXTURE_KEY_TEXT, &k));
    return k;
}

static fw_manifest_err_t verify_str(const char *json, const fw_manifest_key_t *keys, size_t n, fw_manifest_t *m,
                                    int *idx)
{
    return fw_manifest_read(json, strlen(json), keys, n, m, idx);
}

/* ---- a signer for generated manifests ---------------------------------------------------- */

typedef struct {
    mbedtls_svc_key_id_t id;
    fw_manifest_key_t pub;
} signer_t;

static void signer_new(signer_t *s)
{
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_crypto_init());
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_generate_key(&a, &s->id));
    size_t n = 0;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_export_public_key(s->id, s->pub.point, sizeof(s->pub.point), &n));
    TEST_ASSERT_EQUAL_size_t(FW_MANIFEST_KEY_LEN, n);
}

static void signer_free(signer_t *s)
{
    psa_destroy_key(s->id);
}

static char *b64(const uint8_t *data, size_t len)
{
    size_t n = 0;
    (void)mbedtls_base64_encode(NULL, 0, &n, data, len);
    char *out = malloc(n + 1);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(0, mbedtls_base64_encode((unsigned char *)out, n + 1, &n, data, len));
    out[n] = '\0';
    return out;
}

static char *sign_envelope(const signer_t *s, const char *payload)
{
    uint8_t raw[64];
    size_t raw_len = 0;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_sign_message(s->id, PSA_ALG_ECDSA(PSA_ALG_SHA_256), (const uint8_t *)payload,
                                                    strlen(payload), raw, sizeof(raw), &raw_len));
    uint8_t der[80];
    size_t der_len = 0;
    TEST_ASSERT_EQUAL_INT(0, mbedtls_ecdsa_raw_to_der(256, raw, raw_len, der, sizeof(der), &der_len));
    char *p = b64((const uint8_t *)payload, strlen(payload));
    char *g = b64(der, der_len);
    char *env = make_envelope(p, g);
    free(p);
    free(g);
    return env;
}

/* Sign `payload` with a fresh key and verify it with that key. */
static fw_manifest_err_t sign_and_verify(const char *payload, fw_manifest_t *m)
{
    signer_t s;
    signer_new(&s);
    char *env = sign_envelope(&s, payload);
    fw_manifest_err_t err = verify_str(env, &s.pub, 1, m, NULL);
    free(env);
    signer_free(&s);
    return err;
}

#define SHA_A "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
#define APP_OK "\"app\":{\"asset\":\"fw.bin\",\"size\":1000,\"sha256\":\"" SHA_A "\"}"

/* ---- keys ------------------------------------------------------------------------- */

TEST_CASE("manifest keys parse from openssl's text output and plain hex", "[manifest]")
{
    fw_manifest_key_t k;
    TEST_ASSERT_TRUE(fw_manifest_key_from_hex(FIXTURE_KEY_TEXT, &k));
    TEST_ASSERT_EQUAL_HEX8(0x04, k.point[0]);
    TEST_ASSERT_EQUAL_HEX8(0xe3, k.point[1]);
    TEST_ASSERT_EQUAL_HEX8(0xaf, k.point[64]);

    char plain[131];
    for (size_t i = 0; i < 65; i++) {
        snprintf(&plain[2 * i], 3, "%02X", k.point[i]);
    }
    fw_manifest_key_t k2;
    TEST_ASSERT_TRUE(fw_manifest_key_from_hex(plain, &k2));
    TEST_ASSERT_EQUAL_MEMORY(k.point, k2.point, sizeof(k.point));

    TEST_ASSERT_FALSE(fw_manifest_key_from_hex("", &k2));                 /* no key configured */
    TEST_ASSERT_FALSE(fw_manifest_key_from_hex(&plain[2], &k2));          /* 64 bytes */
    plain[1] = '2';                                                       /* 02...: compressed form */
    TEST_ASSERT_FALSE(fw_manifest_key_from_hex(plain, &k2));
    TEST_ASSERT_FALSE(fw_manifest_key_from_hex("04zz", &k2));
    char longer[140];
    snprintf(longer, sizeof(longer), "04%s00", &plain[2]);
    TEST_ASSERT_FALSE(fw_manifest_key_from_hex(longer, &k2));             /* 66 bytes */
}

/* ---- signature -------------------------------------------------------------------- */

TEST_CASE("manifest signed with openssl verifies and parses", "[manifest]")
{
    fw_manifest_key_t key = fixture_key();
    char *env = make_envelope(FIXTURE_PAYLOAD_B64, FIXTURE_SIG_B64);
    fw_manifest_t m;
    int idx = -2;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(env, &key, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(0, idx);
    TEST_ASSERT_EQUAL_STRING("v0.3.0", m.version);
    TEST_ASSERT_EQUAL_STRING("onocoydiy-esp-um982.bin", m.app.asset);
    TEST_ASSERT_EQUAL_UINT32(1234567, m.app.size);
    TEST_ASSERT_EQUAL_HEX8(0x00, m.app.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, m.app.sha256[31]);
    TEST_ASSERT_EQUAL_UINT8(1, m.gnss_count);
    TEST_ASSERT_EQUAL_STRING("UM982", m.gnss[0].model);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", m.gnss[0].version);
    TEST_ASSERT_EQUAL_UINT32(18975, m.gnss[0].build);
    TEST_ASSERT_EQUAL_STRING("UM982_R4.10Build18975.pkg", m.gnss[0].file.asset);
    TEST_ASSERT_EQUAL_UINT32(3027628, m.gnss[0].file.size);
    static const uint8_t pkg_sha[32] = {
        0x9a, 0xc2, 0x5d, 0x1d, 0x35, 0x9e, 0x58, 0x15, 0xb1, 0x32, 0x91, 0xf6, 0x14, 0x05, 0xfa, 0xef,
        0x53, 0xd7, 0x1e, 0x30, 0x6a, 0x60, 0x21, 0x84, 0x54, 0x6b, 0x32, 0x58, 0x4b, 0x35, 0xec, 0x71,
    };
    TEST_ASSERT_EQUAL_MEMORY(pkg_sha, m.gnss[0].file.sha256, 32);
    free(env);
}

TEST_CASE("manifest: either compiled-in key may sign (rotation)", "[manifest]")
{
    signer_t other;
    signer_new(&other);
    fw_manifest_key_t keys[2] = { other.pub, fixture_key() };
    char *env = make_envelope(FIXTURE_PAYLOAD_B64, FIXTURE_SIG_B64);
    fw_manifest_t m;
    int idx = -1;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(env, keys, 2, &m, &idx));
    TEST_ASSERT_EQUAL_INT(1, idx);
    /* Only the other key: not accepted, and nothing of the payload is used. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_SIGNATURE, verify_str(env, keys, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_STRING("", m.version);
    TEST_ASSERT_EQUAL_UINT8(0, m.gnss_count);
    free(env);
    signer_free(&other);
}

/* ---- trust: with and without a key ------------------------------------------------- */

#define PLAIN_OK "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK "}\n"

TEST_CASE("manifest: a build without a key accepts the plain manifest", "[manifest]")
{
    fw_manifest_t m;
    int idx = -2;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(PLAIN_OK, NULL, 0, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);   /* nothing was verified by a signature */
    TEST_ASSERT_EQUAL_STRING("v1.0.0", m.version);
    TEST_ASSERT_EQUAL_STRING("fw.bin", m.app.asset);
    TEST_ASSERT_EQUAL_UINT32(1000, m.app.size);
    TEST_ASSERT_EQUAL_HEX8(0x00, m.app.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, m.app.sha256[31]);

    /* The content rules still apply. */
    static const char *const bad[] = {
        "{\"schema\":2,\"version\":\"v1.0.0\"," APP_OK "}",
        "{\"schema\":1,\"version\":\"v1.0.0+dev\"," APP_OK "}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"../fw.bin\",\"size\":1,\"sha256\":\"" SHA_A "\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_CONTENT, verify_str(bad[i], NULL, 0, &m, NULL), bad[i]);
        TEST_ASSERT_EQUAL_STRING("", m.version);
    }
    /* Not an object, or something after it. */
    static const char *const format[] = { "", "not json", "[]", "5", PLAIN_OK "{}", PLAIN_OK "x" };
    for (size_t i = 0; i < sizeof(format) / sizeof(format[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_FORMAT, verify_str(format[i], NULL, 0, &m, NULL), format[i]);
    }
}

TEST_CASE("manifest: a build without a key reads a signed envelope without checking it", "[manifest]")
{
    fw_manifest_t m;
    int idx = -2;
    char *env = make_envelope(FIXTURE_PAYLOAD_B64, FIXTURE_SIG_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(env, NULL, 0, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_STRING("v0.3.0", m.version);
    TEST_ASSERT_EQUAL_UINT8(1, m.gnss_count);
    free(env);

    /* No key to check a signature against: its value does not matter. */
    env = make_envelope(FIXTURE_PAYLOAD_B64, "***");
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(env, NULL, 0, &m, NULL));
    free(env);
    char *one = malloc(strlen(FIXTURE_PAYLOAD_B64) + 32);
    TEST_ASSERT_NOT_NULL(one);
    sprintf(one, "{\"payload\": \"%s\"}", FIXTURE_PAYLOAD_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(one, NULL, 0, &m, NULL));
    free(one);

    /* A broken envelope is still refused. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_FORMAT, verify_str("{\"payload\": \"***\"}", NULL, 0, &m, NULL));
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_FORMAT, verify_str("{\"payload\": null, \"schema\": 1}", NULL, 0, &m, NULL));
}

TEST_CASE("manifest: a build with a key refuses unsigned manifests", "[manifest]")
{
    fw_manifest_key_t key = fixture_key();
    fw_manifest_t m;
    int idx = -2;
    /* The plain form, otherwise valid. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_UNSIGNED, verify_str(PLAIN_OK, &key, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    TEST_ASSERT_EQUAL_STRING("", m.version);
    TEST_ASSERT_EQUAL_STRING("", m.app.asset);

    /* An envelope without a signature, or with a null one. */
    char *one = malloc(strlen(FIXTURE_PAYLOAD_B64) + 64);
    TEST_ASSERT_NOT_NULL(one);
    sprintf(one, "{\"payload\": \"%s\"}", FIXTURE_PAYLOAD_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_UNSIGNED, verify_str(one, &key, 1, &m, NULL));
    sprintf(one, "{\"payload\": \"%s\", \"signature\": null}", FIXTURE_PAYLOAD_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_UNSIGNED, verify_str(one, &key, 1, &m, NULL));
    free(one);
    TEST_ASSERT_EQUAL_STRING("", m.version);

    /* Signed by another key: refused (no fallback to "unsigned is fine"). */
    signer_t other;
    signer_new(&other);
    char *env = sign_envelope(&other, PLAIN_OK);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_SIGNATURE, verify_str(env, &key, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    /* ... while the build without a key reads it. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(env, NULL, 0, &m, NULL));
    free(env);
    signer_free(&other);
}

TEST_CASE("manifest: a changed size or digest breaks the signature", "[manifest]")
{
    signer_t s;
    signer_new(&s);
    char *good = sign_envelope(&s, PLAIN_OK);
    fw_manifest_t m;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(good, &s.pub, 1, &m, NULL));

    /* Same signature over a payload with another size, then another digest. */
    static const char *const changed[] = {
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":1001,\"sha256\":\"" SHA_A
        "\"}}\n",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":1000,\"sha256\":"
        "\"10112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}}\n",
    };
    cJSON *orig = cJSON_Parse(good);
    TEST_ASSERT_NOT_NULL(orig);
    const char *sig = cJSON_GetObjectItemCaseSensitive(orig, "signature")->valuestring;
    for (size_t i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        char *p = b64((const uint8_t *)changed[i], strlen(changed[i]));
        char *env = make_envelope(p, sig);
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_SIGNATURE, verify_str(env, &s.pub, 1, &m, NULL), changed[i]);
        TEST_ASSERT_EQUAL_UINT32(0, m.app.size);
        free(env);
        free(p);
    }
    cJSON_Delete(orig);
    free(good);
    signer_free(&s);
}

TEST_CASE("manifest: any change to the signed bytes is rejected", "[manifest]")
{
    fw_manifest_key_t key = fixture_key();
    fw_manifest_t m;

    /* Payload changed (one base64 character: three bytes differ). */
    char *payload = strdup(FIXTURE_PAYLOAD_B64);
    payload[40] = payload[40] == 'A' ? 'B' : 'A';
    char *env = make_envelope(payload, FIXTURE_SIG_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_SIGNATURE, verify_str(env, &key, 1, &m, NULL));
    free(env);
    free(payload);

    /* Signature changed inside r. */
    char *sig = strdup(FIXTURE_SIG_B64);
    sig[20] = sig[20] == 'A' ? 'B' : 'A';
    env = make_envelope(FIXTURE_PAYLOAD_B64, sig);
    fw_manifest_err_t err = verify_str(env, &key, 1, &m, NULL);
    TEST_ASSERT_TRUE(err == FW_MANIFEST_ERR_SIGNATURE || err == FW_MANIFEST_ERR_FORMAT);
    free(env);
    free(sig);

    /* The same checks pass for a manifest signed with a generated key. */
    signer_t s;
    signer_new(&s);
    char *good = sign_envelope(&s, "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK "}");
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(good, &s.pub, 1, &m, NULL));
    free(good);
    signer_free(&s);
}

TEST_CASE("manifest: a signed envelope must be well formed", "[manifest]")
{
    fw_manifest_key_t key = fixture_key();
    fw_manifest_t m;
    static const char *const bad[] = {
        "",
        "not json",
        "[]",
        "{\"payload\": 5, \"signature\": \"MEYCIQ==\"}",
        "{\"payload\": \"\", \"signature\": \"MEYCIQ==\"}",
        "{\"payload\": \"eyJ9\", \"signature\": \"***\"}",
        "{\"payload\": \"eyJ9\", \"signature\": 5}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_FORMAT, fw_manifest_read(bad[i], strlen(bad[i]), &key, 1, &m, NULL),
                                  bad[i]);
    }
    /* No payload member: the plain form; no signature member: unsigned. Both
     * are unsigned to a build with a key. */
    static const char *const unsigned_[] = {
        "{}",
        "{\"payload\": \"eyJ9\"}",
        "{\"signature\": \"MEYCIQ==\"}",
    };
    for (size_t i = 0; i < sizeof(unsigned_) / sizeof(unsigned_[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_UNSIGNED,
                                  fw_manifest_read(unsigned_[i], strlen(unsigned_[i]), &key, 1, &m, NULL), unsigned_[i]);
    }
    /* A raw r||s signature instead of DER. */
    uint8_t raw[64];
    memset(raw, 0x11, sizeof(raw));
    char *sig = b64(raw, sizeof(raw));
    char *env = make_envelope(FIXTURE_PAYLOAD_B64, sig);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_FORMAT, verify_str(env, &key, 1, &m, NULL));
    free(env);
    free(sig);
    /* A length that does not match the buffer. */
    env = make_envelope(FIXTURE_PAYLOAD_B64, FIXTURE_SIG_B64);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_FORMAT, fw_manifest_read(env, 20, &key, 1, &m, NULL));
    free(env);
}

/* ---- content ------------------------------------------------------------------------ */

TEST_CASE("manifest content: minimal, unknown fields and empty receiver list", "[manifest]")
{
    fw_manifest_t m;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, sign_and_verify("{\"schema\":1,\"version\":\"v1.2.3\"," APP_OK "}", &m));
    TEST_ASSERT_EQUAL_STRING("v1.2.3", m.version);
    TEST_ASSERT_EQUAL_UINT8(0, m.gnss_count);
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK,
                      sign_and_verify("{\"schema\":1,\"version\":\"1.2.3-rc.1\",\"notes\":\"x\",\"future\":{\"a\":1},"
                                      APP_OK ",\"gnss\":null}\n\n",
                                      &m));
    TEST_ASSERT_EQUAL_STRING("1.2.3-rc.1", m.version);
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, sign_and_verify("{\"schema\":1,\"version\":\"v1.2.3\"," APP_OK ",\"gnss\":[]}", &m));
}

TEST_CASE("manifest content: fields outside the rules are rejected", "[manifest]")
{
    static const char *const bad[] = {
        /* schema */
        "{\"version\":\"v1.0.0\"," APP_OK "}",
        "{\"schema\":2,\"version\":\"v1.0.0\"," APP_OK "}",
        "{\"schema\":\"1\",\"version\":\"v1.0.0\"," APP_OK "}",
        /* version: a release only */
        "{\"schema\":1," APP_OK "}",
        "{\"schema\":1,\"version\":\"v1.0.0+dev\"," APP_OK "}",
        "{\"schema\":1,\"version\":\"v1.0\"," APP_OK "}",
        "{\"schema\":1,\"version\":\"v1.0.0-this-pre-release-is-far-too-long\"," APP_OK "}",
        /* app */
        "{\"schema\":1,\"version\":\"v1.0.0\"}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"../fw.bin\",\"size\":1,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"a/b.bin\",\"size\":1,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin?x=1\",\"size\":1,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\".hidden\",\"size\":1,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":0,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":-5,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":1.5,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":4294967296,\"sha256\":\"" SHA_A "\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":1,\"sha256\":\"00112233\"}}",
        "{\"schema\":1,\"version\":\"v1.0.0\",\"app\":{\"asset\":\"fw.bin\",\"size\":1,\"sha256\":"
        "\"g0112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\"}}",
        /* receiver entries */
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":{}}",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[{\"model\":\"UM982\",\"version\":\"R4.10Build18975\","
        "\"build\":18976,\"asset\":\"a.pkg\",\"size\":1,\"sha256\":\"" SHA_A "\"}]}",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[{\"model\":\"UM982\",\"version\":\"R4.10Build\","
        "\"build\":18975,\"asset\":\"a.pkg\",\"size\":1,\"sha256\":\"" SHA_A "\"}]}",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[{\"model\":\"\",\"version\":\"R1\","
        "\"build\":1,\"asset\":\"a.pkg\",\"size\":1,\"sha256\":\"" SHA_A "\"}]}",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[{\"model\":\"UM982\",\"version\":\"R1\","
        "\"asset\":\"a.pkg\",\"size\":1,\"sha256\":\"" SHA_A "\"}]}",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[{\"model\":\"UM982 with a very long name\","
        "\"version\":\"R1\",\"build\":1,\"asset\":\"a.pkg\",\"size\":1,\"sha256\":\"" SHA_A "\"}]}",
        /* not an object, or something after it */
        "[1,2,3]",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK "} trailing",
        "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK "}{}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fw_manifest_t m;
        TEST_ASSERT_EQUAL_MESSAGE(FW_MANIFEST_ERR_CONTENT, fw_manifest_parse_payload(bad[i], strlen(bad[i]), &m),
                                  bad[i]);
        TEST_ASSERT_EQUAL_STRING("", m.version);
    }
    /* Correctly signed, still rejected. */
    fw_manifest_t m;
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_CONTENT, sign_and_verify(bad[1], &m));
}

TEST_CASE("manifest content: receiver entries and their limit", "[manifest]")
{
#define G(model, ver, build)                                                                                         \
    "{\"model\":\"" model "\",\"version\":\"" ver "\",\"build\":" #build ",\"asset\":\"p" #build ".pkg\",\"size\":" \
    "3027628,\"sha256\":\"" SHA_A "\"}"
    const char *four = "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[" G("UM982", "R4.10Build18975", 18975)
        "," G("UM982", "R4.10Build13495", 13495) "," G("UM980", "R4.10Build11833", 11833) "," G("UM982", "custom-7", 7)
        "]}";
    fw_manifest_t m;
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, fw_manifest_parse_payload(four, strlen(four), &m));
    TEST_ASSERT_EQUAL_UINT8(4, m.gnss_count);
    TEST_ASSERT_EQUAL_STRING("UM980", m.gnss[2].model);
    TEST_ASSERT_EQUAL_UINT32(13495, m.gnss[1].build);
    TEST_ASSERT_EQUAL_STRING("p7.pkg", m.gnss[3].file.asset);

    const char *five = "{\"schema\":1,\"version\":\"v1.0.0\"," APP_OK ",\"gnss\":[" G("UM982", "R1", 1) "," G(
        "UM982", "R2", 2) "," G("UM982", "R3", 3) "," G("UM982", "R4", 4) "," G("UM982", "R5", 5) "]}";
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_CONTENT, fw_manifest_parse_payload(five, strlen(five), &m));
#undef G
}

TEST_CASE("manifest asset names are plain file names", "[manifest]")
{
    static const char *const good[] = { "onocoydiy-esp-um982.bin", "UM982_R4.10Build18975.pkg", "manifest.json", "a" };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(fw_manifest_asset_name_ok(good[i]), good[i]);
    }
    static const char *const bad[] = { "", ".", "..", "../x", "a/b", "a b", "a%2Fb", "a?b", "a#b", "a\\b", "\xc3\xa9", NULL };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(fw_manifest_asset_name_ok(bad[i]), bad[i] ? bad[i] : "(null)");
    }
}

/* ---- what the release tool writes ----------------------------------------------------- */

/* tools/release_manifest.py without a signing key (plain) */
static const char TOOL_PLAIN[] =
    "{\n  \"schema\": 1,\n  \"version\": \"v0.3.0\",\n  \"app\": {\n    \"asset\": \"onocoydiy-esp-um982.bin"
    "\",\n    \"size\": 1438240,\n    \"sha256\": \"6e64cf26bd6e0148cf909905b8a6f6d2a34ac7f3f4e7d9b413a28a9b4"
    "4d776aa\"\n  },\n  \"gnss\": [\n    {\n      \"model\": \"UM982\",\n      \"version\": \"R4.10Build18975"
    "\",\n      \"build\": 18975,\n      \"asset\": \"UM982_R4.10Build18975.pkg\",\n      \"size\": 3027628,"
    "\n      \"sha256\": \"9ac25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71\"\n    }\n  ]\n}"
    "\n";
/* the same release signed with --sign-key (a throwaway key, private half not kept) */
static const char TOOL_SIGNED[] =
    "{\n  \"payload\": \"ewogICJzY2hlbWEiOiAxLAogICJ2ZXJzaW9uIjogInYwLjMuMCIsCiAgImFwcCI6IHsKICAgICJhc3NldCI6"
    "ICJvbm9jb3lkaXktZXNwLXVtOTgyLmJpbiIsCiAgICAic2l6ZSI6IDE0MzgyNDAsCiAgICAic2hhMjU2IjogIjZlNjRjZjI2YmQ2ZTAx"
    "NDhjZjkwOTkwNWI4YTZmNmQyYTM0YWM3ZjNmNGU3ZDliNDEzYTI4YTliNDRkNzc2YWEiCiAgfSwKICAiZ25zcyI6IFsKICAgIHsKICAg"
    "ICAgIm1vZGVsIjogIlVNOTgyIiwKICAgICAgInZlcnNpb24iOiAiUjQuMTBCdWlsZDE4OTc1IiwKICAgICAgImJ1aWxkIjogMTg5NzUs"
    "CiAgICAgICJhc3NldCI6ICJVTTk4Ml9SNC4xMEJ1aWxkMTg5NzUucGtnIiwKICAgICAgInNpemUiOiAzMDI3NjI4LAogICAgICAic2hh"
    "MjU2IjogIjlhYzI1ZDFkMzU5ZTU4MTViMTMyOTFmNjE0MDVmYWVmNTNkNzFlMzA2YTYwMjE4NDU0NmIzMjU4NGIzNWVjNzEiCiAgICB9"
    "CiAgXQp9Cg==\",\n  \"signature\": \"MEQCIDHw5/NvbzYawlHb8Jq0ij/+TTxIEIJ7NjltFyznaA0/AiBtwq1uPmjj3rNr3EpH"
    "YV0q8r+Oy1h4i1Cutlz18Aj18w==\"\n}\n";

static const char TOOL_KEY_TEXT[] = "04:22:bc:33:ff:33:29:33:dd:39:8b:23:bf:11:75:17:9c:21:35:aa:ff:ae:e5:fd:37:fc:a8"
                                    ":d1:d8:2e:e5:bb:56:30:08:1a:b6:0b:70:c2:35:d2:5a:32:4c:aa:29:e2:48:58:50:00:84"
                                    ":e6:88:cc:05:20:a2:44:8b:3c:99:11:26";

static void assert_tool_release(const fw_manifest_t *m)
{
    TEST_ASSERT_EQUAL_STRING("v0.3.0", m->version);
    TEST_ASSERT_EQUAL_STRING("onocoydiy-esp-um982.bin", m->app.asset);
    TEST_ASSERT_EQUAL_UINT32(1438240, m->app.size);
    TEST_ASSERT_EQUAL_HEX8(0x6e, m->app.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xaa, m->app.sha256[31]);
    TEST_ASSERT_EQUAL_UINT8(1, m->gnss_count);
    TEST_ASSERT_EQUAL_STRING("UM982", m->gnss[0].model);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", m->gnss[0].version);
    TEST_ASSERT_EQUAL_UINT32(18975, m->gnss[0].build);
    TEST_ASSERT_EQUAL_STRING("UM982_R4.10Build18975.pkg", m->gnss[0].file.asset);
    TEST_ASSERT_EQUAL_UINT32(3027628, m->gnss[0].file.size);
    TEST_ASSERT_EQUAL_HEX8(0x9a, m->gnss[0].file.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0x71, m->gnss[0].file.sha256[31]);
}

TEST_CASE("manifest: the release tool's plain and signed output read back", "[manifest]")
{
    fw_manifest_key_t key;
    TEST_ASSERT_TRUE(fw_manifest_key_from_hex(TOOL_KEY_TEXT, &key));
    fw_manifest_t m;
    int idx = -2;

    /* No key in the build: both forms. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(TOOL_PLAIN, NULL, 0, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    assert_tool_release(&m);
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(TOOL_SIGNED, NULL, 0, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    assert_tool_release(&m);

    /* The tool's key in the build: signed only. */
    TEST_ASSERT_EQUAL(FW_MANIFEST_OK, verify_str(TOOL_SIGNED, &key, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(0, idx);
    assert_tool_release(&m);
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_UNSIGNED, verify_str(TOOL_PLAIN, &key, 1, &m, &idx));
    TEST_ASSERT_EQUAL_INT(-1, idx);
    /* Another key in the build: the tool's signature does not count. */
    fw_manifest_key_t other = fixture_key();
    TEST_ASSERT_EQUAL(FW_MANIFEST_ERR_SIGNATURE, verify_str(TOOL_SIGNED, &other, 1, &m, NULL));
}

/* ---- per-asset size and SHA-256 ------------------------------------------------------- */

/* A manifest entry for `data`, as the release tool writes it. */
static fw_manifest_file_t file_for(const uint8_t *data, size_t len)
{
    fw_manifest_file_t f;
    memset(&f, 0, sizeof(f));
    strcpy(f.asset, "fw.bin");
    f.size = (uint32_t)len;
    size_t n = 0;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_crypto_init());
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, data, len, f.sha256, sizeof(f.sha256), &n));
    return f;
}

/* Feed `data` in pieces of `step` bytes; the first failing result, or finish(). */
static fw_asset_result_t feed(const fw_manifest_file_t *f, const uint8_t *data, size_t len, size_t step)
{
    fw_asset_check_t c;
    TEST_ASSERT_EQUAL(FW_ASSET_OK, fw_asset_check_begin(&c, f));
    for (size_t off = 0; off < len; off += step) {
        size_t n = len - off < step ? len - off : step;
        fw_asset_result_t r = fw_asset_check_update(&c, data + off, n);
        if (r != FW_ASSET_OK) {
            TEST_ASSERT_FALSE(c.open);   /* closed on the failure */
            return r;
        }
    }
    fw_asset_result_t r = fw_asset_check_finish(&c);
    TEST_ASSERT_FALSE(c.open);
    return r;
}

TEST_CASE("asset check: the manifest's bytes pass, in any chunking", "[manifest]")
{
    static uint8_t img[10000];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 7u + 3u);
    }
    fw_manifest_file_t f = file_for(img, sizeof(img));
    TEST_ASSERT_EQUAL(FW_ASSET_OK, feed(&f, img, sizeof(img), 4096));
    TEST_ASSERT_EQUAL(FW_ASSET_OK, feed(&f, img, sizeof(img), 1));
    TEST_ASSERT_EQUAL(FW_ASSET_OK, feed(&f, img, sizeof(img), sizeof(img)));
    TEST_ASSERT_EQUAL(FW_ASSET_OK, fw_asset_check_length(&f, -1));   /* not announced */
    TEST_ASSERT_EQUAL(FW_ASSET_OK, fw_asset_check_length(&f, (int64_t)sizeof(img)));
}

TEST_CASE("asset check: tampered bytes or a wrong size are refused", "[manifest]")
{
    static uint8_t img[10000];
    static uint8_t bigger[10001];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 13u + 1u);
    }
    memcpy(bigger, img, sizeof(img));
    bigger[sizeof(img)] = 0xAA;
    fw_manifest_file_t f = file_for(img, sizeof(img));

    /* One byte changed anywhere: same size, other digest. */
    static const size_t where[] = { 0, 4095, 4096, sizeof(img) - 1 };
    for (size_t i = 0; i < sizeof(where) / sizeof(where[0]); i++) {
        img[where[i]] ^= 0x01;
        TEST_ASSERT_EQUAL(FW_ASSET_ERR_SHA256, feed(&f, img, sizeof(img), 4096));
        img[where[i]] ^= 0x01;
    }
    TEST_ASSERT_EQUAL(FW_ASSET_OK, feed(&f, img, sizeof(img), 4096));

    /* Cut short, or one byte too many (refused as it arrives, before finish). */
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, feed(&f, img, sizeof(img) - 1, 4096));
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, feed(&f, bigger, sizeof(bigger), 4096));
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, feed(&f, bigger, sizeof(bigger), 1));

    /* A Content-Length that cannot be the asset. */
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, fw_asset_check_length(&f, (int64_t)sizeof(img) - 1));
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, fw_asset_check_length(&f, (int64_t)sizeof(img) + 1));

    /* The manifest's own size or digest changed (an unsigned manifest that
     * was tampered with): the real file no longer matches it. */
    fw_manifest_file_t g = f;
    g.size += 1;
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SIZE, feed(&g, img, sizeof(img), 4096));
    g = f;
    g.sha256[17] ^= 0x80;
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_SHA256, feed(&g, img, sizeof(img), 4096));
}

TEST_CASE("asset check: a closed check refuses more work, abort is safe", "[manifest]")
{
    const uint8_t data[4] = { 1, 2, 3, 4 };
    fw_manifest_file_t f = file_for(data, sizeof(data));
    fw_asset_check_t c;
    TEST_ASSERT_EQUAL(FW_ASSET_OK, fw_asset_check_begin(&c, &f));
    TEST_ASSERT_EQUAL(FW_ASSET_OK, fw_asset_check_update(&c, data, 0));
    fw_asset_check_abort(&c);
    fw_asset_check_abort(&c);
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_CRYPTO, fw_asset_check_update(&c, data, sizeof(data)));
    TEST_ASSERT_EQUAL(FW_ASSET_ERR_CRYPTO, fw_asset_check_finish(&c));
    TEST_ASSERT_EQUAL_STRING("SHA-256 differs from the manifest", fw_asset_result_str(FW_ASSET_ERR_SHA256));
    TEST_ASSERT_EQUAL_STRING("not signed, and this build requires a signature",
                             fw_manifest_err_str(FW_MANIFEST_ERR_UNSIGNED));
}
