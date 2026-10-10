/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver reply lines from a real receiver (test/fixtures/unicore: reply
 * samples quoted in a public MIT-licensed library): checksum rules,
 * classification, dump items and the base-mode helpers.
 *
 * Checksum fact these lines confirm: the replies, dump items, restart
 * banner and the base-mode log end in a 2-hex XOR that includes the leading
 * '$' or '#' (unlike NMEA, which leaves the '$' out).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "gnss_framing.h"
#include "um98x_base.h"
#include "um98x_profile.h"
#include "um98x_reply.h"
#include "unity.h"

static cJSON *load_samples(void)
{
    size_t n;
    char *text = (char *)fx_read("unicore/sparkfun_reply_samples.json", &n);
    cJSON *j = cJSON_Parse(text);
    free(text);
    TEST_ASSERT_NOT_NULL(j);
    return j;
}

static const char *sample(const cJSON *j, const char *kind, unsigned nth)
{
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "samples"))
    {
        if (strcmp(cJSON_GetObjectItemCaseSensitive(s, "kind")->valuestring, kind) == 0 && nth-- == 0) {
            return cJSON_GetObjectItemCaseSensitive(s, "text")->valuestring;
        }
    }
    TEST_FAIL_MESSAGE(kind);
    return NULL;
}

TEST_CASE("fixtures: receiver reply checksums include the lead character", "[fixtures]")
{
    cJSON *j = load_samples();
    const cJSON *s;
    unsigned n = 0;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "samples"))
    {
        const char *line = cJSON_GetObjectItemCaseSensitive(s, "text")->valuestring;
        const size_t len = strlen(line);
        TEST_ASSERT_TRUE_MESSAGE(gnss_text_line_valid(line, len), line);
        unsigned want = (unsigned)strtoul(line + len - 2, NULL, 16);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(want, um98x_xor8(line, len - 3), line);       /* lead included */
        TEST_ASSERT_NOT_EQUAL_MESSAGE(want, um98x_xor8(line + 1, len - 4), line);    /* not the NMEA rule */
        /* Any changed character fails. */
        char tmp[160];
        TEST_ASSERT_LESS_THAN_size_t(sizeof(tmp), len + 1);
        for (size_t i = 1; i < len - 3; i++) {
            memcpy(tmp, line, len + 1);
            tmp[i] ^= 0x01;
            TEST_ASSERT_FALSE_MESSAGE(gnss_text_line_valid(tmp, len), line);
            um98x_line_t l;
            um98x_line_classify(tmp, len, &l);
            TEST_ASSERT_TRUE_MESSAGE(l.kind == UM98X_LINE_BAD || l.kind == UM98X_LINE_OTHER, line);
        }
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(8, n);
    cJSON_Delete(j);
}

TEST_CASE("fixtures: receiver replies classify with echo, response and dump items", "[fixtures]")
{
    cJSON *j = load_samples();
    um98x_line_t l;

    const char *banner = sample(j, "boot_banner", 0);
    um98x_line_classify(banner, strlen(banner), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_BANNER, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("COM3", l.text, l.text_len);
    TEST_ASSERT_EQUAL_size_t(4, l.text_len);

    const char *ok = sample(j, "command_ok", 0);
    um98x_line_classify(ok, strlen(ok), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_TRUE(l.ok);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "MODE"));
    TEST_ASSERT_EQUAL_STRING_LEN("OK", l.text, l.text_len);

    /* The error reply: the echo keeps the command's case, the reason repeats
     * it upper-cased after two spaces, and "FAILD" is the receiver's spelling. */
    const char *err = sample(j, "command_error", 0);
    um98x_line_classify(err, strlen(err), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_ACK, l.kind);
    TEST_ASSERT_FALSE(l.ok);
    TEST_ASSERT_EQUAL_STRING_LEN("badResponse", l.echo, l.echo_len);
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "badResponse"));
    TEST_ASSERT_TRUE(um98x_echo_matches(l.echo, l.echo_len, "BADRESPONSE"));
    TEST_ASSERT_FALSE(um98x_echo_matches(l.echo, l.echo_len, "MODE"));
    static const char k_reason[] = "PARSING FAILD NO MATCHING FUNC  BADRESPONSE";
    TEST_ASSERT_EQUAL_size_t(sizeof(k_reason) - 1, l.text_len);
    TEST_ASSERT_EQUAL_STRING_LEN(k_reason, l.text, l.text_len);

    /* Dump items: COM3 is not the link port; MASK reads back as 5.0 degrees. */
    um98x_config_readback_t rb;
    memset(&rb, 0, sizeof(rb));
    const char *com3 = sample(j, "config_dump", 0);
    um98x_line_classify(com3, strlen(com3), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_CONFIG, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("COM3", l.item, l.item_len);
    TEST_ASSERT_EQUAL_STRING_LEN("CONFIG COM3 115200", l.text, l.text_len);
    um98x_config_item(&rb, l.item, l.item_len, l.text, l.text_len);
    TEST_ASSERT_FALSE(rb.com1_baud_known);
    const char *mask = sample(j, "config_dump", 1);
    um98x_line_classify(mask, strlen(mask), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_CONFIG, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("MASK", l.item, l.item_len);
    TEST_ASSERT_EQUAL_STRING_LEN("MASK 5.0", l.text, l.text_len);
    um98x_config_item(&rb, l.item, l.item_len, l.text, l.text_len);
    TEST_ASSERT_TRUE(rb.rx.mask_known);
    TEST_ASSERT_EQUAL_INT16(50, rb.rx.mask_ddeg);
    TEST_ASSERT_EQUAL_UINT16(2, rb.items);
    cJSON_Delete(j);
}

TEST_CASE("fixtures: base-mode logs from a receiver: rover, survey-in and fixed base", "[fixtures]")
{
    cJSON *j = load_samples();
    um98x_line_t l;
    char body[UM98X_MODE_BODY_MAX];

    const char *rover = sample(j, "mode_query", 0);
    um98x_line_classify(rover, strlen(rover), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    TEST_ASSERT_EQUAL_STRING_LEN("MODE ROVER SURVEY,", l.text, l.text_len);
    snprintf(body, sizeof(body), "%.*s", (int)l.text_len, l.text);
    TEST_ASSERT_FALSE(um98x_mode_is_survey(body));
    TEST_ASSERT_FALSE(um98x_mode_is_fixed_base(body));

    /* Survey-in: the receiver drops the station ID and the distance limit
     * the command carried. */
    const char *survey = sample(j, "mode_query", 1);
    um98x_line_classify(survey, strlen(survey), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    snprintf(body, sizeof(body), "%.*s", (int)l.text_len, l.text);
    TEST_ASSERT_EQUAL_STRING("MODE BASE TIME 60,", body);
    TEST_ASSERT_TRUE(um98x_mode_is_survey(body));
    TEST_ASSERT_FALSE(um98x_mode_is_fixed_base(body));
    um98x_base_profile_t p;
    um98x_profile_default(&p, 1234);
    p.survey_in_s = 60;
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, body));
    p.survey_in_s = 600;
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));

    /* Fixed base: ECEF coordinates in metres with four decimals. */
    const char *fixed = sample(j, "mode_query", 2);
    um98x_line_classify(fixed, strlen(fixed), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_MODE, l.kind);
    snprintf(body, sizeof(body), "%.*s", (int)l.text_len, l.text);
    TEST_ASSERT_FALSE(um98x_mode_is_survey(body));
    TEST_ASSERT_TRUE(um98x_mode_is_fixed_base(body));
    const double xyz[3] = { -1280206.5680, -4716804.4030, 4086665.4840 };
    double lat;
    double lon;
    double h;
    TEST_ASSERT_TRUE(um98x_ecef_to_llh(xyz, &lat, &lon, &h));
    p.base_mode = UM98X_BASE_FIXED;
    p.fixed_lat_deg = lat;
    p.fixed_lon_deg = lon;
    p.fixed_height_m = h;
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));
    TEST_ASSERT_TRUE(um98x_mode_matches(&p, body));
    p.fixed_height_m = h + 0.01;
    TEST_ASSERT_FALSE(um98x_mode_matches(&p, body));
    cJSON_Delete(j);
}

TEST_CASE("fixtures: a version log whose PN-SN field is a lone dash", "[fixtures]")
{
    /* Published version logs show "-" where a unit has no PN-SN. Built here
     * in that layout with made-up values; CRC-32 computed with zlib
     * (crc32(body, 0xFFFFFFFF) ^ 0xFFFFFFFF = reflected, initial 0, no final
     * XOR), lower case. */
    static const char k_line[] = "#VERSIONA,80,GPS,FINE,2400,100000000,0,0,18,12;\"UM982\",\"R4.10Build18975\","
                                 "\"HRPT00-S10C-P\",\"-\",\"0123456789abcdef\",\"2025/04/18\"*03686e13";
    TEST_ASSERT_TRUE(gnss_text_line_valid(k_line, strlen(k_line)));
    um98x_line_t l;
    um98x_line_classify(k_line, strlen(k_line), &l);
    TEST_ASSERT_EQUAL(UM98X_LINE_VERSION, l.kind);
    TEST_ASSERT_EQUAL_STRING("UM982", l.version.model);
    TEST_ASSERT_EQUAL_UINT32(18975, l.version.build);
    TEST_ASSERT_EQUAL_STRING("", l.version.pn);
    TEST_ASSERT_EQUAL_STRING("", l.version.serial);
    TEST_ASSERT_EQUAL_STRING("0123456789abcdef", l.version.efuse_id);
    TEST_ASSERT_EQUAL_STRING("2025/04/18", l.version.compile_date);
}
