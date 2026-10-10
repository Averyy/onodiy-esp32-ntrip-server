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
 *  - A '$' line is emitted when its "*hh" checksum verifies under either
 *    rule: the NMEA rule (XOR of the bytes between '$' and '*', for NMEA
 *    sentences) or the '$'-inclusive rule (XOR including the '$', used by the
 *    receiver's command acknowledgements, configuration dumps and its restart
 *    banner).
 *  - A '#' line (receiver ASCII log) is emitted when it ends in either a
 *    2-hex XOR including the '#' (e.g. the base-mode log) or an 8-hex CRC32
 *    over the bytes after '#' up to '*' (e.g. the version log; the common
 *    reflected CRC-32 with polynomial 0xEDB88320, initial value 0 and no final
 *    XOR, lower-case hex).
 *  - Lines that fail are dropped and counted in text_rejected; payload bytes
 *    that happen to contain '$' or CR/LF therefore never produce a fake reply
 *    line. Lines are at most GNSS_TEXT_MAX_LINE bytes.
 *  - Loader prompt detector (armed only while the station looks for the
 *    receiver, under its command lease; during a firmware update session the
 *    framer is not fed and the flasher matches the loader's text itself):
 *    the receiver's loader prints the five bytes "boot>" with no terminator
 *    and no checksum, so the line rules never see it. While armed, the
 *    framer keeps a rolling window of the last GNSS_PROMPT_WINDOW received
 *    bytes, kept across feeds and framer resets (baud switches), searches it
 *    byte-exactly and NUL-safely for "boot>", and emits one
 *    GNSS_FRAME_PROMPT per hit. A hit is ignored when a CRC-valid RTCM frame
 *    was decoded within the same window (five bytes inside a long RTCM
 *    stream can match by chance). Disarming clears the window.
 *  - All counters are 64-bit: at ~50 lines/s a 32-bit counter would wrap in
 *    under three years of uptime.
 *
 * Version log parser (gnss_versiona_parse): the one parser for the receiver's
 * version log, shared by the command layer and the firmware flasher. The line
 * must start with "#VERSIONA," and pass its CRC32; header and body are split
 * at the first ';', the body is split on ',' and one pair of surrounding '"'
 * is stripped from each field; at least six fields are required: model,
 * firmware, authorisation, PN-SN (split at the first '-'), efuse/board ID,
 * compile date. The build is the integer after "Build" in the firmware field
 * (0 if absent); builds are compared as integers, never as substrings.
 *
 * Status: API settled, parsing not implemented yet. The checksum rules and
 * the version log layout are PROVISIONAL, to be confirmed on hardware
 * against captures from our receiver.
 */
#pragma once

#include <stdbool.h>
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
#define GNSS_PROMPT_WINDOW       512u   /**< bytes the loader prompt detector looks back */
#define GNSS_VERSIONA_MIN_FIELDS 6u     /**< body fields the version log must carry */

/** Kind of unit the framer emits. */
typedef enum {
    GNSS_FRAME_RTCM3 = 0,   /**< complete RTCM3 frame incl. header and CRC */
    GNSS_FRAME_TEXT,        /**< one text line without the CR/LF terminator */
    GNSS_FRAME_PROMPT,      /**< the loader prompt "boot>" was seen (detector armed only) */
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
    uint64_t prompts;           /**< loader prompts reported */
} gnss_framing_stats_t;

/** Framer state. Treat as opaque; public only so callers can allocate it statically. */
typedef struct {
    gnss_frame_cb_t cb;
    void *ctx;
    uint8_t state;
    size_t fill;
    size_t expected;
    uint8_t buf[GNSS_RTCM3_MAX_FRAME > GNSS_TEXT_MAX_LINE ? GNSS_RTCM3_MAX_FRAME : GNSS_TEXT_MAX_LINE];
    bool prompt_armed;
    size_t prompt_fill;
    size_t prompt_since_rtcm;   /**< bytes since the last CRC-valid RTCM frame ended */
    uint8_t prompt_win[GNSS_PROMPT_WINDOW];
    gnss_framing_stats_t stats;
} gnss_framer_t;

/** Body fields of a version log, as parsed (framing-local: callers copy into um98x_info_t). */
typedef struct {
    char model[16];
    char firmware[48];
    uint32_t build;          /**< integer after "Build", 0 if absent */
    char pn[24];
    char serial[32];
    char efuse_id[24];
    char compile_date[12];
} gnss_versiona_t;

/** Initialise a framer and zero its counters. `cb` must not be NULL. Prompt detector disarmed. */
void gnss_framer_init(gnss_framer_t *f, gnss_frame_cb_t cb, void *ctx);

/**
 * Drop any partial frame/line (e.g. after a baud-rate change or when the link
 * changes owner). Counters are kept; dropped bytes count as discarded. The
 * prompt detector's window is kept (a prompt may straddle a baud switch).
 */
void gnss_framer_reset(gnss_framer_t *f);

/** Feed received bytes; invokes the callback for each complete unit. */
void gnss_framer_feed(gnss_framer_t *f, const uint8_t *data, size_t len);

/** Copy the counters. */
void gnss_framer_get_stats(const gnss_framer_t *f, gnss_framing_stats_t *out);

/** Arm or disarm the loader prompt detector (disarming clears its window). */
void gnss_framer_set_prompt_detect(gnss_framer_t *f, bool armed);

/**
 * Parse one version log line (without CR/LF). Requires the "#VERSIONA,"
 * prefix, a valid CRC32 and at least GNSS_VERSIONA_MIN_FIELDS body fields;
 * see the rules above.
 * @return false (out cleared) if the line is not a valid version log.
 */
bool gnss_versiona_parse(const char *line, size_t len, gnss_versiona_t *out);

#ifdef __cplusplus
}
#endif
