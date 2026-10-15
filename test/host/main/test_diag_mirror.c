/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The console's copy on the built-in USB against a model of that port: a
 * transmit buffer of a given size that takes a write whole or not at all,
 * a host that is attached or not and reads (drains the buffer) when told.
 */
#include <stdio.h>
#include <string.h>

#include "diag_mirror_core.h"
#include "unity.h"

typedef struct {
    bool host;           /* a host is attached */
    size_t cap;          /* transmit buffer */
    size_t fill;
    size_t puts;         /* writes handed over (taken or not) */
    char out[8192];      /* bytes taken, in order */
    size_t out_len;
} port_model_t;

static bool m_connected(void *ctx)
{
    return ((port_model_t *)ctx)->host;
}

static bool m_put(void *ctx, const char *s, size_t len)
{
    port_model_t *p = ctx;
    p->puts++;
    TEST_ASSERT_TRUE_MESSAGE(p->host, "nothing is handed over without a host");
    if (p->fill + len > p->cap) {
        return false;
    }
    p->fill += len;
    TEST_ASSERT_TRUE(p->out_len + len <= sizeof(p->out));
    memcpy(p->out + p->out_len, s, len);
    p->out_len += len;
    p->out[p->out_len] = '\0';
    return true;
}

static void m_read(port_model_t *p)
{
    p->fill = 0;
}

typedef struct {
    port_model_t usb;
    dmir_port_t port;
    dmir_t m;
    char buf[DMIR_BUF_SIZE(256)];
} rig_t;

static void rig(rig_t *r, size_t cap, dmir_eol_t eol)
{
    memset(r, 0, sizeof(*r));
    r->usb.host = true;
    r->usb.cap = cap;
    r->port = (dmir_port_t){ .connected = m_connected, .put = m_put, .ctx = &r->usb, .eol = eol };
    dmir_init(&r->m, r->buf, sizeof(r->buf));
}

static void w(rig_t *r, const char *s)
{
    dmir_write(&r->m, &r->port, s, strlen(s));
}

TEST_CASE("usb copy: a host that reads gets every write as it is, CR LF line endings", "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 4096, DMIR_EOL_CRLF);
    w(&r, "I (100) app: one\n");
    w(&r, "I (101) app: two\nI (102) app: three\n");
    w(&r, "> ");   /* a prompt waits on its line */
    TEST_ASSERT_EQUAL_STRING("I (100) app: one\r\nI (101) app: two\r\nI (102) app: three\r\n> ", r.usb.out);
    TEST_ASSERT_EQUAL_UINT32(0, r.m.lost);
    TEST_ASSERT_EQUAL_UINT64(0, r.m.lost_bytes);
    TEST_ASSERT_EQUAL(3, r.usb.puts);
}

TEST_CASE("usb copy: without a host nothing is handed over and nothing counts as lost", "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 4096, DMIR_EOL_LF);
    w(&r, "before\nhalf a li");
    r.usb.host = false;
    w(&r, "ne\n");
    w(&r, "nobody saw this\n");
    TEST_ASSERT_EQUAL_UINT32(0, r.m.lost);
    TEST_ASSERT_EQUAL_UINT64(0, r.m.lost_bytes);
    TEST_ASSERT_EQUAL(1, r.usb.puts);
    r.usb.host = true;
    w(&r, "after\n");
    /* The open line's end was never sent: the next write starts on a line of its own, without a note. */
    TEST_ASSERT_EQUAL_STRING("before\nhalf a li\nafter\n", r.usb.out);
    w(&r, "and on\n");
    TEST_ASSERT_EQUAL_STRING("before\nhalf a li\nafter\nand on\n", r.usb.out);
}

TEST_CASE("usb copy: a host that does not read loses lines, counted, and the next write says how many", "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 32, DMIR_EOL_LF);
    w(&r, "0123456789\n");                  /* 11 of 32 */
    w(&r, "0123456789\n");                  /* 22 */
    w(&r, "0123456789\n");                  /* 33: lost */
    w(&r, "a\nb\nc\n");                     /* 28: fits, but notes come first: 6 + note does not fit, lost */
    TEST_ASSERT_EQUAL_UINT32(4, r.m.lost);
    TEST_ASSERT_EQUAL_UINT64(11 + 6, r.m.lost_bytes);
    TEST_ASSERT_EQUAL_UINT32(4, r.m.pending);
    m_read(&r.usb);
    r.usb.cap = 4096;
    w(&r, "back\n");
    TEST_ASSERT_EQUAL_STRING("0123456789\n0123456789\n[diag: 4 lines not sent on USB]\nback\n", r.usb.out);
    TEST_ASSERT_EQUAL_UINT32(0, r.m.pending);
    TEST_ASSERT_EQUAL_UINT32(4, r.m.lost);
    w(&r, "on\n");
    TEST_ASSERT_EQUAL_STRING("0123456789\n0123456789\n[diag: 4 lines not sent on USB]\nback\non\n", r.usb.out);
}

TEST_CASE("usb copy: a note goes out only with its write, and one loss is a line", "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 8, DMIR_EOL_CRLF);
    w(&r, "too long for it\n");
    TEST_ASSERT_EQUAL_UINT32(1, r.m.lost);
    w(&r, "x\n");   /* 3 bytes would fit, but not with the note in front */
    TEST_ASSERT_EQUAL_UINT32(2, r.m.lost);
    TEST_ASSERT_EQUAL_size_t(0, r.usb.out_len);
    r.usb.cap = 4096;
    w(&r, "y\n");
    TEST_ASSERT_EQUAL_STRING("[diag: 2 lines not sent on USB]\r\ny\r\n", r.usb.out);
    rig(&r, 4096, DMIR_EOL_LF);
    r.usb.fill = r.usb.cap;
    w(&r, "one\n");
    m_read(&r.usb);
    w(&r, "two\n");
    TEST_ASSERT_EQUAL_STRING("[diag: 1 line not sent on USB]\ntwo\n", r.usb.out);
}

TEST_CASE("usb copy: a line written in parts counts once, and a note after a part sent starts a new line",
          "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 4096, DMIR_EOL_LF);
    w(&r, "start of a ");          /* sent: the host has a line open */
    r.usb.fill = r.usb.cap;        /* stops reading */
    w(&r, "line in ");             /* lost: that line counts */
    w(&r, "parts\nnext ");         /* the same line (not again) and the next one */
    w(&r, "line\n");               /* the next one again: not again */
    TEST_ASSERT_EQUAL_UINT32(2, r.m.lost);
    TEST_ASSERT_EQUAL_UINT64(8 + 11 + 5, r.m.lost_bytes);
    m_read(&r.usb);
    w(&r, "later\n");
    TEST_ASSERT_EQUAL_STRING("start of a \n[diag: 2 lines not sent on USB]\nlater\n", r.usb.out);

    /* A part lost while no host was there is not counted, even when a loss continues its line. */
    rig(&r, 4096, DMIR_EOL_LF);
    r.usb.host = false;
    w(&r, "open ");
    r.usb.host = true;
    r.usb.fill = r.usb.cap;
    w(&r, "line\n");
    TEST_ASSERT_EQUAL_UINT32(1, r.m.lost);
    w(&r, "a\nb");
    TEST_ASSERT_EQUAL_UINT32(3, r.m.lost);
}

TEST_CASE("usb copy: text longer than the scratch goes in pieces, each whole or not at all", "[diag_mirror]")
{
    static rig_t r;
    static char big[1000];
    rig(&r, 4096, DMIR_EOL_CRLF);
    for (size_t i = 0; i < sizeof(big) - 1u; i++) {
        big[i] = (i % 50u == 49u) ? '\n' : (char)('a' + i % 26u);
    }
    big[sizeof(big) - 1u] = '\0';
    w(&r, big);
    const size_t piece = (sizeof(r.buf) - DMIR_NOTE_MAX) / 2u;
    TEST_ASSERT_EQUAL((sizeof(big) - 1u + piece - 1u) / piece, r.usb.puts);
    /* What went out, with CR LF taken back to LF, is the text. */
    static char back[2048];
    size_t n = 0;
    for (size_t i = 0; i < r.usb.out_len; i++) {
        if (r.usb.out[i] != '\r') {
            back[n++] = r.usb.out[i];
        }
    }
    back[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(big, back);
    TEST_ASSERT_EQUAL_UINT32(0, r.m.lost);
}

TEST_CASE("usb copy: CR line endings, and nothing for empty writes", "[diag_mirror]")
{
    static rig_t r;
    rig(&r, 4096, DMIR_EOL_CR);
    w(&r, "a\nb\n");
    w(&r, "");
    dmir_write(&r.m, &r.port, NULL, 3);
    TEST_ASSERT_EQUAL_STRING("a\rb\r", r.usb.out);
    TEST_ASSERT_EQUAL(1, r.usb.puts);
}
