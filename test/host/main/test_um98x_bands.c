/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The signals a signal group configures: the bodies UM982 build 25164 gave
 * for ANT1GROUPA and ANT2GROUPA in each group it accepts (our bench,
 * 2026-10-14, no antenna), decoded with the bit order measured there, the
 * band counts per constellation, and the comparison with what a group
 * should configure.
 */
#include <string.h>

#include "um98x_bands.h"
#include "unity.h"

/* Main antenna input (ANT1GROUPA) and second input (ANT2GROUPA), as recorded. */
static const struct {
    const char *main;
    const char *second;
} k_bench[] = {
    { "4,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x07,GQ,0x15", "5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05" },
    { "3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35", "6,4,GP,0x0D,GA,0x05,GB,0x05,GQ,0x05" },
    { "7,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15" },
    { "5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15" },
};

static um98x_bands_t parse(const char *body)
{
    um98x_bands_t b;
    TEST_ASSERT_TRUE_MESSAGE(um98x_bands_parse(body, strlen(body), &b), body);
    TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_OK, b.state);
    return b;
}

/* "E1 E5a E5b E6" for one constellation of a report. */
static void names(const um98x_bands_t *b, unsigned sys, char *out, size_t cap)
{
    out[0] = '\0';
    for (unsigned bit = 0; bit < 32; bit++) {
        if (b->mask[sys] & (1u << bit)) {
            const char *n = um98x_bands_signal_name(sys, bit);
            TEST_ASSERT_NOT_NULL(n);
            if (out[0] != '\0') {
                strncat(out, " ", cap - strlen(out) - 1);
            }
            strncat(out, n, cap - strlen(out) - 1);
        }
    }
}

TEST_CASE("bands: every report of build 25164 parses, the second input's too", "[um98x_bands]")
{
    for (size_t i = 0; i < sizeof(k_bench) / sizeof(k_bench[0]); i++) {
        um98x_bands_t m = parse(k_bench[i].main);
        um98x_bands_t s = parse(k_bench[i].second);
        TEST_ASSERT_EQUAL_HEX8(0x1F, m.present);   /* all five constellations */
        (void)s;
    }
    um98x_bands_t s36 = parse(k_bench[1].second);
    TEST_ASSERT_EQUAL_UINT8(6, s36.group);
    TEST_ASSERT_EQUAL_HEX8((1u << UM98X_BSYS_GPS) | (1u << UM98X_BSYS_GALILEO) | (1u << UM98X_BSYS_BEIDOU) |
                               (1u << UM98X_BSYS_QZSS),
                           s36.present);   /* no GLONASS on the second input in 3 6 */
    TEST_ASSERT_EQUAL_HEX32(0, s36.mask[UM98X_BSYS_GLONASS]);
}

TEST_CASE("bands: the main input's signals and bands per group, decoded", "[um98x_bands]")
{
    char t[64];
    um98x_bands_t b = parse(k_bench[1].main);   /* 3 6 */
    TEST_ASSERT_EQUAL_UINT8(3, b.group);
    names(&b, UM98X_BSYS_GALILEO, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("E1 E5a E5b E6", t);
    TEST_ASSERT_EQUAL_UINT8(4, um98x_bands_count(UM98X_BSYS_GALILEO, b.mask[UM98X_BSYS_GALILEO]));
    names(&b, UM98X_BSYS_BEIDOU, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("B1I B3I B1C B2b", t);
    TEST_ASSERT_EQUAL_UINT8(3, um98x_bands_count(UM98X_BSYS_BEIDOU, b.mask[UM98X_BSYS_BEIDOU]));
    names(&b, UM98X_BSYS_QZSS, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("L1C/A L2C L5 L6", t);
    names(&b, UM98X_BSYS_GPS, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("L1C/A L2C L2P L5", t);
    TEST_ASSERT_EQUAL_UINT8(3, um98x_bands_count(UM98X_BSYS_GPS, b.mask[UM98X_BSYS_GPS]));
    names(&b, UM98X_BSYS_GLONASS, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("G1 G2", t);

    b = parse(k_bench[2].main);   /* 7 0 */
    names(&b, UM98X_BSYS_BEIDOU, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("B1I B2I B3I B1C B2a B2b", t);
    TEST_ASSERT_EQUAL_UINT8(4, um98x_bands_count(UM98X_BSYS_BEIDOU, b.mask[UM98X_BSYS_BEIDOU]));
    names(&b, UM98X_BSYS_GALILEO, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("E1 E5a E5b", t);   /* no E6 */

    b = parse(k_bench[0].main);   /* 4 5: at most three bands anywhere */
    for (unsigned s = 0; s < UM98X_BANDS_SYS; s++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT8(3, um98x_bands_count(s, b.mask[s]));
    }
    b = parse(k_bench[3].main);   /* 5 0: no L5 at all */
    names(&b, UM98X_BSYS_GPS, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("L1C/A L2C L2P", t);
    names(&b, UM98X_BSYS_GALILEO, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("E1 E5b", t);

    TEST_ASSERT_EQUAL_STRING("galileo", um98x_bands_sys_name(UM98X_BSYS_GALILEO));
    TEST_ASSERT_NULL(um98x_bands_sys_name(UM98X_BANDS_SYS));
    TEST_ASSERT_NULL(um98x_bands_signal_name(UM98X_BSYS_QZSS, 3));   /* never seen set */
    TEST_ASSERT_NULL(um98x_bands_signal_name(UM98X_BSYS_GLONASS, 3));
}

TEST_CASE("bands: each report matches its own group and no other", "[um98x_bands]")
{
    static const uint8_t groups[] = { 4, 3, 7, 5 };
    for (size_t i = 0; i < 4; i++) {
        um98x_bands_t b = parse(k_bench[i].main);
        uint32_t missing[UM98X_BANDS_SYS];
        TEST_ASSERT_TRUE(um98x_bands_match(groups[i], &b, missing));
        for (unsigned s = 0; s < UM98X_BANDS_SYS; s++) {
            TEST_ASSERT_EQUAL_HEX32(0, missing[s]);
        }
        for (size_t j = 0; j < 4; j++) {
            if (j != i) {
                TEST_ASSERT_FALSE(um98x_bands_match(groups[j], &b, NULL));
            }
        }
    }

    /* The group is 3 but E6 is missing: the alert's case, with what lacks. */
    um98x_bands_t b = parse("3,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x2D,GQ,0x35");
    uint32_t missing[UM98X_BANDS_SYS];
    TEST_ASSERT_FALSE(um98x_bands_match(3, &b, missing));
    TEST_ASSERT_EQUAL_HEX32(0x08, missing[UM98X_BSYS_GALILEO]);
    TEST_ASSERT_EQUAL_STRING("E6", um98x_bands_signal_name(UM98X_BSYS_GALILEO, 3));

    /* More signals than expected still match (a newer firmware may add some). */
    b = parse("3,5,GP,0x1F,GL,0x07,GA,0x0F,GB,0x3F,GQ,0x37");
    TEST_ASSERT_TRUE(um98x_bands_match(3, &b, NULL));

    /* A group with no expectation: only the number counts. */
    b = parse("1,1,GP,0x01");
    TEST_ASSERT_TRUE(um98x_bands_match(1, &b, NULL));
    TEST_ASSERT_FALSE(um98x_bands_match(2, &b, NULL));

    /* Not a report: never a match. */
    um98x_bands_t none;
    memset(&none, 0, sizeof(none));
    TEST_ASSERT_FALSE(um98x_bands_match(3, &none, NULL));
    none.state = UM98X_BANDS_REFUSED;
    TEST_ASSERT_FALSE(um98x_bands_match(3, &none, NULL));
}

TEST_CASE("bands: malformed bodies are not read, odd ones are", "[um98x_bands]")
{
    static const char *const bad[] = {
        "", "3", "3,", "x,5,GP,0x1D", "3,2,GP,0x1D", "3,1,GP", "3,1,GP,0xZZ", "3,1,,0x1D", "3,1,GP,0x1D,GL",
        "256,1,GP,0x1D", "3,17,GP,0x1D", "3,1,GP,0x1D,GL,0x03",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        um98x_bands_t b;
        TEST_ASSERT_FALSE_MESSAGE(um98x_bands_parse(bad[i], strlen(bad[i]), &b), bad[i]);
        TEST_ASSERT_EQUAL_UINT8(UM98X_BANDS_UNREAD, b.state);
    }
    /* Spaces around fields, a trailing comma, decimal masks, lower-case
     * tokens and a constellation this firmware does not decode (skipped). */
    um98x_bands_t b = parse(" 3 , 3 , gp , 29 , GI , 0x01 , GA , 0x0F ,");
    TEST_ASSERT_EQUAL_UINT8(3, b.group);
    TEST_ASSERT_EQUAL_HEX32(0x1D, b.mask[UM98X_BSYS_GPS]);
    TEST_ASSERT_EQUAL_HEX32(0x0F, b.mask[UM98X_BSYS_GALILEO]);
    TEST_ASSERT_EQUAL_HEX8((1u << UM98X_BSYS_GPS) | (1u << UM98X_BSYS_GALILEO), b.present);
    TEST_ASSERT_FALSE(um98x_bands_parse(NULL, 0, &b));
    TEST_ASSERT_FALSE(um98x_bands_parse("3,0", 3, NULL));
    b = parse("3,0");   /* an empty list is still a report */
    TEST_ASSERT_EQUAL_HEX8(0, b.present);
}
