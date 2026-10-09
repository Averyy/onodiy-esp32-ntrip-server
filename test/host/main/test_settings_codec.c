/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "settings_codec.h"
#include "unity.h"

TEST_CASE("codec round-trips every field type", "[settings_codec]")
{
    uint8_t buf[128];
    blob_writer_t w;
    blob_writer_init(&w, buf, sizeof(buf));
    blob_put_u8(&w, 0xA5);
    blob_put_u16(&w, 0xBEEF);
    blob_put_u32(&w, 0xDEADBEEFu);
    blob_put_u64(&w, 0x0123456789ABCDEFull);
    blob_put_i64(&w, -42);
    blob_put_f32(&w, 1.5f);
    blob_put_f64(&w, -123.456789012345);
    blob_put_bool(&w, true);
    blob_put_str(&w, "servers.example");
    blob_put_str(&w, "");
    TEST_ASSERT_TRUE(blob_writer_ok(&w));

    blob_reader_t r;
    blob_reader_init(&r, buf, w.len);
    TEST_ASSERT_EQUAL_HEX8(0xA5, blob_get_u8(&r));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, blob_get_u16(&r));
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, blob_get_u32(&r));
    TEST_ASSERT_EQUAL_UINT64(0x0123456789ABCDEFull, blob_get_u64(&r));
    TEST_ASSERT_EQUAL_INT64(-42, blob_get_i64(&r));
    TEST_ASSERT_TRUE(blob_get_f32(&r) == 1.5f);
    TEST_ASSERT_TRUE(blob_get_f64(&r) == -123.456789012345);
    TEST_ASSERT_TRUE(blob_get_bool(&r));
    char s[32];
    blob_get_str(&r, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("servers.example", s);
    blob_get_str(&r, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_TRUE(blob_reader_ok(&r));
    TEST_ASSERT_EQUAL_size_t(0, blob_reader_remaining(&r));
}

TEST_CASE("codec writes little-endian regardless of host", "[settings_codec]")
{
    uint8_t buf[4];
    blob_writer_t w;
    blob_writer_init(&w, buf, sizeof(buf));
    blob_put_u32(&w, 0x11223344u);
    TEST_ASSERT_TRUE(blob_writer_ok(&w));
    const uint8_t expected[4] = { 0x44, 0x33, 0x22, 0x11 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, 4);
}

TEST_CASE("codec errors are sticky on overflow and short input", "[settings_codec]")
{
    uint8_t buf[3];
    blob_writer_t w;
    blob_writer_init(&w, buf, sizeof(buf));
    blob_put_u16(&w, 1);
    blob_put_u16(&w, 2);           /* does not fit */
    blob_put_u8(&w, 3);            /* would fit, but the writer already failed */
    TEST_ASSERT_FALSE(blob_writer_ok(&w));
    TEST_ASSERT_EQUAL_size_t(2, w.len);

    blob_reader_t r;
    blob_reader_init(&r, buf, 2);
    (void)blob_get_u32(&r);        /* short */
    TEST_ASSERT_FALSE(blob_reader_ok(&r));
    TEST_ASSERT_EQUAL_UINT8(0, blob_get_u8(&r));
    TEST_ASSERT_EQUAL_size_t(0, blob_reader_remaining(&r));
}

TEST_CASE("codec rejects strings that do not fit", "[settings_codec]")
{
    uint8_t buf[32];
    blob_writer_t w;
    blob_writer_init(&w, buf, sizeof(buf));
    blob_put_str(&w, "abcdefgh");
    TEST_ASSERT_TRUE(blob_writer_ok(&w));

    blob_reader_t r;
    blob_reader_init(&r, buf, w.len);
    char small[8];                 /* 8 chars need 9 bytes */
    blob_get_str(&r, small, sizeof(small));
    TEST_ASSERT_FALSE(blob_reader_ok(&r));
    TEST_ASSERT_EQUAL_STRING("", small);

    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    uint8_t large[400];
    blob_writer_init(&w, large, sizeof(large));
    blob_put_str(&w, big);         /* longer than 255 */
    TEST_ASSERT_FALSE(blob_writer_ok(&w));
}

TEST_CASE("codec hash is FNV-1a 32", "[settings_codec]")
{
    TEST_ASSERT_EQUAL_HEX32(0x811C9DC5u, blob_hash32(NULL, 0));
    TEST_ASSERT_EQUAL_HEX32(0xE40C292Cu, blob_hash32((const uint8_t *)"a", 1));
    TEST_ASSERT_EQUAL_HEX32(0xBF9CF968u, blob_hash32((const uint8_t *)"foobar", 6));
}
