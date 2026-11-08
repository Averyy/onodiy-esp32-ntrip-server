/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The unchanged framing code on a host compiler, outside ESP-IDF
 * (docs/component-reuse.md): a recorded RTCM3 file fed to gnss_framer in
 * uneven pieces, every emitted frame's CRC-24Q checked again with
 * gnss_rtcm3_crc24q, and the number of frames compared with the expected
 * one. tools/ci/external_host_framing.sh builds and runs it:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I<repo>/components/gnss_framing/include \
 *      framing_check.c <repo>/components/gnss_framing/gnss_framing.c \
 *      <repo>/components/gnss_framing/gnss_rtcm3.c -o framing_check
 *   ./framing_check <file.rtcm3> <expected frames>
 *
 * Exit status 0 when every frame checks and the count matches, 1 otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gnss_framing.h"
#include "gnss_rtcm3.h"

typedef struct {
    unsigned long frames;
    unsigned long bad_crc;
} tally_t;

static void on_unit(const gnss_frame_t *fr, void *ctx)
{
    tally_t *t = ctx;
    if (fr->kind != GNSS_FRAME_RTCM3) {
        return;
    }
    t->frames++;
    /* Header and payload, then the 24-bit CRC, most significant byte first. */
    size_t body = fr->len - 3;
    uint32_t sent = ((uint32_t)fr->data[body] << 16) | ((uint32_t)fr->data[body + 1] << 8) | fr->data[body + 2];
    if (gnss_rtcm3_crc24q(fr->data, body) != sent) {
        t->bad_crc++;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <file.rtcm3> <expected frames>\n", argv[0]);
        return 1;
    }
    unsigned long expected = strtoul(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    static gnss_framer_t framer;
    tally_t t = { 0 };
    gnss_framer_init(&framer, on_unit, &t);
    uint8_t buf[97];   /* pieces of 1 to 97 bytes: frames straddle every feed boundary */
    size_t want = 1;
    size_t n;
    while ((n = fread(buf, 1, want, f)) > 0) {
        gnss_framer_feed(&framer, buf, n);
        want = want % sizeof(buf) + 1;
    }
    fclose(f);
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&framer, &st);
    printf("%s: %lu frames (expected %lu), %lu failed the CRC check again, %llu bytes discarded\n", argv[1],
           t.frames, expected, t.bad_crc, (unsigned long long)st.bytes_discarded);
    return (t.frames == expected && t.bad_crc == 0 && st.rtcm_frames == expected) ? 0 : 1;
}
