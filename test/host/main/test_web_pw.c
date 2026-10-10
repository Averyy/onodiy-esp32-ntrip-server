/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "web_core.h"
#include "web_pw.h"

static void hex32(const char *hex, uint8_t out[32])
{
    TEST_ASSERT_TRUE(web_parse_sha256_hex(hex, out));
}

TEST_CASE("PBKDF2-HMAC-SHA256 through PSA matches the published vectors", "[web_pw]")
{
    static const struct {
        uint32_t c;
        const char *dk;
    } vec[] = {
        { 1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b" },
        { 2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43" },
        { 4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" },
    };
    for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        uint8_t want[32];
        uint8_t got[32];
        hex32(vec[i].dk, want);
        TEST_ASSERT_TRUE(web_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, vec[i].c, got));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, 32);
    }
}

TEST_CASE("password record: make, check, encode, decode", "[web_pw]")
{
    uint8_t salt[WEB_PW_SALT_LEN];
    for (size_t i = 0; i < sizeof(salt); i++) {
        salt[i] = (uint8_t)i;
    }
    web_pw_record_t rec;
    TEST_ASSERT_TRUE(web_pw_make("correct horse", salt, 1000, &rec));
    uint8_t want[32];
    hex32("c914cc4f06cc6e8f46d157e3a1b5aa7abceebb17bb0444cd4c4ac16ca2ae9864", want);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, rec.hash, 32);
    TEST_ASSERT_TRUE(web_pw_check(&rec, "correct horse"));
    TEST_ASSERT_FALSE(web_pw_check(&rec, "correct horsE"));
    TEST_ASSERT_FALSE(web_pw_check(&rec, ""));

    uint8_t blob[WEB_PW_BLOB_LEN];
    TEST_ASSERT_EQUAL(WEB_PW_BLOB_LEN, web_pw_encode(&rec, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_HEX8(1, blob[0]);                  /* version */
    TEST_ASSERT_EQUAL_HEX8(0xE8, blob[1]);               /* 1000, little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x03, blob[2]);
    web_pw_record_t back;
    TEST_ASSERT_TRUE(web_pw_decode(blob, sizeof(blob), &back));
    TEST_ASSERT_EQUAL_UINT32(1000, back.iterations);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rec.salt, back.salt, WEB_PW_SALT_LEN);
    TEST_ASSERT_TRUE(web_pw_check(&back, "correct horse"));

    /* The stored count is used, whatever the build's default. */
    TEST_ASSERT_TRUE(web_pw_make("correct horse", salt, 2000, &rec));
    TEST_ASSERT_TRUE(web_pw_check(&rec, "correct horse"));
    TEST_ASSERT_FALSE(memcmp(want, rec.hash, 32) == 0);

    TEST_ASSERT_EQUAL(0, web_pw_encode(&rec, blob, sizeof(blob) - 1));
    TEST_ASSERT_FALSE(web_pw_make("x", salt, 10, &rec));   /* cost below the floor */
}

TEST_CASE("password record decode refuses other layouts", "[web_pw]")
{
    uint8_t salt[WEB_PW_SALT_LEN] = { 0 };
    web_pw_record_t rec;
    TEST_ASSERT_TRUE(web_pw_make("12345678", salt, 1000, &rec));
    uint8_t blob[WEB_PW_BLOB_LEN + 1];
    TEST_ASSERT_EQUAL(WEB_PW_BLOB_LEN, web_pw_encode(&rec, blob, sizeof(blob)));
    web_pw_record_t out;
    TEST_ASSERT_FALSE(web_pw_decode(blob, WEB_PW_BLOB_LEN - 1, &out));
    TEST_ASSERT_FALSE(web_pw_decode(blob, WEB_PW_BLOB_LEN + 1, &out));
    blob[0] = 2;
    TEST_ASSERT_FALSE(web_pw_decode(blob, WEB_PW_BLOB_LEN, &out));
    blob[0] = 1;
    blob[1] = 0;
    blob[2] = 0;   /* 0 iterations */
    TEST_ASSERT_FALSE(web_pw_decode(blob, WEB_PW_BLOB_LEN, &out));
}

TEST_CASE("constant-time compare", "[web_pw]")
{
    uint8_t a[4] = { 1, 2, 3, 4 };
    uint8_t b[4] = { 1, 2, 3, 4 };
    TEST_ASSERT_TRUE(web_ct_equal(a, b, 4));
    b[3] = 5;
    TEST_ASSERT_FALSE(web_ct_equal(a, b, 4));
    TEST_ASSERT_TRUE(web_ct_equal(a, b, 0));
}
