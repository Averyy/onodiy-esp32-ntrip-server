/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS byte-stream framing (pure logic, no RTOS, host-testable).
 *
 * The receiver's COM1 carries an interleaved stream of:
 *   - RTCM3 frames: 0xD3 preamble, 6 reserved bits + 10-bit payload length,
 *     payload (0..1023 bytes), CRC-24Q over header + payload  (public RTCM 10403.x)
 *   - text lines: NMEA sentences and receiver command replies/logs, each
 *     terminated by CR/LF
 *   - noise (partial frames after a baud change, line glitches)
 *
 * The framer splits the stream into whole RTCM3 frames (CRC verified) and whole
 * text lines, counts everything it throws away, and resynchronises on the next
 * plausible frame start. It never allocates and never blocks; the caller owns
 * the gnss_framer_t storage.
 *
 * Rules (so RTCM payload bytes are never mistaken for text and vice versa):
 *  - A frame candidate is 0xD3 followed by 6 reserved bits that are all zero
 *    and a length <= 1023. Anything else is not a frame start.
 *  - After a CRC failure the search restarts at the byte after the failed
 *    preamble (not after the whole candidate), so a real frame that started
 *    inside the bad one is still found.
 *  - Text is emitted only for lines that start with '$' and end in a valid
 *    NMEA "*hh" checksum, or start with '#' (receiver ASCII log) and end in
 *    that format's checksum per the public command manual. Lines that fail
 *    are dropped and counted in text_rejected; payload bytes that happen to
 *    contain '$' or CR/LF therefore never produce a fake reply line.
 *  - All counters are 64-bit: at ~50 lines/s a 32-bit counter would wrap in
 *    under three years of uptime.
 *
 * Status: API settled, parsing not implemented yet.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_RTCM3_PREAMBLE      0xD3u
#define GNSS_RTCM3_HEADER_LEN    3u     /**< preamble + 16 bits reserved/length */
#define GNSS_RTCM3_CRC_LEN       3u     /**< CRC-24Q */
#define GNSS_RTCM3_MAX_PAYLOAD   1023u
#define GNSS_RTCM3_MAX_FRAME     (GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_MAX_PAYLOAD + GNSS_RTCM3_CRC_LEN)
#define GNSS_TEXT_MAX_LINE       512u   /**< longer lines are dropped and counted */

/** Kind of unit the framer emits. */
typedef enum {
    GNSS_FRAME_RTCM3 = 0,   /**< complete RTCM3 frame incl. header and CRC */
    GNSS_FRAME_TEXT,        /**< one text line without the CR/LF terminator */
} gnss_frame_kind_t;

/** One emitted unit. `data` is only valid during the callback. */
typedef struct {
    gnss_frame_kind_t kind;
    const uint8_t *data;
    size_t len;
    uint16_t rtcm_msg_type;  /**< RTCM3 message number (first 12 payload bits); 0 for text */
} gnss_frame_t;

/** Called synchronously from gnss_framer_feed() for every complete unit. */
typedef void (*gnss_frame_cb_t)(const gnss_frame_t *frame, void *ctx);

/** Counters; monotonic since gnss_framer_init(), not cleared by reset. */
typedef struct {
    uint64_t bytes_in;          /**< every byte fed */
    uint64_t bytes_discarded;   /**< bytes that ended up in no frame or line */
    uint64_t rtcm_frames;       /**< RTCM3 frames emitted (CRC OK) */
    uint64_t rtcm_crc_errors;   /**< candidate frames rejected by CRC */
    uint64_t text_lines;        /**< text lines emitted (checksum OK) */
    uint64_t text_rejected;     /**< lines dropped: no valid prefix or checksum */
    uint64_t text_overflows;    /**< lines longer than GNSS_TEXT_MAX_LINE */
    uint64_t resyncs;           /**< times the framer lost and re-found sync */
} gnss_framing_stats_t;

/** Framer state. Treat as opaque; public only so callers can allocate it statically. */
typedef struct {
    gnss_frame_cb_t cb;
    void *ctx;
    uint8_t state;
    size_t fill;
    size_t expected;
    uint8_t buf[GNSS_RTCM3_MAX_FRAME > GNSS_TEXT_MAX_LINE ? GNSS_RTCM3_MAX_FRAME : GNSS_TEXT_MAX_LINE];
    gnss_framing_stats_t stats;
} gnss_framer_t;

/** Initialise a framer and zero its counters. `cb` must not be NULL. */
void gnss_framer_init(gnss_framer_t *f, gnss_frame_cb_t cb, void *ctx);

/**
 * Drop any partial frame/line (e.g. after a baud-rate change or when the link
 * changes owner). Counters are kept; dropped bytes count as discarded.
 */
void gnss_framer_reset(gnss_framer_t *f);

/** Feed received bytes; invokes the callback for each complete unit. */
void gnss_framer_feed(gnss_framer_t *f, const uint8_t *data, size_t len);

/** Copy the counters. */
void gnss_framer_get_stats(const gnss_framer_t *f, gnss_framing_stats_t *out);

#ifdef __cplusplus
}
#endif
