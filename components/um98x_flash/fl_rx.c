/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fl_rx.h"

#include <string.h>

/* ---- rolling window --------------------------------------------------------- */

void fl_win_init(fl_win_t *win)
{
    memset(win, 0, sizeof(*win));
}

void fl_win_clear(fl_win_t *win)
{
    uint64_t total = win->total;
    memset(win, 0, sizeof(*win));
    win->total = total;
}

int fl_win_watch(fl_win_t *win, const char *s)
{
    for (unsigned i = 0; i < win->nw; i++) {
        if (win->w[i].s == s || strcmp(win->w[i].s, s) == 0) {
            return (int)i;
        }
    }
    if (win->nw >= FL_WATCH_MAX || strlen(s) == 0 || strlen(s) > FL_WATCH_STR_MAX) {
        return -1;
    }
    win->w[win->nw].s = s;
    win->w[win->nw].seen = false;
    return (int)win->nw++;
}

void fl_win_append(fl_win_t *win, const uint8_t *data, size_t len)
{
    while (len > 0) {
        /* At most half a window at a time, so a watched string that starts
         * in the old part is still in the buffer when the new part lands. */
        size_t n = len < FL_WIN_CAP / 2 ? len : FL_WIN_CAP / 2;
        size_t keep_old = FL_WATCH_STR_MAX - 1;   /* bytes before the new ones a match may start in */
        if (win->len + n > FL_WIN_CAP) {
            size_t drop = win->len + n - FL_WIN_CAP;
            memmove(win->buf, win->buf + drop, win->len - drop);
            win->len -= drop;
        }
        size_t old_len = win->len;
        memcpy(win->buf + win->len, data, n);
        win->len += n;
        uint64_t base = win->total - old_len;   /* stream offset of buf[0] */
        win->total += n;
        size_t from = old_len > keep_old ? old_len - keep_old : 0;
        for (unsigned i = 0; i < win->nw; i++) {
            fl_watch_t *w = &win->w[i];
            size_t sl = strlen(w->s);
            for (size_t p = from; p + sl <= win->len; p++) {
                if (win->buf[p] != (uint8_t)w->s[0] || memcmp(win->buf + p, w->s, sl) != 0) {
                    continue;
                }
                uint64_t end = base + p + sl;
                if (end <= base + old_len) {
                    continue;   /* entirely in the old part: already counted */
                }
                if (!w->seen) {
                    w->seen = true;
                    w->first_end = end;
                }
                if (end > w->last_end) {
                    w->last_end = end;
                }
            }
        }
        data += n;
        len -= n;
    }
}

bool fl_win_seen(const fl_win_t *win, int i)
{
    return i >= 0 && (unsigned)i < win->nw && win->w[i].seen;
}

bool fl_win_seen_after(const fl_win_t *win, int i, uint64_t after)
{
    return fl_win_seen(win, i) && win->w[i].last_end > after;
}

bool fl_win_has(const fl_win_t *win, const char *s)
{
    return fl_memmem(win->buf, win->len, s) != NULL;
}

/* ---- classifier --------------------------------------------------------------- */

static bool printable(uint8_t b)
{
    return (b >= 0x20 && b <= 0x7E) || b == '\r' || b == '\n' || b == '\t';
}

static bool mismatch_byte(uint8_t b)
{
    return b == 0x00 || b == 0x78 || b == 0x80 || b == 0xF8;
}

void fl_rx_init(fl_rx_t *rx, const fl_io_t *io)
{
    memset(rx, 0, sizeof(*rx));
    rx->io = io;
    rx->err_base = io->line_errors != NULL ? io->line_errors(io->ctx) : 0;
}

/* Move the queued bytes to the front of the buffer. */
static void compact(fl_rx_t *rx)
{
    if (rx->q_head > 0) {
        memmove(rx->q, rx->q + rx->q_head, rx->q_len);
        rx->q_head = 0;
    }
}

void fl_rx_push(fl_rx_t *rx, const uint8_t *data, size_t len)
{
    compact(rx);
    size_t room = FL_RXQ_CAP - rx->q_len;
    if (len > room) {
        len = room;   /* never happens with the callers' read sizes */
    }
    memcpy(rx->q + rx->q_len, data, len);
    rx->q_len += len;
}

static uint8_t peek(const fl_rx_t *rx, size_t i)
{
    return rx->q[rx->q_head + i];
}

/* Read once into the queue, waiting up to wait_ms (<= one slice). */
static bool fill_once(fl_rx_t *rx, uint32_t wait_ms)
{
    if (rx->q_len >= FL_RXQ_CAP) {
        return true;   /* full: classify first */
    }
    compact(rx);
    if (wait_ms > FL_SLICE_MS) {
        wait_ms = FL_SLICE_MS;
    }
    size_t n = rx->io->read(rx->io->ctx, rx->q + rx->q_len, FL_RXQ_CAP - rx->q_len, wait_ms);
    if (n > 0) {
        if (rx->tap != NULL) {
            rx->tap(rx->tap_ctx, rx->q + rx->q_len, n);
        }
        rx->q_len += n;
    }
    return n > 0;
}

/* Wait (up to ms from now) until the queue holds at least `need` bytes. */
static bool have(fl_rx_t *rx, size_t need, uint32_t ms)
{
    uint32_t t0 = fl_now(rx->io);
    while (rx->q_len < need) {
        uint32_t spent = fl_since(rx->io, t0);
        if (spent >= ms) {
            return false;
        }
        (void)fill_once(rx, ms - spent);
    }
    return true;
}

static uint8_t take(fl_rx_t *rx)
{
    uint8_t b = rx->q[rx->q_head];
    rx->q_head++;
    rx->q_len--;
    if (rx->q_len == 0) {
        rx->q_head = 0;
    }
    if (mismatch_byte(b)) {
        rx->mismatch++;
    }
    return b;
}

static bool mismatch_hit(fl_rx_t *rx)
{
    uint32_t errs = 0;
    if (rx->io->line_errors != NULL) {
        errs = rx->io->line_errors(rx->io->ctx) - rx->err_base;
    }
    return rx->mismatch + errs >= FL_MISMATCH_BYTES;
}

void fl_rx_reset_mismatch(fl_rx_t *rx)
{
    rx->mismatch = 0;
    rx->mismatch_reported = false;
    rx->err_base = rx->io->line_errors != NULL ? rx->io->line_errors(rx->io->ctx) : 0;
}

static void line_add(fl_rx_t *rx, uint8_t b)
{
    if (rx->line_len < FL_LINE_MAX) {
        rx->line[rx->line_len++] = b;
        rx->line[rx->line_len] = 0;
    }
}

static bool line_blank(const fl_rx_t *rx)
{
    for (size_t i = 0; i < rx->line_len; i++) {
        uint8_t b = rx->line[i];
        if (b != '\r' && b != '\n' && b != '\t' && b != ' ') {
            return false;
        }
    }
    return true;
}

/* Text capture: the queue starts with the line (after `prefix_c` 'C's that
 * were already taken). Ends at LF, after FL_TEXT_GAP_MS of silence, at a
 * protocol control byte (left queued), or when the line is full. */
static fl_ev_t capture(fl_rx_t *rx, unsigned prefix_c)
{
    rx->line_len = 0;
    rx->line[0] = 0;
    for (unsigned i = 0; i < prefix_c; i++) {
        line_add(rx, 'C');
    }
    for (;;) {
        if (rx->q_len == 0 && !have(rx, 1, FL_TEXT_GAP_MS)) {
            break;
        }
        uint8_t b = peek(rx, 0);
        if (b == FL_ACK || b == FL_NAK || b == FL_CAN) {
            break;
        }
        (void)take(rx);
        line_add(rx, b);
        if (b == '\n' || rx->line_len >= FL_LINE_MAX) {
            break;
        }
    }
    if (line_blank(rx)) {
        rx->junk += (uint32_t)rx->line_len;
        return FL_EV_JUNK;
    }
    rx->lines++;
    return FL_EV_TEXT;
}

fl_ev_t fl_rx_next(fl_rx_t *rx, uint32_t deadline)
{
    for (;;) {
        if (!rx->mismatch_reported && mismatch_hit(rx)) {
            rx->mismatch_reported = true;
            return FL_EV_MISMATCH;
        }
        if (rx->q_len == 0) {
            uint32_t left = fl_left(rx->io, deadline);
            if (left == 0) {
                return FL_EV_TIMEOUT;
            }
            (void)fill_once(rx, left);
            continue;
        }
        uint8_t b = peek(rx, 0);
        if (b == FL_ACK) {
            (void)take(rx);
            fl_rx_reset_mismatch(rx);
            return FL_EV_ACK;
        }
        if (b == FL_NAK) {
            (void)take(rx);
            fl_rx_reset_mismatch(rx);
            return FL_EV_NAK;
        }
        if (b == FL_CAN) {
            (void)take(rx);
            if (have(rx, 1, FL_CAN_PAIR_MS) && peek(rx, 0) == FL_CAN) {
                (void)take(rx);
                return FL_EV_CANCEL;
            }
            rx->lone_cans++;
            return FL_EV_CAN1;   /* the next byte stays queued */
        }
        if (b == 'C') {
            /* The window runs from the first 'C': a run of them with no other
             * printable byte inside it is one start byte. Measured from each
             * 'C' instead, a receiver repeating its start byte faster than the
             * window would never end the run (and no deadline would hold). */
            unsigned run = 0;
            uint32_t t_first = fl_now(rx->io);
            for (;;) {
                (void)take(rx);
                run++;
                uint32_t spent = fl_since(rx->io, t_first);
                if (spent >= FL_LONE_C_MS || !have(rx, 1, FL_LONE_C_MS - spent)) {
                    return FL_EV_C;   /* later 'C's stay queued: their own events */
                }
                uint8_t n = peek(rx, 0);
                if (n == 'C') {
                    continue;
                }
                if (printable(n)) {
                    return capture(rx, run);
                }
                return FL_EV_C;   /* a control or high byte follows: processed on its own */
            }
        }
        if (printable(b)) {
            fl_ev_t ev = capture(rx, 0);
            if (!rx->mismatch_reported && mismatch_hit(rx)) {
                /* Garbage from another baud that happened to look printable. */
                rx->mismatch_reported = true;
                return FL_EV_MISMATCH;
            }
            return ev;
        }
        (void)take(rx);
        rx->junk++;
        if (!rx->mismatch_reported && mismatch_hit(rx)) {
            rx->mismatch_reported = true;
            return FL_EV_MISMATCH;
        }
        return FL_EV_JUNK;
    }
}

void fl_rx_poll(fl_rx_t *rx)
{
    while (rx->q_len < FL_RXQ_CAP && fill_once(rx, 0)) {
    }
}

void fl_rx_drop_stale(fl_rx_t *rx, bool c_run_at_end)
{
    while (rx->q_len > 0) {
        uint8_t b = peek(rx, 0);
        if (b == FL_ACK || b == FL_NAK) {
            (void)take(rx);
            continue;
        }
        if (b == 'C') {
            /* Only a 'C' that is not the start of text already received. */
            size_t i = 0;
            while (i < rx->q_len && peek(rx, i) == 'C') {
                i++;
            }
            if (i < rx->q_len && printable(peek(rx, i))) {
                return;
            }
            if (i == rx->q_len && !c_run_at_end) {
                /* The run ends the queue: whether text follows is not known yet. */
                return;
            }
            while (i-- > 0) {
                (void)take(rx);
            }
            continue;
        }
        return;
    }
}

bool fl_rx_line_has(const fl_rx_t *rx, const char *s)
{
    return fl_memmem(rx->line, rx->line_len, s) != NULL;
}
