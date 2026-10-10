/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * XMODEM-1K sender (pure logic, host-tested against a simulated receiver;
 * written from the public XMODEM-1K protocol).
 *
 * Framing: STX, block number (starts at 1, 8-bit, wraps 255 -> 0), its
 * complement, 1024 data bytes, then CRC-16/XMODEM high byte first (CRC mode)
 * or the 8-bit sum (checksum mode). 128-byte frames are never used. The last
 * short frame is filled with one 0x1A, then 0x00. A length that is an exact
 * multiple of 1024 is refused (which extra frame the receiver tolerates is
 * not known), as is an empty one.
 *
 * Handshake: one window (XM_HANDSHAKE_MS) from the end of the receiver's
 * ready line for the first valid start byte. NAK locks checksum mode, a lone
 * 'C' locks CRC mode; further start bytes are ignored until frame 1 is out,
 * which goes at once. Loader text (abort, prompt, menu errors, the firmware
 * submenu) or CAN CAN fails the handshake; other text is logged and the wait
 * goes on; nothing in the window: CAN CAN and fail. No frame was sent: a
 * handshake failure is before the point of no return.
 *
 * Point of no return: the first data frame. job->before_first_frame (the
 * journal's "transferring" record) must succeed first, else nothing is sent.
 *
 * Each block (XM_ACK_WAIT_MS per wait, XM_FRAME_MAX_MS per block in total):
 *   ACK           next block (progress)
 *   NAK, lone C   resend (+1 for the block and the transfer)
 *   wait over     resend (+1 / +1)
 *   junk, lone CAN  ignored (the per-block cap still runs)
 *   CAN CAN       the receiver cancelled: one ACK, stop
 *   text with "xmodem error" / "uart getc time"  the receiver reports an
 *                 error: send nothing, keep waiting
 *   any other text  stop at once and never resend (the receiver may be back
 *                 at its menu, where frame bytes would be menu input)
 *   mismatch pattern  the receiver left the loader: stop
 * Limits: XM_BLOCK_TRIES failed tries on one block, XM_TOTAL_TRIES in the
 * transfer, or the per-block time cap: CAN CAN, stop. Before every frame the
 * single control bytes already received are dropped, those still in the
 * UART driver included (read in without waiting first): a stale ACK of an
 * earlier try never counts for the new one, and before frame 1 every
 * further start byte is stale, so a repeated 'C' that arrived while the
 * journal was written is never taken for a retransmit request. After the
 * ACK of a block that went out more than once, the answers to its other
 * copies are dropped for XM_DUP_SETTLE_MS before the next block (a receiver
 * acknowledges a duplicate block again; that ACK would otherwise count for
 * the next block and shift every answer after it by one).
 *
 * End: EOT, wait XM_EOT_WAIT_MS. ACK: done. NAK: EOT again (XM_EOT_MAX in
 * total; after the last NAK the post-transfer wait starts anyway). Silence,
 * text or any other byte: stop sending and start the post-transfer wait.
 * Every byte read from the first EOT on goes to job->post_tap (the start of
 * the post-transfer buffer). After the last block's ACK nothing but EOT is
 * ever sent: no CAN, no menu key, no reset.
 *
 * PROVISIONAL, to be confirmed on hardware: every time and retry limit.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fl_io.h"
#include "fl_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XM_BLOCK          1024u
#define XM_FRAME_MAX      (3u + XM_BLOCK + 2u)
#define XM_HANDSHAKE_MS   15000u
#define XM_ACK_WAIT_MS    30000u
#define XM_FRAME_MAX_MS   60000u
#define XM_BLOCK_TRIES    10u
#define XM_TOTAL_TRIES    40u
#define XM_EOT_WAIT_MS    5000u
#define XM_EOT_MAX        10u
#define XM_PAD_FIRST      0x1Au
/** After the ACK of a block sent more than once: how long the answers to its other copies are dropped (one
 *  frame at 115200 is 90 ms; PROVISIONAL, to be confirmed on hardware with a duplicate block). */
#define XM_DUP_SETTLE_MS  250u

typedef enum {
    XM_MODE_NONE = 0,
    XM_MODE_CRC,
    XM_MODE_SUM,
} xm_mode_t;

typedef enum {
    XM_OK = 0,              /**< every block acknowledged, EOT sent: the post-transfer wait follows */
    XM_REFUSED,             /**< nothing sent: empty, exact multiple of 1024, or larger than 255 * 1024 * 2^24 */
    XM_HANDSHAKE_FAILED,    /**< no frame sent (before the point of no return) */
    XM_NOT_STARTED,         /**< before_first_frame refused: no frame sent */
    XM_CANCELLED,           /**< the receiver sent CAN CAN */
    XM_TEXT_ABORT,          /**< receiver text during the transfer */
    XM_RETRY_LIMIT,         /**< per-block or transfer-wide limit (CAN CAN sent) */
    XM_FRAME_TIMEOUT,       /**< per-block time cap (CAN CAN sent) */
    XM_BAUD_MISMATCH,       /**< the receiver left the loader */
    XM_WRITE_FAILED,        /**< our write failed */
    XM_STOPPED,             /**< bench: stopped on purpose after job->stop_after acknowledged blocks (nothing more sent) */
} xm_result_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
    void *ctx;
    /** Before the first frame (persist "transferring"); false: nothing is sent. */
    bool (*before_first_frame)(void *ctx, xm_mode_t mode);
    /** After each acknowledged block. */
    void (*on_acked)(void *ctx, uint32_t blocks_acked, uint32_t bytes_acked, uint32_t retries);
    /** Before the first EOT (persist "eot-sent"; a failure does not stop anything). */
    void (*before_eot)(void *ctx);
    /** Every byte read from the first EOT on (the start of the post-transfer buffer). */
    fl_tap_t post_tap;
    void *post_ctx;
    /** Bench only: stop sending (no CAN, nothing) after this many acknowledged blocks; 0 = never. */
    uint32_t stop_after;
} xm_job_t;

typedef struct {
    xm_mode_t mode;
    bool first_frame_sent;   /**< the point of no return was crossed */
    uint32_t blocks;         /**< frames of the file */
    uint32_t blocks_acked;
    uint32_t frames_sent;    /**< every frame written, resends included */
    uint32_t retries;        /**< failed tries in the whole transfer */
    uint32_t start_junk;     /**< bytes ignored in the handshake */
    uint8_t eots;
    bool eot_acked;
    uint32_t last_eot_ms;    /**< when the last EOT was written (T1/T2 start) */
    uint32_t handshake_ms;   /**< from the ready line to the start byte */
    uint32_t late_answers;   /**< answers to earlier copies of a block, dropped after its ACK */
    char text[FL_LINE_MAX + 1];   /**< the line that decided a failure ("" none) */
    uint8_t frame[XM_FRAME_MAX];  /**< work buffer */
} xm_state_t;

/** Number of frames for len bytes (the last one padded). */
uint32_t xm_blocks(uint32_t len);

/** Build frame `index` (0-based; block number index + 1 mod 256). Returns its length on the wire. */
size_t xm_build_frame(const uint8_t *data, uint32_t len, uint32_t index, xm_mode_t mode, uint8_t *out);

/** Send the file. rx holds any bytes that came after the ready line in the same read. */
xm_result_t xm_send(const fl_io_t *io, fl_rx_t *rx, const xm_job_t *job, xm_state_t *st);

/** Short name for logs. */
const char *xm_result_name(xm_result_t r);

#ifdef __cplusplus
}
#endif
