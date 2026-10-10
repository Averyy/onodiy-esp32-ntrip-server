/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "web_json.h"

static char s_buf[512];

static const char *build(void (*fn)(wj_t *w))
{
    wj_t w;
    wj_init(&w, s_buf, sizeof(s_buf), NULL, NULL);
    fn(&w);
    TEST_ASSERT_TRUE(wj_finish(&w));
    return wj_text(&w);
}

static void nested(wj_t *w)
{
    wj_obj_open(w, NULL);
    wj_int(w, "a", -3);
    wj_arr_open(w, "list");
    wj_uint(w, NULL, 1);
    wj_obj_open(w, NULL);
    wj_bool(w, "x", true);
    wj_obj_close(w);
    wj_null(w, NULL);
    wj_arr_close(w);
    wj_str(w, "s", "hi");
    wj_obj_close(w);
}

TEST_CASE("json writer places commas and nesting", "[web_json]")
{
    TEST_ASSERT_EQUAL_STRING("{\"a\":-3,\"list\":[1,{\"x\":true},null],\"s\":\"hi\"}", build(nested));
}

static void escapes(wj_t *w)
{
    wj_obj_open(w, NULL);
    wj_str(w, "q", "a\"b\\c");
    wj_str(w, "ctl", "l1\nl2\r\t\x01\x7f");
    wj_str(w, "utf8", "caf\xc3\xa9 \xe2\x82\xac");
    wj_str(w, "bad", "x\xff\xc3y");
    wj_strn(w, "nul", "a\0b", 3);
    wj_obj_close(w);
}

TEST_CASE("json writer escapes control bytes and keeps valid UTF-8", "[web_json]")
{
    TEST_ASSERT_EQUAL_STRING("{\"q\":\"a\\\"b\\\\c\",\"ctl\":\"l1\\nl2\\r\\t\\u0001\\u007f\","
                             "\"utf8\":\"caf\xc3\xa9 \xe2\x82\xac\",\"bad\":\"x\\u00ff\\u00c3y\",\"nul\":\"a\\u0000b\"}",
                             build(escapes));
}

static void sentinels(wj_t *w)
{
    wj_obj_open(w, NULL);
    wj_str(w, "n", NULL);
    wj_str_or_null(w, "e", "");
    wj_u32_or_null(w, "never", UINT32_MAX);
    wj_u32_or_null(w, "v", 7);
    wj_unix(w, "t0", 0);
    wj_unix(w, "t", 1760000000);
    wj_double(w, "lat", 45.123456789, 9);
    wj_double(w, "neg0", -0.00001, 4);
    wj_double(w, "nan", NAN, 4);
    wj_double(w, "inf", INFINITY, 2);
    wj_uint(w, "big", 9007199254740991ULL);
    wj_obj_close(w);
}

TEST_CASE("json writer maps sentinels to null and formats numbers", "[web_json]")
{
    TEST_ASSERT_EQUAL_STRING("{\"n\":null,\"e\":null,\"never\":null,\"v\":7,\"t0\":null,\"t\":1760000000,"
                             "\"lat\":45.123456789,\"neg0\":0.0000,\"nan\":null,\"inf\":null,\"big\":9007199254740991}",
                             build(sentinels));
}

static void hexes(wj_t *w)
{
    static const uint8_t d[] = { 0x00, 0x9a, 0xff };
    wj_arr_open(w, NULL);
    wj_hex(w, NULL, d, sizeof(d));
    wj_addr(w, NULL, 0x4200abcd);
    wj_raw(w, NULL, "{\"r\":1}", 7);
    wj_arr_close(w);
}

TEST_CASE("json writer writes hex, addresses and raw values", "[web_json]")
{
    TEST_ASSERT_EQUAL_STRING("[\"009aff\",\"0x4200abcd\",{\"r\":1}]", build(hexes));
}

TEST_CASE("json writer reports overflow without a flush callback", "[web_json]")
{
    char small[8];
    wj_t w;
    wj_init(&w, small, sizeof(small), NULL, NULL);
    wj_obj_open(&w, NULL);
    wj_str(&w, "key", "value");
    wj_obj_close(&w);
    TEST_ASSERT_FALSE(wj_ok(&w));
    TEST_ASSERT_FALSE(wj_finish(&w));
    TEST_ASSERT_TRUE(strlen(wj_text(&w)) < sizeof(small));
}

TEST_CASE("json writer refuses keys in arrays and missing keys in objects", "[web_json]")
{
    wj_t w;
    wj_init(&w, s_buf, sizeof(s_buf), NULL, NULL);
    wj_arr_open(&w, NULL);
    wj_int(&w, "k", 1);
    TEST_ASSERT_FALSE(wj_ok(&w));

    wj_init(&w, s_buf, sizeof(s_buf), NULL, NULL);
    wj_obj_open(&w, NULL);
    wj_int(&w, NULL, 1);
    TEST_ASSERT_FALSE(wj_ok(&w));

    wj_init(&w, s_buf, sizeof(s_buf), NULL, NULL);
    wj_obj_open(&w, NULL);
    wj_arr_close(&w);
    TEST_ASSERT_FALSE(wj_ok(&w));
}

typedef struct {
    char out[1024];
    size_t len;
    int calls;
} sink_t;

static bool sink(void *ctx, const char *data, size_t len)
{
    sink_t *s = ctx;
    TEST_ASSERT_TRUE(s->len + len < sizeof(s->out));
    memcpy(s->out + s->len, data, len);
    s->len += len;
    s->out[s->len] = '\0';
    s->calls++;
    return true;
}

TEST_CASE("json writer streams through a small buffer", "[web_json]")
{
    char small[16];
    sink_t s = { .len = 0, .calls = 0 };
    wj_t w;
    wj_init(&w, small, sizeof(small), sink, &s);
    nested(&w);
    TEST_ASSERT_TRUE(wj_finish(&w));
    TEST_ASSERT_TRUE(s.calls > 1);
    TEST_ASSERT_EQUAL_STRING("{\"a\":-3,\"list\":[1,{\"x\":true},null],\"s\":\"hi\"}", s.out);
}

TEST_CASE("json fragments are members or elements without brackets", "[web_json]")
{
    wj_t w;
    wj_init_members(&w, s_buf, sizeof(s_buf));
    wj_obj_open(&w, "device");
    wj_str(&w, "hostname", "x");
    wj_obj_close(&w);
    wj_int(&w, "n", 2);
    TEST_ASSERT_TRUE(wj_ok(&w));
    TEST_ASSERT_EQUAL_STRING("\"device\":{\"hostname\":\"x\"},\"n\":2", wj_text(&w));

    wj_init_elements(&w, s_buf, sizeof(s_buf));
    wj_obj_open(&w, NULL);
    wj_int(&w, "slot", 0);
    wj_obj_close(&w);
    wj_obj_open(&w, NULL);
    wj_int(&w, "slot", 1);
    wj_obj_close(&w);
    TEST_ASSERT_EQUAL_STRING("{\"slot\":0},{\"slot\":1}", wj_text(&w));
}

TEST_CASE("UTF-8 sequence check refuses overlongs and surrogates", "[web_json]")
{
    TEST_ASSERT_EQUAL(1, wj_utf8_seq_len((const uint8_t *)"a", 1));
    TEST_ASSERT_EQUAL(2, wj_utf8_seq_len((const uint8_t *)"\xc3\xa9", 2));
    TEST_ASSERT_EQUAL(3, wj_utf8_seq_len((const uint8_t *)"\xe2\x82\xac", 3));
    TEST_ASSERT_EQUAL(4, wj_utf8_seq_len((const uint8_t *)"\xf0\x9f\x98\x80", 4));
    TEST_ASSERT_EQUAL(0, wj_utf8_seq_len((const uint8_t *)"\xc0\xaf", 2));          /* overlong '/' */
    TEST_ASSERT_EQUAL(0, wj_utf8_seq_len((const uint8_t *)"\xed\xa0\x80", 3));      /* surrogate */
    TEST_ASSERT_EQUAL(0, wj_utf8_seq_len((const uint8_t *)"\xf4\x90\x80\x80", 4));  /* > U+10FFFF */
    TEST_ASSERT_EQUAL(0, wj_utf8_seq_len((const uint8_t *)"\xe2\x82", 2));          /* cut short */
    TEST_ASSERT_EQUAL(0, wj_utf8_seq_len((const uint8_t *)"\x80", 1));
}
