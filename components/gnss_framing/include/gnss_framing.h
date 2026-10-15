/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS byte-stream framing (pure logic, no RTOS, host-testable).
 *
 * The receiver's COM1 carries an interleaved stream of:
 *   - RTCM3 frames: 0xD3 preamble, 6 reserved bits + 10-bit payload length,
 *     payload (0..1023 bytes), CRC-24Q over header + payload  (public RTCM 10403.x)
 *   - text lines: NMEA sentences, receiver command replies and '#' ASCII
 *     logs, each terminated by CR/LF
 *   - receiver binary logs (0xAA 0x44 0xB5 sync), never enabled by us
 *   - noise (partial frames after a baud change, line glitches, NUL bytes)
 *
 * The framer splits the stream into whole RTCM3 frames (CRC verified) and whole
 * text lines (checksum verified), counts everything it throws away, and
 * resynchronises on the next plausible unit start. It never allocates, never
 * blocks and never logs; the caller owns the gnss_framer_t storage. Not
 * thread-safe: one task feeds it, and the callback must not call back into
 * the same framer except gnss_framer_get_stats().
 *
 * Rules (so RTCM payload bytes are never mistaken for text and vice versa):
 *  - A frame candidate is 0xD3 followed by 6 reserved bits that are all zero
 *    and a payload length of 2..1023 (a message number needs 2 bytes). A
 *    candidate is emitted only if its CRC-24Q verifies.
 *  - When a candidate is rejected (reserved bits, length or CRC) the bytes it
 *    held are scanned again from the byte after its preamble, through the
 *    full rules (frames, lines, binary logs), so a real frame or line that
 *    started inside the bad candidate (e.g. after a truncated frame) is still
 *    found. The bytes in front of the next unit count as discarded.
 *  - A text line starts at '$' or '#' and holds printable ASCII and TAB. It
 *    ends at CR or LF (a LF right after the CR that ended a line belongs to
 *    that line). Any other byte (a control byte, NUL, >= 0x80, e.g. an RTCM
 *    preamble) aborts the line (text_aborts) and is examined again as a
 *    possible unit start. Lines longer than GNSS_TEXT_MAX_LINE bytes are
 *    counted (text_overflows) and skipped up to their terminator, with the
 *    same abort rule, so a long log never costs a following RTCM frame.
 *  - A '$' line is emitted when its "*hh" checksum verifies under either
 *    rule: the NMEA rule (XOR of the bytes between '$' and '*', for NMEA
 *    sentences) or the '$'-inclusive rule (XOR including the '$', used by the
 *    receiver's command acknowledgements, configuration dumps and its restart
 *    banner).
 *  - A '#' line (receiver ASCII log) is emitted when it ends in either a
 *    2-hex XOR including the '#' (e.g. the base-mode log) or an 8-hex CRC32
 *    over the bytes after '#' up to '*' (e.g. the version log; the common
 *    reflected CRC-32 with polynomial 0xEDB88320, initial value 0 and no final
 *    XOR). Hex digits are accepted in either case.
 *  - One line has no lead character: while the receiver restarts it answers
 *    every command with "The Board is resetting,do not response any
 *    command!*39" (seen on UM982 build 25164), whose "*hh" is the XOR of
 *    the text before '*'. A unit that starts with the exact bytes of
 *    GNSS_NOTICE_PREFIX is collected as a line and emitted when that
 *    checksum verifies (data[0] is then 'T', not '$' or '#'; the line
 *    counts in text_lines). Only this prefix starts such a line: a 'T'
 *    that does not continue it is discarded as before and the bytes after
 *    it are scanned again, so the rule costs no following unit.
 *  - Lines that fail are dropped and counted in text_rejected; payload bytes
 *    that happen to contain '$' or CR/LF therefore never produce a fake reply
 *    line. A line that fails but holds another '$' or '#' after its first
 *    byte is scanned again from the byte after that first byte, terminator
 *    included, like a rejected RTCM candidate: noise that ends in a lead
 *    byte (seen in a real capture: the last byte of a binary message is a
 *    '#' right in front of a sentence) never costs the line behind it. A
 *    lead byte right after the lead byte restarts the line there (no line
 *    starts with two; "$$..." would otherwise pass the '$'-inclusive rule),
 *    counted in text_aborts. Emitted lines exclude the terminator and are
 *    NUL-terminated (data[len] == 0) for the callback's convenience.
 *  - Binary logs: after 0xAA 0x44 0xB5 the 24-byte header is collected; its
 *    16-bit message length at offset 6 (little-endian, message body only;
 *    so laid out in a real capture of 39 logs from a sibling receiver,
 *    PROVISIONAL for ours, to be confirmed on hardware) gives the bytes to
 *    skip (body + 4-byte CRC), counted in bin_skipped and as discarded bytes.
 *    The CRC is not checked (the bytes are not kept). A length above
 *    GNSS_BIN_MAX_MSG is a false sync and is rescanned like a rejected RTCM
 *    candidate.
 *  - Loader prompt detector (armed only while the station looks for the
 *    receiver, under its command lease; during a firmware update session the
 *    framer is not fed and the flasher matches the loader's text itself):
 *    the receiver's loader prints the five bytes "boot>" with no terminator
 *    and no checksum, so the line rules never see it. While armed, the
 *    framer matches every received byte against "boot>" byte-exactly and
 *    NUL-safely, carrying a partial match across feeds and framer resets
 *    (baud switches), and reports a hit as GNSS_FRAME_PROMPT. A hit is
 *    ignored (prompts_ignored) when a CRC-valid RTCM frame lies within
 *    GNSS_PROMPT_WINDOW bytes of it (five bytes inside a long RTCM stream
 *    can match by chance): frames before it always count, frames after it
 *    count until the hit is decided. The decision is made at the end of the
 *    feed, unless an RTCM candidate that started before the hit (and may
 *    contain it) is still open: then it waits for that candidate, or for
 *    gnss_framer_reset(). At most one GNSS_FRAME_PROMPT per feed. Disarming
 *    forgets any partial match or pending hit.
 *  - Counters are 64-bit: at ~50 lines/s a 32-bit counter would wrap in
 *    under three years of uptime. Every fed byte ends up in exactly one
 *    emitted unit (a line's CR/LF belong to it), in bytes_discarded, or in
 *    the unit still being collected (discarded if a reset drops it).
 *
 * Cost: a rejected candidate is rescanned once; a stream built to chain
 * false candidates can make the framer rescan each byte many times (CRC
 * work, no allocation). Ordinary noise produces a false candidate roughly
 * every 16 KB. Units swallowed by a false candidate come out when it fails
 * its CRC, i.e. after at most GNSS_RTCM3_MAX_FRAME bytes of further input.
 * A rejected line is rescanned only when it holds another lead byte; a line
 * built to nest failing lines costs at most about GNSS_TEXT_MAX_LINE^2 / 4
 * steps.
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
 * Status: implemented and host-tested. The checksum rules, the version log
 * layout and the binary log length are PROVISIONAL, to be confirmed on
 * hardware against captures from our receiver.
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
#define GNSS_PROMPT_WINDOW       512u   /**< bytes around a loader prompt that must hold no valid RTCM frame */
#define GNSS_VERSIONA_MIN_FIELDS 6u     /**< body fields the version log must carry */
#define GNSS_BIN_HEADER_LEN      24u    /**< receiver binary log header */
#define GNSS_BIN_MAX_MSG         4096u  /**< larger binary message lengths are false syncs */
/** Start of the receiver's "restarting" answer, the one text line without a lead character. */
#define GNSS_NOTICE_PREFIX       "The Board is resetting"

/** Kind of unit the framer emits. */
typedef enum {
    GNSS_FRAME_RTCM3 = 0,   /**< complete RTCM3 frame incl. header and CRC */
    GNSS_FRAME_TEXT,        /**< one text line without the CR/LF terminator */
    GNSS_FRAME_PROMPT,      /**< the loader prompt "boot>" was seen (detector armed only) */
} gnss_frame_kind_t;

/** One emitted unit. `data` is only valid during the callback. */
typedef struct {
    gnss_frame_kind_t kind;
    const uint8_t *data;     /**< frame, line (NUL-terminated at data[len]) or the prompt text */
    size_t len;
    uint16_t rtcm_msg_type;  /**< RTCM3 message number (first 12 payload bits); 0 for text and prompt */
    uint64_t at;             /**< prompt: stream offset of the match's last byte (bytes fed before it since init); 0 otherwise */
} gnss_frame_t;

/** Called synchronously from gnss_framer_feed() (and gnss_framer_reset() for a pending prompt). */
typedef void (*gnss_frame_cb_t)(const gnss_frame_t *frame, void *ctx);

/** Counters; monotonic since gnss_framer_init(), not cleared by reset. */
typedef struct {
    uint64_t bytes_in;          /**< every byte fed */
    uint64_t bytes_discarded;   /**< bytes that ended up in no frame or line (incl. skipped binary logs) */
    uint64_t rtcm_frames;       /**< RTCM3 frames emitted (CRC OK) */
    uint64_t rtcm_crc_errors;   /**< candidate frames rejected by CRC */
    uint64_t rtcm_reserved_nonzero; /**< candidates rejected because the 6 reserved bits were not 0 */
    uint64_t rtcm_bad_length;   /**< candidates rejected because the payload was shorter than 2 bytes */
    uint64_t text_lines;        /**< text lines emitted (checksum OK) */
    uint64_t text_rejected;     /**< complete lines dropped: no valid checksum suffix or a wrong checksum */
    uint64_t text_aborts;       /**< partial lines dropped at a non-text byte */
    uint64_t text_overflows;    /**< lines longer than GNSS_TEXT_MAX_LINE */
    uint64_t bin_skipped;       /**< receiver binary logs skipped */
    uint64_t resyncs;           /**< valid frames or lines that followed discarded bytes (sync lost, then found) */
    uint64_t prompts;           /**< loader prompts reported */
    uint64_t prompts_ignored;   /**< loader prompt matches ignored because of RTCM within the window */
} gnss_framing_stats_t;

/** Framer state. Treat as opaque; public only so callers can allocate it statically. */
typedef struct {
    gnss_frame_cb_t cb;
    void *ctx;
    uint8_t state;
    uint8_t after_cr;           /**< the previous line ended at a CR (and whether it was emitted) */
    bool sync_lost;             /**< bytes were discarded since the last emitted unit */
    size_t fill;                /**< bytes of the current unit held in buf */
    size_t need;                /**< RTCM: whole frame length once known; binary: bytes left to skip */
    size_t rescan_pos;          /**< bytes of a rejected candidate still to be scanned again: */
    size_t rescan_end;          /**<   buf[rescan_pos .. rescan_end), always above buf[0 .. fill) */
    uint8_t buf[GNSS_RTCM3_MAX_FRAME > GNSS_TEXT_MAX_LINE + 1 ? GNSS_RTCM3_MAX_FRAME : GNSS_TEXT_MAX_LINE + 1];
    bool prompt_armed;
    bool prompt_pending;        /**< a match waits for its decision */
    bool rtcm_seen;             /**< a CRC-valid frame was emitted since init */
    uint8_t prompt_match;       /**< prompt bytes matched so far */
    uint64_t prompt_at;         /**< stream offset of the pending match's last byte */
    uint64_t rtcm_end_at;       /**< stream offset of the last byte of the last CRC-valid frame */
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
 * prompt detector's partial match is kept (a prompt may straddle a baud
 * switch); a pending prompt hit is decided now (the candidate that could
 * have held it is gone), so the callback may run for GNSS_FRAME_PROMPT.
 */
void gnss_framer_reset(gnss_framer_t *f);

/** Feed received bytes; invokes the callback for each complete unit. */
void gnss_framer_feed(gnss_framer_t *f, const uint8_t *data, size_t len);

/** Copy the counters. */
void gnss_framer_get_stats(const gnss_framer_t *f, gnss_framing_stats_t *out);

/** Arm or disarm the loader prompt detector (disarming forgets partial and pending matches). */
void gnss_framer_set_prompt_detect(gnss_framer_t *f, bool armed);

/**
 * Checksum rule of one text line (without CR/LF), as the framer applies it:
 * '$' lines by the NMEA or the '$'-inclusive XOR, '#' lines by the
 * '#'-inclusive XOR or the CRC32. For text cut from raw bytes elsewhere
 * (e.g. by the flasher).
 */
bool gnss_text_line_valid(const char *line, size_t len);

/**
 * Parse one version log line (without CR/LF; trailing CR/LF are ignored).
 * Requires the "#VERSIONA," prefix, a valid CRC32 and at least
 * GNSS_VERSIONA_MIN_FIELDS body fields; see the rules above. A model or
 * firmware field that is empty or does not fit its buffer fails the parse;
 * the other fields are cut to fit. A PN-SN field without '-' is taken as the
 * serial (PN empty).
 * @return false (out cleared) if the line is not a valid version log.
 */
bool gnss_versiona_parse(const char *line, size_t len, gnss_versiona_t *out);

#ifdef __cplusplus
}
#endif
