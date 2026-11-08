/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM3-only scanner for untrusted input (pure logic, no IDF or RTOS
 * includes, host-testable).
 *
 * Finds CRC-valid RTCM3 frames in a byte stream and nothing else: text
 * lines, binary logs, the receiver's restarting answer and the loader
 * prompt are not looked for, and every byte outside an emitted frame is
 * discarded. Meant for input where only corrections matter and anyone may
 * write (a phone app); the receiver link keeps gnss_framer.
 *
 * Frames: the rule of gnss_framer, a 0xD3 preamble, six zero reserved bits,
 * a payload length of 2..1023 and the CRC-24Q over the whole frame.
 *
 * Bounded cost: nothing is ever scanned twice. The scanner keeps the running
 * CRC-24Q of the whole stream. A candidate (any byte that starts a valid
 * header) is checked once, when its last byte arrives, from the running CRC
 * at its first byte and at its last: CRC-24Q is linear, so the CRC of a span
 * is CRC(end) XOR CRC(start) * x^(8 * length) modulo its generator, at most
 * 11 multiplications in that field of 24 steps each. Per input byte: one
 * table CRC step, a header test and a pass over the open candidates (at
 * most GNSS_RTCM_SCAN_OPEN). No input, crafted or not, costs more.
 *
 * Open candidates may overlap. A frame is emitted when its last byte
 * arrives, unless it overlaps a frame already emitted; emitting a frame
 * drops every open candidate that started before its end (counted in
 * overlaps). So where two CRC-valid frames overlap, which only crafted
 * input does, the scanner keeps the one that ends first, while gnss_framer
 * keeps the one that starts first; on real streams both give the same
 * frames (host-tested on every public capture, at several feed sizes). A
 * frame behind a truncated one comes out as soon as it is complete, without
 * waiting for the truncated candidate to fail. At most GNSS_RTCM_SCAN_OPEN
 * candidates are open: a new one beyond that replaces the oldest (counted in
 * dropped), so a flood of false candidates can hide a real frame but never
 * makes the scanner slower.
 *
 * Held input (gnss_rtcm_scan_held): from the first byte that may still be
 * part of a frame (the oldest open candidate's first byte, or the last one
 * or two bytes when they could start a header) to the newest. Every byte
 * before it is in an emitted frame or counted in bytes_discarded, so
 * bytes_in == bytes_discarded + the bytes of the frames emitted + held at
 * any time. Each frame reaches the callback with the stream offset of its
 * first byte (gnss_frame_t.start), its message number and kind
 * GNSS_FRAME_RTCM3.
 *
 * Not thread-safe: one task feeds it; from the callback only
 * gnss_rtcm_scan_held() and gnss_rtcm_scan_get_stats() may be called. The
 * caller owns the storage (3.7 KB: a 2 KB ring of the newest bytes, a
 * frame's copy for the callback and the open candidates).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_RTCM_SCAN_OPEN 32u     /**< open candidates at most */
#define GNSS_RTCM_SCAN_RING 2048u   /**< bytes kept (a power of two above GNSS_RTCM3_MAX_FRAME) */

/** Counters; monotonic since gnss_rtcm_scan_init(). */
typedef struct {
    uint64_t bytes_in;          /**< every byte fed */
    uint64_t bytes_discarded;   /**< bytes before the held input that are in no emitted frame */
    uint64_t rtcm_frames;       /**< frames emitted */
    uint64_t candidates;        /**< headers that opened a candidate */
    uint64_t crc_errors;        /**< candidates whose CRC failed */
    uint64_t reserved_nonzero;  /**< 0xD3 followed by nonzero reserved bits */
    uint64_t bad_length;        /**< 0xD3 with a payload length below 2 */
    uint64_t overlaps;          /**< open candidates dropped because a frame emitted over them */
    uint64_t dropped;           /**< open candidates replaced because GNSS_RTCM_SCAN_OPEN were open */
} gnss_rtcm_scan_stats_t;

/** One open candidate. */
typedef struct {
    uint64_t start;             /**< stream offset of its 0xD3 */
    uint32_t crc_before;        /**< the running CRC before its first byte */
    uint16_t len;               /**< whole frame, header and CRC included */
} gnss_rtcm_scan_cand_t;

/** Scanner state. Treat as opaque; public only so callers can allocate it statically. */
typedef struct {
    gnss_frame_cb_t cb;
    void *ctx;
    uint32_t crc;               /**< CRC-24Q of every byte fed */
    uint32_t crc_hist[3];       /**< the running CRC before each of the last three bytes, newest first */
    uint64_t floor;             /**< no frame starts before this offset (an emitted frame's end, a reset) */
    uint64_t emitted_bytes;
    uint8_t n_open;
    gnss_rtcm_scan_cand_t open[GNSS_RTCM_SCAN_OPEN];
    gnss_rtcm_scan_stats_t stats;
    uint8_t ring[GNSS_RTCM_SCAN_RING];
    uint8_t frame[GNSS_RTCM3_MAX_FRAME];   /**< the frame handed to the callback, contiguous */
} gnss_rtcm_scanner_t;

/** Initialise a scanner and zero its counters. `cb` must not be NULL. */
void gnss_rtcm_scan_init(gnss_rtcm_scanner_t *s, gnss_frame_cb_t cb, void *ctx);

/** Feed bytes; the callback runs for each frame as its last byte arrives. */
void gnss_rtcm_scan_feed(gnss_rtcm_scanner_t *s, const uint8_t *data, size_t len);

/**
 * Drop the held input (counted as discarded): no frame can start before the
 * next byte. For a gap or discontinuity in the input.
 */
void gnss_rtcm_scan_reset(gnss_rtcm_scanner_t *s);

/** Bytes held (see above); *first (may be NULL) gets the offset of the oldest (bytes_in when none). */
size_t gnss_rtcm_scan_held(const gnss_rtcm_scanner_t *s, uint64_t *first);

/** Copy the counters (bytes_discarded as of now). */
void gnss_rtcm_scan_get_stats(const gnss_rtcm_scanner_t *s, gnss_rtcm_scan_stats_t *out);

#ifdef __cplusplus
}
#endif
