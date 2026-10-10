/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Version log parser. The vectors are SYNTHETIC (see um98x_test_vectors.h).
 */
#include <stdio.h>
#include <string.h>

#include "gnss_framing.h"
#include "um98x_test_vectors.h"
#include "unity.h"

/* Test-side CRC to build mutated vectors (the main vectors' CRCs come from an
 * independent implementation, see um98x_test_vectors.h). */
static uint32_t ref_crc32(const char *s, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t)s[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc;
}

/* "#<content>*crc" with a correct CRC. */
static void make_log(char *out, size_t cap, const char *content)
{
    uint32_t crc = ref_crc32(content, strlen(content));
    snprintf(out, cap, "#%s*%08x", content, (unsigned)crc);
}

static bool parse(const char *line, gnss_versiona_t *v)
{
    return gnss_versiona_parse(line, strlen(line), v);
}

TEST_CASE("version log: synthetic UM982 line parses into the body fields", "[versiona]")
{
    gnss_versiona_t v;
    TEST_ASSERT_TRUE(parse(TV_VERSIONA_UM982, &v));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", v.firmware);
    TEST_ASSERT_EQUAL_UINT32(18975, v.build);
    TEST_ASSERT_EQUAL_STRING("1234567890123", v.pn);
    TEST_ASSERT_EQUAL_STRING("SYNTH00000001", v.serial);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", v.efuse_id);
    TEST_ASSERT_EQUAL_STRING("2025/04/18", v.compile_date);

    /* Another header layout: only the body is used. */
    TEST_ASSERT_TRUE(parse(TV_VERSIONA_UM980, &v));
    TEST_ASSERT_EQUAL_STRING("UM980", v.model);
    TEST_ASSERT_EQUAL_UINT32(7923, v.build);

    /* Trailing CR LF is tolerated. */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\r\n", TV_VERSIONA_UM982);
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
}

TEST_CASE("version log: CRC hex digits in either case, every changed byte rejected", "[versiona]")
{
    char buf[512];
    gnss_versiona_t v;
    strcpy(buf, TV_VERSIONA_UM982);
    size_t len = strlen(buf);
    for (size_t i = len - 8; i < len; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'f') {
            buf[i] = (char)(buf[i] - 'a' + 'A');
        }
    }
    TEST_ASSERT_TRUE(parse(buf, &v));

    /* Flip one bit anywhere in the checked bytes (or the CRC itself). */
    for (size_t i = 1; i < len; i++) {
        strcpy(buf, TV_VERSIONA_UM982);
        if (buf[i] == '*') {
            continue;
        }
        buf[i] ^= 0x01;
        memset(&v, 0x5A, sizeof(v));
        TEST_ASSERT_FALSE_MESSAGE(parse(buf, &v), buf);
        TEST_ASSERT_EQUAL_STRING("", v.model);
        TEST_ASSERT_EQUAL_UINT32(0, v.build);
    }
}

TEST_CASE("version log: prefix, field count and form are enforced", "[versiona]")
{
    char buf[512];
    gnss_versiona_t v;

    /* The plain (unquoted) version log is not accepted. */
    make_log(buf, sizeof(buf), "VERSION,COM1;UM982,R4.10Build18975,HRPT00-S10C-P,1-2,ff,2025/04/18");
    TEST_ASSERT_FALSE(parse(buf, &v));

    /* Five body fields are too few; six suffice; more are fine. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"1-2\",\"ff\"");
    TEST_ASSERT_FALSE(parse(buf, &v));
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    make_log(buf, sizeof(buf),
             "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"1-2\",\"ff\",\"2025/04/18\",\"extra\",\"x\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("2025/04/18", v.compile_date);

    /* No ';' between header and body. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1,\"UM982\",\"R4.10Build18975\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_FALSE(parse(buf, &v));

    /* Header split at the FIRST ';' only. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"R4.10;Build18975\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("R4.10;Build18975", v.firmware);

    /* Unquoted fields are taken as they are; one pair of quotes is stripped. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;UM982,\"\"R4.10Build18975\"\",A,1-2,ff,2025/04/18");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("UM982", v.model);
    TEST_ASSERT_EQUAL_STRING("\"R4.10Build18975\"", v.firmware);

    /* PN-SN without '-': all PN, no serial; split at the first '-' only. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"12345\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("12345", v.pn);
    TEST_ASSERT_EQUAL_STRING("", v.serial);
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"1-2-3\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_STRING("1", v.pn);
    TEST_ASSERT_EQUAL_STRING("2-3", v.serial);

    /* NULL arguments and a NUL inside the line. */
    TEST_ASSERT_FALSE(gnss_versiona_parse(NULL, 10, &v));
    TEST_ASSERT_FALSE(gnss_versiona_parse(TV_VERSIONA_UM982, strlen(TV_VERSIONA_UM982), NULL));
    strcpy(buf, TV_VERSIONA_UM982);
    TEST_ASSERT_FALSE(gnss_versiona_parse(buf, strlen(buf) + 1, &v));
}

TEST_CASE("version log: model and firmware must fit whole, display fields are cut", "[versiona]")
{
    char buf[512];
    gnss_versiona_t v;

    /* Empty model or firmware. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"\",\"R4.10Build18975\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_FALSE(parse(buf, &v));
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982\",\"\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_FALSE(parse(buf, &v));

    /* A model longer than the field could be cut into a supported one: rejected. */
    make_log(buf, sizeof(buf), "VERSIONA,COM1;\"UM982XXXXXXXXXXXXXXX\",\"R4\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"");
    TEST_ASSERT_FALSE(parse(buf, &v));
    make_log(buf, sizeof(buf),
             "VERSIONA,COM1;\"UM982\",\"R4.10Build18975-0123456789012345678901234567890123\",\"A\",\"1-2\",\"ff\","
             "\"2025/04/18\"");
    TEST_ASSERT_FALSE(parse(buf, &v));

    /* Long display fields are cut to their buffers. */
    make_log(buf, sizeof(buf),
             "VERSIONA,COM1;\"UM982\",\"R4.10Build18975\",\"A\",\"1-2\",\"0123456789abcdef0123456789abcdef\","
             "\"2025/04/18\"");
    TEST_ASSERT_TRUE(parse(buf, &v));
    TEST_ASSERT_EQUAL_size_t(sizeof(v.efuse_id) - 1, strlen(v.efuse_id));
}

TEST_CASE("version log: build is the integer after Build, compared as an integer", "[versiona]")
{
    static const struct {
        const char *fw;
        uint32_t build;
    } cases[] = {
        { "R4.10Build18975", 18975 },
        { "R4.10Build1883", 1883 },
        { "R4.10Build18835", 18835 },
        { "R4.10Build", 0 },
        { "R4.10", 0 },
        { "R4.10Build99999999999", 0 },   /* does not fit 32 bits */
        { "Build42abc", 42 },
    };
    char content[256];
    char buf[512];
    gnss_versiona_t v;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(content, sizeof(content), "VERSIONA,COM1;\"UM982\",\"%s\",\"A\",\"1-2\",\"ff\",\"2025/04/18\"",
                 cases[i].fw);
        make_log(buf, sizeof(buf), content);
        TEST_ASSERT_TRUE(parse(buf, &v));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i].build, v.build, cases[i].fw);
    }
    /* A substring test would call 1883 "installed" on 18835; integers do not. */
    TEST_ASSERT_NOT_EQUAL(1883u, 18835u);
}
