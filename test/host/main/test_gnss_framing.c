/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Placeholder: proves the host test wiring (firmware component builds for the
 * linux target and links into Unity). Real framing tests (valid frames, CRC
 * errors, resync, interleaved text, both '$' and both '#' checksum rules, the
 * loader prompt across a read boundary and after a NUL, captured version
 * logs and a corrupted copy of each) arrive with the framer implementation.
 */
#include <string.h>

#include "gnss_framing.h"
#include "unity.h"

static void count_frames(const gnss_frame_t *frame, void *ctx)
{
    (void)frame;
    (*(int *)ctx)++;
}

TEST_CASE("framer init zeroes counters and reset keeps them", "[gnss_framing]")
{
    gnss_framer_t framer;
    int frames = 0;
    gnss_framing_stats_t stats;

    memset(&framer, 0xA5, sizeof(framer));
    gnss_framer_init(&framer, count_frames, &frames);
    gnss_framer_get_stats(&framer, &stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(0, stats.rtcm_frames);
    TEST_ASSERT_EQUAL_UINT64(0, stats.text_lines);

    static const uint8_t junk[] = { 0x00, 0x01, 0x02, 0x03 };
    gnss_framer_feed(&framer, junk, sizeof(junk));
    gnss_framer_reset(&framer);
    gnss_framer_get_stats(&framer, &stats);
    TEST_ASSERT_EQUAL_UINT64(sizeof(junk), stats.bytes_in);
    TEST_ASSERT_EQUAL_INT(0, frames);
}

TEST_CASE("maximum RTCM3 frame size follows the standard", "[gnss_framing]")
{
    TEST_ASSERT_EQUAL_UINT32(1029, GNSS_RTCM3_MAX_FRAME);
}

TEST_CASE("framer prompt detector arms and disarms", "[gnss_framing]")
{
    gnss_framer_t framer;
    int frames = 0;
    gnss_framer_init(&framer, count_frames, &frames);
    TEST_ASSERT_FALSE(framer.prompt_armed);
    gnss_framer_set_prompt_detect(&framer, true);
    TEST_ASSERT_TRUE(framer.prompt_armed);
    gnss_framer_reset(&framer);                          /* a baud switch keeps it armed */
    TEST_ASSERT_TRUE(framer.prompt_armed);
    gnss_framer_set_prompt_detect(&framer, false);
    TEST_ASSERT_FALSE(framer.prompt_armed);
    TEST_ASSERT_EQUAL_size_t(0, framer.prompt_fill);
}

TEST_CASE("version log parser rejects lines that are not a valid version log", "[gnss_framing]")
{
    static const char *const lines[] = {
        "",
        "$GPGGA,000000.00,,,,,0,00,99.99,,,,,,*48",
        "#MODEA,COM1,0,0,UNKNOWN,0,0,0,0,0;MODE BASE TIME 600 0*00",
        "#VERSIONA,COM1;\"UM982\"*00000000",                 /* bad CRC, too few fields */
    };
    gnss_versiona_t v;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        memset(&v, 0x5A, sizeof(v));
        TEST_ASSERT_FALSE(gnss_versiona_parse(lines[i], strlen(lines[i]), &v));
        TEST_ASSERT_EQUAL_STRING("", v.model);
        TEST_ASSERT_EQUAL_UINT32(0, v.build);
    }
}
