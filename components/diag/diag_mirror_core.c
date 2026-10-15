/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_mirror_core.h"

#include <stdio.h>
#include <string.h>

void dmir_init(dmir_t *m, char *buf, size_t cap)
{
    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->cap = buf != NULL ? cap : 0;
}

static uint32_t add_sat(uint32_t a, uint32_t b)
{
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

/* `len` bytes into the scratch at *n, each '\n' as the port's line ending. */
static void put_text(const dmir_t *m, const dmir_port_t *p, size_t *n, const char *s, size_t len)
{
    size_t j = *n;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n' && p->eol != DMIR_EOL_LF) {
            m->buf[j++] = '\r';
            if (p->eol == DMIR_EOL_CR) {
                continue;
            }
        }
        m->buf[j++] = s[i];
    }
    *n = j;
}

/* Text the port did not get: a loss (counted) when a host was attached. */
static void not_sent(dmir_t *m, const char *s, size_t len, bool host)
{
    bool ends = s[len - 1u] == '\n';
    bool has_nl = memchr(s, '\n', len) != NULL;
    m->gap = true;
    if (host) {
        uint32_t lines = 0;
        for (size_t i = 0; i < len; i++) {
            lines += s[i] == '\n';
        }
        lines += ends ? 0u : 1u;   /* the line it leaves open */
        if (m->in_line && m->line_lost) {
            lines--;               /* its first bytes continue a line counted already */
        }
        m->lost = add_sat(m->lost, lines);
        m->pending = add_sat(m->pending, lines);
        m->lost_bytes += len;
        m->line_lost = !ends;
    } else if (has_nl) {
        m->line_lost = false;      /* the line it leaves open is not counted */
    }
    m->in_line = !ends;
}

/* One piece, its text at most (cap - DMIR_NOTE_MAX) / 2 bytes. */
static void write_piece(dmir_t *m, const dmir_port_t *p, const char *s, size_t len)
{
    if (p->connected != NULL && !p->connected(p->ctx)) {
        not_sent(m, s, len, false);
        return;
    }
    size_t n = 0;
    if (m->pending > 0) {
        char note[DMIR_NOTE_MAX];
        int k = snprintf(note, sizeof(note), "%s[diag: %u line%s not sent on USB]\n", m->sent_mid_line ? "\n" : "",
                         (unsigned)m->pending, m->pending == 1u ? "" : "s");
        size_t note_len = (k > 0 && (size_t)k < sizeof(note)) ? (size_t)k : 0;
        /* The note's own line endings: at most two more bytes, inside DMIR_NOTE_MAX. */
        put_text(m, p, &n, note, note_len);
    } else if (m->gap && m->sent_mid_line && s[0] != '\n') {
        put_text(m, p, &n, "\n", 1);   /* the host never saw the end of that line */
    }
    put_text(m, p, &n, s, len);
    if (!p->put(p->ctx, m->buf, n)) {
        not_sent(m, s, len, true);
        return;
    }
    bool ends = s[len - 1u] == '\n';
    if (memchr(s, '\n', len) != NULL) {
        m->line_lost = false;
    }
    m->sent_mid_line = !ends;
    m->in_line = !ends;
    m->gap = false;
    m->pending = 0;
}

void dmir_write(dmir_t *m, const dmir_port_t *port, const char *s, size_t len)
{
    if (m == NULL || port == NULL || port->put == NULL || s == NULL || len == 0) {
        return;
    }
    size_t piece = m->cap > DMIR_NOTE_MAX ? (m->cap - DMIR_NOTE_MAX) / 2u : 0;
    if (piece == 0) {
        not_sent(m, s, len, true);   /* no scratch: nothing can go out */
        return;
    }
    while (len > 0) {
        size_t k = len < piece ? len : piece;
        write_piece(m, port, s, k);
        s += k;
        len -= k;
    }
}
