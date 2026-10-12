/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Console writer core against a model of the UART: a transmit buffer of a
 * given size that drains at 115200 baud (11.5 bytes per millisecond, in
 * 10 ms steps), loggers that append to a real log ring and try the fast
 * path, and the writer task that pumps whenever it runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_console_core.h"
#include "diag_logring.h"
#include "unity.h"

#define OUT_MAX (256u * 1024u)

typedef struct {
    size_t cap;          /* transmit buffer size */
    size_t fill;         /* wire bytes waiting in it */
    bool crlf;
    char *out;           /* text handed over, in order (before line-ending expansion) */
    size_t out_len;
} uart_model_t;

static size_t m_room(void *ctx)
{
    uart_model_t *u = ctx;
    return u->cap - u->fill;
}

static void m_write(void *ctx, const char *s, size_t len)
{
    uart_model_t *u = ctx;
    size_t wire = dcon_wire_len(s, len, u->crlf);
    TEST_ASSERT_TRUE_MESSAGE(u->fill + wire <= u->cap, "a write that does not fit would block");
    u->fill += wire;
    TEST_ASSERT_TRUE(u->out_len + len <= OUT_MAX);
    memcpy(u->out + u->out_len, s, len);
    u->out_len += len;
}

static void m_drain(uart_model_t *u, size_t bytes)
{
    u->fill = u->fill > bytes ? u->fill - bytes : 0;
}

typedef struct {
    uint8_t *mem;
    logring_t ring;
    dcon_t con;
    char buf[DCON_BUF_SIZE];
    uart_model_t uart;
    dcon_tx_t tx;
    char *expect;        /* every line logged, in order */
    size_t expect_len;
    uint32_t direct;     /* lines that took the fast path */
} world_t;

static void world_init(world_t *w, size_t ring_bytes, size_t tx_cap, bool crlf)
{
    memset(w, 0, sizeof(*w));
    w->mem = calloc(1, ring_bytes);
    TEST_ASSERT_NOT_NULL(w->mem);
    logring_attach(&w->ring, w->mem, ring_bytes, NULL, NULL);
    static const uint8_t image[LOGRING_IMAGE_ID] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    logring_set_owner(&w->ring, 0, image);
    logring_format(&w->ring, 1, 41);   /* sequence numbers continue from an earlier boot */
    dcon_init(&w->con, w->ring.hdr->next_seq - 1u, w->buf, sizeof(w->buf));
    w->uart.cap = tx_cap;
    w->uart.crlf = crlf;
    w->uart.out = malloc(OUT_MAX);
    w->expect = malloc(OUT_MAX);
    TEST_ASSERT_NOT_NULL(w->uart.out);
    TEST_ASSERT_NOT_NULL(w->expect);
    w->tx = (dcon_tx_t){ .room = m_room, .write = m_write, .ctx = &w->uart, .total = tx_cap, .margin = 64, .crlf = crlf };
}

static void world_free(world_t *w)
{
    free(w->mem);
    free(w->uart.out);
    free(w->expect);
}

/* A logger: the record goes to the ring first, then straight to the UART if it may. */
static void log_text(world_t *w, const char *text, size_t len)
{
    uint32_t seq = logring_append(&w->ring, text, len);
    TEST_ASSERT_NOT_EQUAL(0, seq);
    memcpy(w->expect + w->expect_len, text, len);
    w->expect_len += len;
    size_t wire = dcon_wire_len(text, len, w->uart.crlf);
    if (dcon_direct_ok(&w->con, seq, wire, m_room(&w->uart), w->tx.margin)) {
        dcon_sent(&w->con, seq, text, len);
        m_write(&w->uart, text, len);
        w->direct++;
    }
}

static void log_line(world_t *w, unsigned i, size_t len)
{
    char line[256];
    TEST_ASSERT_TRUE(len < sizeof(line));
    int n = snprintf(line, sizeof(line), "I (%6u) test: line %04u ", i * 3u, i);
    while ((size_t)n < len - 1u) {
        line[n] = (char)('a' + (n + i) % 26u);
        n++;
    }
    line[len - 1u] = '\n';
    log_text(w, line, len);
}

/* The writer task: pump until it is idle or the buffer is full. */
static dcon_pump_t writer(world_t *w, dcon_loss_t *loss)
{
    dcon_pump_t r;
    int guard = 0;
    while ((r = dcon_pump(&w->con, &w->ring, NULL, NULL, NULL, &w->tx, loss)) == DCON_MORE) {
        TEST_ASSERT_TRUE(++guard < 100000);
    }
    return r;
}

/* Time passes in 10 ms steps: the UART drains 115 bytes, the writer pumps. */
static void run_until_idle(world_t *w, dcon_loss_t *loss)
{
    for (int step = 0; step < 100000; step++) {
        m_drain(&w->uart, 115);
        if (writer(w, loss) == DCON_IDLE && w->uart.fill == 0) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("the writer never caught up");
}

static size_t count_str(const char *hay, size_t len, const char *needle)
{
    size_t n = 0;
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= len; i++) {
        if (memcmp(hay + i, needle, nl) == 0) {
            n++;
        }
    }
    return n;
}

TEST_CASE("console: a 300-line burst through a 4 KB buffer arrives whole and in order", "[diag_console]")
{
    static world_t w;
    world_init(&w, 64u * 1024u, 4096u, true);
    for (unsigned i = 0; i < 300; i++) {
        log_line(&w, i, 80);   /* the whole burst at once: the UART has no time to drain */
    }
    TEST_ASSERT_TRUE(w.direct > 0 && w.direct < 300);   /* the first ones fit, the rest wait in the ring */
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_UINT32(0, loss.lost);
    TEST_ASSERT_EQUAL_UINT32(0, w.con.dropped);
    TEST_ASSERT_EQUAL_UINT64(0, w.con.dropped_bytes);
    TEST_ASSERT_EQUAL_size_t(w.expect_len, w.uart.out_len);
    TEST_ASSERT_EQUAL_MEMORY(w.expect, w.uart.out, w.expect_len);
    TEST_ASSERT_TRUE(dcon_caught_up(&w.con, w.ring.hdr->next_seq));

    /* Caught up: the fast path is open again and keeps the order. */
    size_t before = w.uart.out_len;
    uint32_t direct = w.direct;
    log_line(&w, 300, 80);
    TEST_ASSERT_EQUAL_UINT32(direct + 1u, w.direct);
    TEST_ASSERT_EQUAL_size_t(before + 80u, w.uart.out_len);
    world_free(&w);
}

TEST_CASE("console: while lines wait, a new line never jumps the queue", "[diag_console]")
{
    static world_t w;
    world_init(&w, 64u * 1024u, 1024u, false);
    for (unsigned i = 0; i < 40; i++) {
        log_line(&w, i, 100);
    }
    uint32_t direct = w.direct;
    /* the buffer drains completely, but older lines still wait in the ring */
    m_drain(&w.uart, 1024);
    log_line(&w, 40, 100);
    TEST_ASSERT_EQUAL_UINT32(direct, w.direct);
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_MEMORY(w.expect, w.uart.out, w.expect_len);
    TEST_ASSERT_EQUAL_size_t(w.expect_len, w.uart.out_len);
    world_free(&w);
}

TEST_CASE("console: a ring overrun gives one note with the exact count, at a line start", "[diag_console]")
{
    static world_t w;
    world_init(&w, 8u * 1024u, 4096u, true);
    /* The UART is stuck (nothing drains) while 300 lines arrive: the ring
     * keeps only the newest ones. */
    for (unsigned i = 0; i < 300; i++) {
        log_line(&w, i, 80);
    }
    uint32_t sent_direct = w.direct;
    uint32_t first = w.ring.hdr->first_seq;
    /* records 41 + sent_direct .. first - 1 were overwritten before they were sent */
    uint32_t lost = first - (41u + sent_direct);
    TEST_ASSERT_TRUE(lost > 100);
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_UINT32(lost, loss.lost);
    TEST_ASSERT_EQUAL_UINT32(lost, w.con.dropped);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lost * 80u, w.con.dropped_bytes);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)lost * 80u, loss.lost_bytes);

    char note[64];
    snprintf(note, sizeof(note), "[diag: %u console lines dropped]\n", (unsigned)lost);
    TEST_ASSERT_EQUAL_size_t(1, count_str(w.uart.out, w.uart.out_len, "[diag:"));
    const char *at = NULL;
    for (size_t i = 0; i + strlen(note) <= w.uart.out_len; i++) {
        if (memcmp(w.uart.out + i, note, strlen(note)) == 0) {
            at = w.uart.out + i;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(at, "the note with the exact count");
    TEST_ASSERT_TRUE(at > w.uart.out);
    TEST_ASSERT_EQUAL_CHAR('\n', at[-1]);   /* after a whole line */
    /* What was sent: the lines that went out directly, the note, then the
     * newest lines the ring still held, in order. */
    size_t head = (size_t)sent_direct * 80u;
    TEST_ASSERT_EQUAL_MEMORY(w.expect, w.uart.out, head);
    TEST_ASSERT_EQUAL_PTR(w.uart.out + head, at);
    size_t tail = (size_t)(300u - sent_direct - lost) * 80u;
    TEST_ASSERT_EQUAL_size_t(head + strlen(note) + tail, w.uart.out_len);
    TEST_ASSERT_EQUAL_MEMORY(w.expect + w.expect_len - tail, at + strlen(note), tail);

    /* A later overrun gets its own note. */
    for (unsigned i = 0; i < 300; i++) {
        log_line(&w, 300 + i, 80);
    }
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_size_t(2, count_str(w.uart.out, w.uart.out_len, "[diag:"));
    world_free(&w);
}

TEST_CASE("console: a note after a line fragment starts on a new line", "[diag_console]")
{
    static world_t w;
    world_init(&w, 4u * 1024u, 4096u, false);
    /* A library logs one line in two calls: the first part goes out at once. */
    log_text(&w, "I (1140) wifi:", 14);
    TEST_ASSERT_EQUAL_UINT32(1, w.direct);
    TEST_ASSERT_TRUE(w.con.mid_line);
    /* The UART stalls; its continuation and many more lines are overwritten. */
    w.uart.fill = w.uart.cap;
    log_text(&w, "country set\n", 12);
    for (unsigned i = 0; i < 100; i++) {
        log_line(&w, i, 80);
    }
    w.uart.fill = 0;
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_TRUE(loss.lost > 0);
    TEST_ASSERT_EQUAL_MEMORY("I (1140) wifi:\n[diag: ", w.uart.out, 22);
    TEST_ASSERT_EQUAL_size_t(1, count_str(w.uart.out, w.uart.out_len, "[diag:"));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)12u + (uint64_t)(loss.lost - 1u) * 80u, loss.lost_bytes);
    TEST_ASSERT_FALSE(w.con.mid_line);
    world_free(&w);
}

TEST_CASE("console: fragments that both survive stay together", "[diag_console]")
{
    static world_t w;
    world_init(&w, 64u * 1024u, 1024u, true);
    for (unsigned i = 0; i < 20; i++) {
        log_line(&w, i, 90);   /* fills the buffer: everything after waits */
    }
    log_text(&w, "I (2000) wifi:", 14);
    log_text(&w, "mode : sta\n", 11);
    log_line(&w, 20, 90);
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_UINT32(0, loss.lost);
    TEST_ASSERT_EQUAL_size_t(1, count_str(w.uart.out, w.uart.out_len, "I (2000) wifi:mode : sta\n"));
    TEST_ASSERT_EQUAL_MEMORY(w.expect, w.uart.out, w.expect_len);
    world_free(&w);
}

TEST_CASE("console: a line longer than the whole buffer still goes out, in parts", "[diag_console]")
{
    static world_t w;
    world_init(&w, 64u * 1024u, 1024u, true);
    w.uart.fill = 900;   /* busy: the long line must wait */
    char big[1000];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1u] = '\n';
    log_text(&w, big, sizeof(big));
    log_line(&w, 1, 60);
    TEST_ASSERT_EQUAL_UINT32(0, w.direct);
    dcon_loss_t loss = { 0 };
    run_until_idle(&w, &loss);
    TEST_ASSERT_EQUAL_size_t(w.expect_len, w.uart.out_len);
    TEST_ASSERT_EQUAL_MEMORY(w.expect, w.uart.out, w.expect_len);
    world_free(&w);
}

TEST_CASE("console: caught up only once the cursor reached the ring's next record", "[diag_console]")
{
    static world_t w;
    world_init(&w, 16u * 1024u, 1024u, false);
    TEST_ASSERT_TRUE(dcon_caught_up(&w.con, w.ring.hdr->next_seq));   /* nothing logged yet */
    for (unsigned i = 0; i < 30; i++) {
        log_line(&w, i, 100);
    }
    TEST_ASSERT_FALSE(dcon_caught_up(&w.con, w.ring.hdr->next_seq));
    /* One pump with a little room: a batch is in flight, the cursor has not moved yet. */
    m_drain(&w.uart, 300);
    TEST_ASSERT_EQUAL(DCON_MORE, dcon_pump(&w.con, &w.ring, NULL, NULL, NULL, &w.tx, NULL));
    TEST_ASSERT_TRUE(w.con.len > 0);
    TEST_ASSERT_FALSE(dcon_caught_up(&w.con, w.ring.hdr->next_seq));
    run_until_idle(&w, NULL);
    TEST_ASSERT_TRUE(dcon_caught_up(&w.con, w.ring.hdr->next_seq));
    TEST_ASSERT_EQUAL_UINT32(w.ring.hdr->next_seq - 1u, w.con.cursor);
    TEST_ASSERT_EQUAL(DCON_IDLE, dcon_pump(&w.con, &w.ring, NULL, NULL, NULL, &w.tx, NULL));
    world_free(&w);
}
