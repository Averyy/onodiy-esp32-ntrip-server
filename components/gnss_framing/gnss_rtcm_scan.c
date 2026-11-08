/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM3-only scanner (gnss_rtcm_scan.h).
 */
#include "gnss_rtcm_scan.h"

#include <string.h>

#include "gnss_rtcm3.h"

#define RING_MASK (GNSS_RTCM_SCAN_RING - 1u)
_Static_assert((GNSS_RTCM_SCAN_RING & RING_MASK) == 0, "the ring is a power of two");
_Static_assert(GNSS_RTCM_SCAN_RING > GNSS_RTCM3_MAX_FRAME, "the ring holds a whole frame");
_Static_assert(GNSS_RTCM_SCAN_OPEN <= 255u, "n_open is 8 bits");

/* CRC-24Q generator, x^24 included. */
#define CRC24Q_POLY 0x1864CFBu

/* x^(8 * 2^k) modulo the generator, k = 0..10: a frame is at most 1029 < 2^11 bytes. */
static const uint32_t k_x8_pow2[11] = {
    0x000100u, 0x010000u, 0x668F48u, 0x36EB3Du, 0x6243DAu, 0xCB800Eu,
    0x7DB43Eu, 0xDEF23Cu, 0x3D145Au, 0xC5BF56u, 0x11E898u,
};

/* a * b modulo the generator (24-bit field elements). */
static uint32_t gf_mul(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int i = 23; i >= 0; i--) {
        r <<= 1;
        if (r & 0x1000000u) {
            r ^= CRC24Q_POLY;
        }
        if ((b >> i) & 1u) {
            r ^= a;
        }
    }
    return r & 0xFFFFFFu;
}

/* The CRC of the span of `len` bytes that took the running CRC from `before` to `after`. */
static uint32_t span_crc(uint32_t before, uint32_t after, uint16_t len)
{
    uint32_t shift = 1;   /* x^(8 * len) */
    for (unsigned k = 0; k < 11; k++) {
        if ((len >> k) & 1u) {
            shift = gf_mul(shift, k_x8_pow2[k]);
        }
    }
    return after ^ gf_mul(before, shift);
}

void gnss_rtcm_scan_init(gnss_rtcm_scanner_t *s, gnss_frame_cb_t cb, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
}

/* Offset of the first byte that may still be part of a frame. */
static uint64_t first_held(const gnss_rtcm_scanner_t *s)
{
    uint64_t pos = s->stats.bytes_in;
    uint64_t first = pos;
    for (unsigned i = 0; i < s->n_open; i++) {
        if (s->open[i].start < first) {
            first = s->open[i].start;
        }
    }
    /* The last one or two bytes may begin a header. */
    if (pos >= 1 && pos - 1 >= s->floor && s->ring[(pos - 1) & RING_MASK] == GNSS_RTCM3_PREAMBLE && pos - 1 < first) {
        first = pos - 1;
    }
    if (pos >= 2 && pos - 2 >= s->floor && s->ring[(pos - 2) & RING_MASK] == GNSS_RTCM3_PREAMBLE &&
        (s->ring[(pos - 1) & RING_MASK] >> 2) == 0 && pos - 2 < first) {
        first = pos - 2;
    }
    return first;
}

static void open_candidate(gnss_rtcm_scanner_t *s, uint64_t start, uint32_t crc_before, uint16_t len)
{
    s->stats.candidates++;
    unsigned slot = s->n_open;
    if (slot == GNSS_RTCM_SCAN_OPEN) {
        /* Full: the oldest makes room (a flood can hide a frame, never cost more). */
        slot = 0;
        for (unsigned i = 1; i < s->n_open; i++) {
            if (s->open[i].start < s->open[slot].start) {
                slot = i;
            }
        }
        s->stats.dropped++;
    } else {
        s->n_open++;
    }
    s->open[slot] = (gnss_rtcm_scan_cand_t){ .start = start, .crc_before = crc_before, .len = len };
}

static void emit(gnss_rtcm_scanner_t *s, uint64_t start, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        s->frame[i] = s->ring[(start + i) & RING_MASK];
    }
    uint64_t end = start + len;
    s->floor = end;
    s->emitted_bytes += len;
    s->stats.rtcm_frames++;
    /* Every candidate still open started before this end: it overlaps. */
    s->stats.overlaps += s->n_open;
    s->n_open = 0;
    const gnss_frame_t fr = {
        .kind = GNSS_FRAME_RTCM3,
        .data = s->frame,
        .len = len,
        .rtcm_msg_type = (uint16_t)(((unsigned)s->frame[3] << 4) | (s->frame[4] >> 4)),
        .start = start,
    };
    s->cb(&fr, s->ctx);
}

static void scan_byte(gnss_rtcm_scanner_t *s, uint8_t b)
{
    const uint64_t k = s->stats.bytes_in;   /* this byte's offset */
    s->ring[k & RING_MASK] = b;
    s->crc_hist[2] = s->crc_hist[1];
    s->crc_hist[1] = s->crc_hist[0];
    s->crc_hist[0] = s->crc;
    s->crc = gnss_rtcm3_crc24q_update(s->crc, &b, 1);
    s->stats.bytes_in = k + 1;
    const uint64_t pos = k + 1;

    /* A header complete with this byte: 0xD3 at k - 2. */
    if (k >= 2 && k - 2 >= s->floor && s->ring[(k - 2) & RING_MASK] == GNSS_RTCM3_PREAMBLE) {
        uint8_t h1 = s->ring[(k - 1) & RING_MASK];
        if ((h1 >> 2) != 0) {
            s->stats.reserved_nonzero++;
        } else {
            unsigned n = ((unsigned)(h1 & 0x03u) << 8) | b;
            if (n < 2) {
                s->stats.bad_length++;
            } else {
                open_candidate(s, k - 2, s->crc_hist[2],
                               (uint16_t)(n + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN));
            }
        }
    }

    /* Candidates whose last byte this is: of two valid ones, the one that
     * started first (as gnss_framer would keep it). Every open candidate
     * started at or after the floor (an emission or a reset clears them). */
    bool found = false;
    uint64_t best_start = 0;
    uint16_t best_len = 0;
    for (unsigned i = 0; i < s->n_open;) {
        gnss_rtcm_scan_cand_t c = s->open[i];
        if (c.start + c.len != pos) {
            i++;
            continue;
        }
        s->open[i] = s->open[--s->n_open];   /* closed either way; look at the one moved here */
        if (span_crc(c.crc_before, s->crc, c.len) != 0) {
            s->stats.crc_errors++;
            continue;
        }
        if (found) {
            s->stats.overlaps++;   /* two valid frames ending on this byte */
            if (c.start >= best_start) {
                continue;
            }
        }
        found = true;
        best_start = c.start;
        best_len = c.len;
    }
    if (found) {
        emit(s, best_start, best_len);
    }
}

void gnss_rtcm_scan_feed(gnss_rtcm_scanner_t *s, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        scan_byte(s, data[i]);
    }
}

void gnss_rtcm_scan_reset(gnss_rtcm_scanner_t *s)
{
    s->n_open = 0;
    s->floor = s->stats.bytes_in;
}

size_t gnss_rtcm_scan_held(const gnss_rtcm_scanner_t *s, uint64_t *first)
{
    uint64_t f = first_held(s);
    if (first != NULL) {
        *first = f;
    }
    return (size_t)(s->stats.bytes_in - f);
}

void gnss_rtcm_scan_get_stats(const gnss_rtcm_scanner_t *s, gnss_rtcm_scan_stats_t *out)
{
    *out = s->stats;
    out->bytes_discarded = first_held(s) - s->emitted_bytes;
}
