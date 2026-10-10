/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receive side of the flasher (pure logic, host-testable).
 *
 * fl_win_t: a rolling window over everything the receiver sent, for text
 * matched NUL-safely across read boundaries. Each watched string remembers
 * where in the stream it was first seen (end offset), so "a prompt that
 * arrived after this write" is a comparison of stream offsets, and a match
 * is never lost when it slides out of the window.
 *
 * fl_rx_t: the byte classifier of the XMODEM transfer (handshake, block
 * responses, end of transfer). Byte classes: control (ACK 0x06, NAK 0x15,
 * CAN 0x18, 0x00), printable (0x20-0x7E, CR, LF, TAB), high (>= 0x80),
 * anything else is "other".
 *  - A printable byte other than a lone 'C' opens a text capture: reading
 *    goes on, nothing is sent and nothing is flushed, until a LF arrives or
 *    FL_TEXT_GAP_MS pass without a byte; the captured line is one event.
 *    A capture of only CR, LF, TAB and spaces carries no text and is
 *    reported as junk.
 *  - 'C' counts as a start or retransmit byte only if no other printable
 *    byte follows within FL_LONE_C_MS (a run of 'C's alone counts once);
 *    'C' followed by printable bytes starts a text line.
 *  - CAN followed by a second CAN within FL_CAN_PAIR_MS: the receiver
 *    cancelled. A lone CAN is ignored and the byte after it is processed
 *    normally (never swallowed).
 *  - Unclassified bytes are never discarded. The only drop is
 *    fl_rx_drop_stale(): single control bytes already received before a
 *    resend (a stale ACK of an earlier try must not count for the new one).
 *  - Baud-mismatch pattern: FL_MISMATCH_BYTES bytes from {0x00, 0x78, 0x80,
 *    0xF8} and/or UART framing errors since the last ACK or NAK: what a
 *    115200 talker looks like at 460800.
 * PROVISIONAL, to be confirmed on hardware: the time limits.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fl_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FL_ACK  0x06u
#define FL_NAK  0x15u
#define FL_CAN  0x18u
#define FL_STX  0x02u
#define FL_EOT  0x04u

#define FL_TEXT_GAP_MS     500u   /**< a text capture ends after this long without a byte */
#define FL_LONE_C_MS       100u   /**< 'C' is a start byte if no printable byte follows this soon */
#define FL_CAN_PAIR_MS     1000u  /**< two CANs this close: the receiver cancelled */
#define FL_MISMATCH_BYTES  32u
#define FL_LINE_MAX        256u   /**< longer captures are split into several lines */
#define FL_RXQ_CAP         2048u  /**< bytes read but not classified yet */
#define FL_WIN_CAP         1024u  /**< rolling text window */
#define FL_WATCH_MAX       40u
#define FL_WATCH_STR_MAX   48u    /**< longest watched string */

/* ---- rolling window with watched strings --------------------------------- */

typedef struct {
    const char *s;
    bool seen;
    uint64_t first_end;      /**< stream offset just after the first match */
    uint64_t last_end;       /**< ... and after the latest one */
} fl_watch_t;

typedef struct {
    uint8_t buf[FL_WIN_CAP];
    size_t len;
    uint64_t total;          /**< bytes ever appended (stream offset of the next byte) */
    fl_watch_t w[FL_WATCH_MAX];
    unsigned nw;
} fl_win_t;

void fl_win_init(fl_win_t *win);
/** Forget the text (not the stream offset) and every watch. */
void fl_win_clear(fl_win_t *win);
/** Watch a string (a literal: not copied). Returns its index, or -1 when full. */
int fl_win_watch(fl_win_t *win, const char *s);
void fl_win_append(fl_win_t *win, const uint8_t *data, size_t len);
/** First match of watch i ending after stream offset `after` (UINT64_MAX = none)... see fl_win_seen_after. */
bool fl_win_seen(const fl_win_t *win, int i);
/** True when watch i matched with a match ending after `after` (the latest match is compared). */
bool fl_win_seen_after(const fl_win_t *win, int i, uint64_t after);
/** NUL-safe search of the current window. */
bool fl_win_has(const fl_win_t *win, const char *s);

/* ---- XMODEM byte classifier ------------------------------------------------ */

typedef enum {
    FL_EV_TIMEOUT = 0,  /**< nothing classified before the deadline */
    FL_EV_ACK,
    FL_EV_NAK,
    FL_EV_C,            /**< lone 'C' (or a run of them) */
    FL_EV_CANCEL,       /**< CAN CAN within FL_CAN_PAIR_MS */
    FL_EV_CAN1,         /**< a lone CAN (ignored by every caller) */
    FL_EV_JUNK,         /**< 0x00, high or other byte, or a whitespace-only capture */
    FL_EV_TEXT,         /**< a captured line: fl_rx_t.line / line_len */
    FL_EV_MISMATCH,     /**< baud-mismatch pattern */
} fl_ev_t;

typedef void (*fl_tap_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    const fl_io_t *io;
    uint8_t q[FL_RXQ_CAP];
    size_t q_head;                   /**< first queued byte */
    size_t q_len;
    uint8_t line[FL_LINE_MAX + 1];   /**< NUL-terminated copy of the capture (may hold other NULs) */
    size_t line_len;
    uint32_t mismatch;               /**< mismatch-set bytes since the last ACK/NAK */
    uint32_t err_base;               /**< line_errors() at the last ACK/NAK */
    bool mismatch_reported;
    uint32_t junk;                   /**< ignored bytes (log only) */
    uint32_t lone_cans;
    uint32_t lines;
    fl_tap_t tap;                    /**< every byte read from the io also goes here (when set) */
    void *tap_ctx;
} fl_rx_t;

void fl_rx_init(fl_rx_t *rx, const fl_io_t *io);
/** Bytes that were already read elsewhere (after the ready line, in the same read): scanned, not dropped. */
void fl_rx_push(fl_rx_t *rx, const uint8_t *data, size_t len);
/** Next event, waiting until `deadline` (lookahead of a 'C', a CAN or a text line may run past it, bounded). */
fl_ev_t fl_rx_next(fl_rx_t *rx, uint32_t deadline);
/** Before a (re)send: drop leading single control bytes already received (ACK, NAK, lone 'C'). */
void fl_rx_drop_stale(fl_rx_t *rx);
/** Start counting the mismatch pattern afresh. */
void fl_rx_reset_mismatch(fl_rx_t *rx);
/** True if the captured line contains s (NUL-safe). */
bool fl_rx_line_has(const fl_rx_t *rx, const char *s);

#ifdef __cplusplus
}
#endif
