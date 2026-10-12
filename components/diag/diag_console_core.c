/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_console_core.h"

#include <stdio.h>
#include <string.h>

void dcon_init(dcon_t *c, uint32_t cursor, char *buf, size_t cap)
{
    memset(c, 0, sizeof(*c));
    c->cursor = cursor;
    c->buf = buf;
    c->cap = cap;
}

size_t dcon_wire_len(const char *s, size_t len, bool crlf)
{
    if (!crlf) {
        return len;
    }
    size_t n = len;
    for (size_t i = 0; i < len; i++) {
        n += (s[i] == '\n');
    }
    return n;
}

bool dcon_direct_ok(const dcon_t *c, uint32_t seq, size_t wire_len, size_t room, size_t margin)
{
    return c->len == 0 && seq == c->cursor + 1u && room >= margin && wire_len <= room - margin;
}

void dcon_sent(dcon_t *c, uint32_t seq, const char *text, size_t len)
{
    c->cursor = seq;
    c->bytes += len;
    if (len > 0) {
        c->mid_line = text[len - 1u] != '\n';
    }
}

bool dcon_caught_up(const dcon_t *c, uint32_t next_seq)
{
    return c->len == 0 && (int32_t)(next_seq - (c->cursor + 1u)) <= 0;
}

/* Read the records after the cursor into a new batch. false = nothing waits. */
static bool load_batch(dcon_t *c, logring_t *ring, logring_lock_fn lock, logring_lock_fn unlock, void *lock_ctx,
                       dcon_loss_t *loss)
{
    if (c->buf == NULL || c->cap < DCON_BUF_SIZE) {
        return false;
    }
    char *text = c->buf + DCON_NOTE_MAX;
    size_t text_cap = c->cap - DCON_NOTE_MAX;
    uint32_t cur = c->cursor;
    logring_read_t ri;
    if (lock != NULL) {
        lock(lock_ctx);
    }
    /* Whole records only: the buffer holds the longest one, so nothing is cut. */
    size_t n = logring_read(ring, &cur, text, text_cap, &ri);
    uint64_t dropped_text = ring->dropped_text;
    if (unlock != NULL) {
        unlock(lock_ctx);
    }
    /* Records the cursor skipped without copying them were overwritten. */
    uint32_t lost = (cur - c->cursor) - ri.records;
    if (ri.records == 0 && lost == 0) {
        return false;
    }
    c->start = text;
    c->len = n;
    c->off = 0;
    c->batch_cursor = cur;
    c->batch_bytes = c->bytes + n;
    if (lost > 0) {
        /* The first record copied follows every dropped one: up to it the
         * ring dropped dropped_text bytes, of which c->bytes were sent. */
        uint64_t lost_bytes = dropped_text > c->bytes ? dropped_text - c->bytes : 0;
        c->dropped += lost;
        c->dropped_bytes += lost_bytes;
        c->batch_bytes = (dropped_text > c->bytes ? dropped_text : c->bytes) + n;
        if (loss != NULL) {
            loss->lost += lost;
            loss->lost_bytes += lost_bytes;
        }
        char note[DCON_NOTE_MAX];
        int k = snprintf(note, sizeof(note), "%s[diag: %u console line%s dropped]\n", c->mid_line ? "\n" : "",
                         (unsigned)lost, lost == 1u ? "" : "s");
        size_t note_len = (k > 0 && (size_t)k < sizeof(note)) ? (size_t)k : 0;
        memcpy(text - note_len, note, note_len);
        c->start = text - note_len;
        c->len = n + note_len;
    }
    return true;
}

/* Text bytes of s that fit `avail` wire bytes. */
static size_t fit(const char *s, size_t len, size_t avail, bool crlf)
{
    size_t wire = 0;
    size_t i = 0;
    for (; i < len; i++) {
        size_t w = (crlf && s[i] == '\n') ? 2u : 1u;
        if (wire + w > avail) {
            break;
        }
        wire += w;
    }
    return i;
}

dcon_pump_t dcon_pump(dcon_t *c, logring_t *ring, logring_lock_fn lock, logring_lock_fn unlock, void *lock_ctx,
                      const dcon_tx_t *tx, dcon_loss_t *loss)
{
    if (c->len == 0 && !load_batch(c, ring, lock, unlock, lock_ctx, loss)) {
        return DCON_IDLE;
    }
    const char *s = c->start + c->off;
    size_t rest = c->len - c->off;
    size_t room = tx->room(tx->ctx);
    size_t avail = room > tx->margin ? room - tx->margin : 0;
    size_t k = fit(s, rest, avail, tx->crlf);
    if (k < rest) {
        /* Whole lines where they fit; a line is split only when it could
         * never fit the buffer whole. */
        size_t whole = k;
        while (whole > 0 && s[whole - 1u] != '\n') {
            whole--;
        }
        if (whole > 0) {
            k = whole;
        } else {
            const char *nl = memchr(s, '\n', rest);
            size_t line = nl != NULL ? (size_t)(nl - s) + 1u : rest;
            size_t limit = tx->total > tx->margin ? tx->total - tx->margin : 0;
            if (dcon_wire_len(s, line, tx->crlf) <= limit) {
                k = 0;   /* it fits once the buffer drains: wait for that */
            }
        }
    }
    if (k == 0) {
        return DCON_BUSY;
    }
    tx->write(tx->ctx, s, k);
    c->off += k;
    c->mid_line = s[k - 1u] != '\n';
    if (c->off == c->len) {
        c->cursor = c->batch_cursor;
        c->bytes = c->batch_bytes;
        c->len = 0;
        c->off = 0;
        c->start = NULL;
    }
    return DCON_MORE;
}
