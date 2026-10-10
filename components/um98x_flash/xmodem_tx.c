/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "xmodem_tx.h"

#include <string.h>

#include "fl_crc.h"

/* Loader text that means it is not (or no longer) receiving. */
static const char *const k_not_receiving[] = {
    "## Binary download aborted", "boot>", "Invalid input", "Get input timeout", "input is too long",
    "Choose firmware for update",
};

/* Receiver-side error reports during a transfer: wait for its NAK/ACK/CAN. */
static const char *const k_rx_error[] = { "xmodem error", "uart getc time" };

static bool line_has_any(const fl_rx_t *rx, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (fl_rx_line_has(rx, list[i])) {
            return true;
        }
    }
    return false;
}

static void keep_text(xm_state_t *st, const fl_rx_t *rx)
{
    size_t n = rx->line_len < sizeof(st->text) - 1 ? rx->line_len : sizeof(st->text) - 1;
    memcpy(st->text, rx->line, n);
    st->text[n] = '\0';
}

static void note_line(const fl_io_t *io, const char *what, const fl_rx_t *rx)
{
    char buf[100];
    size_t j = 0;
    for (size_t i = 0; i < rx->line_len && j < sizeof(buf) - 1; i++) {
        uint8_t b = rx->line[i];
        if (b == '\r' || b == '\n') {
            continue;
        }
        buf[j++] = (b >= 0x20 && b <= 0x7E) ? (char)b : '?';
    }
    buf[j] = '\0';
    fl_notef(io, "%s: \"%s\"", what, buf);
}

static void send_cancel(const fl_io_t *io)
{
    static const uint8_t can2[2] = { FL_CAN, FL_CAN };
    (void)fl_write(io, can2, sizeof(can2));
}

static void send_ack(const fl_io_t *io)
{
    static const uint8_t ack = FL_ACK;
    (void)fl_write(io, &ack, 1);
}

/* Before a frame is written: what the receiver sent before it (still in
 * the driver or already queued) answers an earlier frame or is a repeated
 * start byte. Read it in without waiting and drop its control bytes. */
static void purge_stale(fl_rx_t *rx, bool before_frame1)
{
    fl_rx_poll(rx);
    fl_rx_drop_stale(rx, before_frame1);
}

/* After the ACK of a block that went out more than once: the receiver may
 * still answer the other copies (a duplicate block is acknowledged again).
 * Such an answer would count for the next block, and every answer after it
 * would be one block off. Listen XM_DUP_SETTLE_MS and drop them; text, a
 * cancel and the mismatch pattern count as in the block wait. */
static xm_result_t settle(const fl_io_t *io, fl_rx_t *rx, xm_state_t *st, uint32_t idx)
{
    uint32_t until = fl_now(io) + XM_DUP_SETTLE_MS;
    for (;;) {
        fl_ev_t ev = fl_rx_next(rx, until);
        switch (ev) {
        case FL_EV_TIMEOUT:
            return XM_OK;
        case FL_EV_ACK:
        case FL_EV_NAK:
        case FL_EV_C:
            st->late_answers++;
            fl_notef(io, "block %u: a late answer to an earlier copy (%s), dropped", (unsigned)(idx + 1u),
                     ev == FL_EV_ACK ? "ACK" : ev == FL_EV_NAK ? "NAK" : "C");
            break;
        case FL_EV_CANCEL:
            send_ack(io);
            strcpy(st->text, "CAN CAN");
            fl_notef(io, "block %u: the receiver cancelled", (unsigned)(idx + 1u));
            return XM_CANCELLED;
        case FL_EV_TEXT:
            if (line_has_any(rx, k_rx_error, sizeof(k_rx_error) / sizeof(k_rx_error[0]))) {
                note_line(io, "receiver error report", rx);
                break;
            }
            keep_text(st, rx);
            note_line(io, "receiver text during the transfer, stopping", rx);
            return XM_TEXT_ABORT;
        case FL_EV_MISMATCH:
            strcpy(st->text, "baud mismatch pattern");
            fl_notef(io, "block %u: baud-mismatch pattern, the receiver left the loader", (unsigned)(idx + 1u));
            return XM_BAUD_MISMATCH;
        default:
            break;   /* junk and a lone CAN: ignored */
        }
    }
}

/* The deadline of the two that comes first. */
static uint32_t earliest(const fl_io_t *io, uint32_t a, uint32_t b)
{
    return fl_left(io, a) <= fl_left(io, b) ? a : b;
}

uint32_t xm_blocks(uint32_t len)
{
    return (len + XM_BLOCK - 1u) / XM_BLOCK;
}

size_t xm_build_frame(const uint8_t *data, uint32_t len, uint32_t index, xm_mode_t mode, uint8_t *out)
{
    uint8_t blk = (uint8_t)((index + 1u) & 0xFFu);
    out[0] = FL_STX;
    out[1] = blk;
    out[2] = (uint8_t)(0xFFu - blk);
    uint32_t off = index * XM_BLOCK;
    uint32_t n = len - off < XM_BLOCK ? len - off : XM_BLOCK;
    memcpy(out + 3, data + off, n);
    if (n < XM_BLOCK) {
        out[3 + n] = XM_PAD_FIRST;
        memset(out + 3 + n + 1, 0, XM_BLOCK - n - 1u);
    }
    if (mode == XM_MODE_CRC) {
        uint16_t crc = fl_crc16_xmodem(out + 3, XM_BLOCK);
        out[3 + XM_BLOCK] = (uint8_t)(crc >> 8);
        out[4 + XM_BLOCK] = (uint8_t)(crc & 0xFFu);
        return 3u + XM_BLOCK + 2u;
    }
    out[3 + XM_BLOCK] = fl_sum8(out + 3, XM_BLOCK);
    return 3u + XM_BLOCK + 1u;
}

const char *xm_result_name(xm_result_t r)
{
    switch (r) {
    case XM_OK:
        return "ok";
    case XM_REFUSED:
        return "refused";
    case XM_HANDSHAKE_FAILED:
        return "handshake failed";
    case XM_NOT_STARTED:
        return "not started";
    case XM_CANCELLED:
        return "cancelled by the receiver";
    case XM_TEXT_ABORT:
        return "receiver text during the transfer";
    case XM_RETRY_LIMIT:
        return "retry limit";
    case XM_FRAME_TIMEOUT:
        return "block time limit";
    case XM_BAUD_MISMATCH:
        return "receiver left the loader";
    case XM_WRITE_FAILED:
        return "write failed";
    case XM_STOPPED:
        return "stopped on purpose";
    default:
        return "?";
    }
}

/* Wait for the first valid start byte. */
static xm_result_t handshake(const fl_io_t *io, fl_rx_t *rx, xm_state_t *st)
{
    uint32_t t0 = fl_now(io);
    uint32_t deadline = t0 + XM_HANDSHAKE_MS;
    for (;;) {
        fl_ev_t ev = fl_rx_next(rx, deadline);
        switch (ev) {
        case FL_EV_NAK:
            st->mode = XM_MODE_SUM;
            st->handshake_ms = fl_since(io, t0);
            fl_notef(io, "start byte NAK after %u ms: checksum mode", (unsigned)st->handshake_ms);
            return XM_OK;
        case FL_EV_C:
            st->mode = XM_MODE_CRC;
            st->handshake_ms = fl_since(io, t0);
            fl_notef(io, "start byte C after %u ms: CRC mode", (unsigned)st->handshake_ms);
            return XM_OK;
        case FL_EV_TEXT:
            if (line_has_any(rx, k_not_receiving, sizeof(k_not_receiving) / sizeof(k_not_receiving[0]))) {
                keep_text(st, rx);
                note_line(io, "handshake failed, loader text", rx);
                return XM_HANDSHAKE_FAILED;
            }
            note_line(io, "text while waiting for the start byte", rx);
            break;
        case FL_EV_CANCEL:
            send_ack(io);
            strcpy(st->text, "CAN CAN");
            fl_notef(io, "handshake failed: the receiver cancelled");
            return XM_HANDSHAKE_FAILED;
        case FL_EV_TIMEOUT:
            send_cancel(io);
            strcpy(st->text, "no start byte");
            fl_notef(io, "handshake failed: no start byte in %u ms", (unsigned)XM_HANDSHAKE_MS);
            return XM_HANDSHAKE_FAILED;
        default:
            st->start_junk++;   /* 0x00, high, other, lone CAN, a stray ACK: ignored */
            break;
        }
    }
}

xm_result_t xm_send(const fl_io_t *io, fl_rx_t *rx, const xm_job_t *job, xm_state_t *st)
{
    st->mode = XM_MODE_NONE;
    st->first_frame_sent = false;
    st->blocks = 0;
    st->blocks_acked = 0;
    st->frames_sent = 0;
    st->retries = 0;
    st->start_junk = 0;
    st->eots = 0;
    st->eot_acked = false;
    st->last_eot_ms = 0;
    st->handshake_ms = 0;
    st->late_answers = 0;
    st->text[0] = '\0';
    if (job->data == NULL || job->len == 0 || job->len % XM_BLOCK == 0) {
        return XM_REFUSED;
    }
    st->blocks = xm_blocks(job->len);

    xm_result_t r = handshake(io, rx, st);
    if (r != XM_OK) {
        return r;
    }
    if (job->before_first_frame != NULL && !job->before_first_frame(job->ctx, st->mode)) {
        return XM_NOT_STARTED;
    }

    for (uint32_t idx = 0; idx < st->blocks; idx++) {
        size_t flen = xm_build_frame(job->data, job->len, idx, st->mode, st->frame);
        uint32_t tries = 0;
        uint32_t t_block = fl_now(io);
        uint32_t cap = t_block + XM_FRAME_MAX_MS;
        bool acked = false;
        while (!acked) {
            purge_stale(rx, idx == 0);
            st->first_frame_sent = true;   /* from the first byte of frame 1 on */
            if (fl_write(io, st->frame, flen) != 0) {
                fl_notef(io, "block %u: write failed", (unsigned)(idx + 1u));
                return XM_WRITE_FAILED;
            }
            st->frames_sent++;
            uint32_t wait = earliest(io, fl_now(io) + XM_ACK_WAIT_MS, cap);
            bool resend = false;
            while (!acked && !resend) {
                fl_ev_t ev = fl_rx_next(rx, wait);
                switch (ev) {
                case FL_EV_ACK:
                    acked = true;
                    break;
                case FL_EV_NAK:
                case FL_EV_C:
                case FL_EV_TIMEOUT:
                    if (ev == FL_EV_TIMEOUT && fl_left(io, cap) == 0) {
                        send_cancel(io);
                        fl_notef(io, "block %u: no answer within %u ms", (unsigned)(idx + 1u),
                                 (unsigned)XM_FRAME_MAX_MS);
                        return XM_FRAME_TIMEOUT;
                    }
                    tries++;
                    st->retries++;
                    if (tries >= XM_BLOCK_TRIES || st->retries >= XM_TOTAL_TRIES) {
                        send_cancel(io);
                        fl_notef(io, "block %u: %u tries, %u in the transfer: giving up", (unsigned)(idx + 1u),
                                 (unsigned)tries, (unsigned)st->retries);
                        return XM_RETRY_LIMIT;
                    }
                    if (fl_left(io, cap) == 0) {
                        send_cancel(io);
                        fl_notef(io, "block %u: no acknowledgement within %u ms", (unsigned)(idx + 1u),
                                 (unsigned)XM_FRAME_MAX_MS);
                        return XM_FRAME_TIMEOUT;
                    }
                    resend = true;
                    break;
                case FL_EV_CANCEL:
                    send_ack(io);
                    strcpy(st->text, "CAN CAN");
                    fl_notef(io, "block %u: the receiver cancelled", (unsigned)(idx + 1u));
                    return XM_CANCELLED;
                case FL_EV_TEXT:
                    if (line_has_any(rx, k_rx_error, sizeof(k_rx_error) / sizeof(k_rx_error[0]))) {
                        note_line(io, "receiver error report", rx);
                        wait = earliest(io, fl_now(io) + XM_ACK_WAIT_MS, cap);
                        break;
                    }
                    keep_text(st, rx);
                    note_line(io, "receiver text during the transfer, stopping", rx);
                    return XM_TEXT_ABORT;
                case FL_EV_MISMATCH:
                    strcpy(st->text, "baud mismatch pattern");
                    fl_notef(io, "block %u: baud-mismatch pattern, the receiver left the loader", (unsigned)(idx + 1u));
                    return XM_BAUD_MISMATCH;
                default:
                    break;   /* junk and a lone CAN: ignored, the deadlines run on */
                }
            }
        }
        st->blocks_acked++;
        if (tries > 0 && idx + 1u < st->blocks) {
            /* The last block needs none: an extra answer to it lands in the
             * EOT wait, where an ACK or a NAK changes nothing that matters. */
            xm_result_t sr = settle(io, rx, st, idx);
            if (sr != XM_OK) {
                return sr;
            }
        }
        if (job->on_acked != NULL) {
            uint32_t bytes = (idx + 1u) * XM_BLOCK;
            job->on_acked(job->ctx, st->blocks_acked, bytes > job->len ? job->len : bytes, st->retries);
        }
        if (job->stop_after != 0 && st->blocks_acked >= job->stop_after && idx + 1u < st->blocks) {
            fl_notef(io, "stopping on purpose after %u acknowledged blocks", (unsigned)st->blocks_acked);
            return XM_STOPPED;
        }
    }

    /* End of transfer: only EOT from here on. */
    purge_stale(rx, false);
    if (job->before_eot != NULL) {
        job->before_eot(job->ctx);
    }
    rx->tap = job->post_tap;
    rx->tap_ctx = job->post_ctx;
    for (;;) {
        static const uint8_t eot = FL_EOT;
        int w = fl_write(io, &eot, 1);
        st->eots++;
        st->last_eot_ms = fl_now(io);
        if (w != 0) {
            fl_notef(io, "EOT write failed: waiting for the receiver's report");
            break;
        }
        fl_ev_t ev = fl_rx_next(rx, st->last_eot_ms + XM_EOT_WAIT_MS);
        if (ev == FL_EV_ACK) {
            st->eot_acked = true;
            break;
        }
        if (ev == FL_EV_NAK && st->eots < XM_EOT_MAX) {
            continue;
        }
        if (ev == FL_EV_TEXT) {
            note_line(io, "text after EOT", rx);
        }
        fl_notef(io, "EOT %u not acknowledged (%s): waiting for the receiver's report", (unsigned)st->eots,
                 ev == FL_EV_TIMEOUT ? "silence" : ev == FL_EV_NAK ? "NAK" : "other bytes");
        break;
    }
    rx->tap = NULL;
    rx->tap_ctx = NULL;
    return XM_OK;
}
