/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Placeholder: proves the host test wiring (firmware component builds for the
 * linux target and links into Unity). Real framing tests (valid frames, CRC
 * errors, resync, interleaved text) arrive with the framer implementation.
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
