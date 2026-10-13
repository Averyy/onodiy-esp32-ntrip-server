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

/* ---- text that is not a log line, and lines gathered from pieces ----------------------------------------- */

typedef struct {
    char out[4096];
    size_t len;
    unsigned calls;
    size_t sizes[64];
} sink_t;

static void sink_emit(void *ctx, const char *s, size_t len)
{
    sink_t *k = ctx;
    TEST_ASSERT_TRUE(k->len + len <= sizeof(k->out));
    memcpy(k->out + k->len, s, len);
    k->len += len;
    if (k->calls < 64) {
        k->sizes[k->calls] = len;
    }
    k->calls++;
}

TEST_CASE("console lines: pieces are handed over as whole lines only", "[diag_console]")
{
    static sink_t k;
    memset(&k, 0, sizeof(k));
    char buf[64];
    dcon_lines_t g;
    dcon_lines_init(&g, buf, sizeof(buf));
    /* One line in three pieces: nothing until it ends, then all of it at once. */
    dcon_lines_put(&g, "BENCH state 3", 13, sink_emit, &k);
    dcon_lines_put(&g, " (streaming)", 12, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(0, k.calls);
    dcon_lines_put(&g, ", flashing built in\n", 20, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(1, k.calls);
    TEST_ASSERT_EQUAL_size_t(45, k.sizes[0]);
    TEST_ASSERT_EQUAL_MEMORY("BENCH state 3 (streaming), flashing built in\n", k.out, 45);
    TEST_ASSERT_EQUAL_size_t(0, g.len);
    /* A piece that ends two lines and starts a third: both lines in one hand-over, the start kept. */
    dcon_lines_put(&g, "a\nb\nc", 5, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(2, k.calls);
    TEST_ASSERT_EQUAL_size_t(4, k.sizes[1]);
    TEST_ASSERT_EQUAL_size_t(1, g.len);
    /* The end of a reply ends its last line. */
    dcon_lines_flush(&g, true, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(3, k.calls);
    TEST_ASSERT_EQUAL_MEMORY("a\nb\nc\n", k.out + 45, 6);
    /* A prompt goes as it is; nothing gathered, nothing handed over. */
    dcon_lines_put(&g, "bench> ", 7, sink_emit, &k);
    dcon_lines_flush(&g, false, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(4, k.calls);
    TEST_ASSERT_EQUAL_MEMORY("bench> ", k.out + 51, 7);
    dcon_lines_flush(&g, true, sink_emit, &k);
    dcon_lines_flush(&g, false, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(4, k.calls);
    /* Text the caller wrote into the buffer itself (printf straight into it). */
    memcpy(g.buf + g.len, "x=1", 3);
    dcon_lines_added(&g, 3, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(4, k.calls);
    memcpy(g.buf + g.len, ", y=2\n", 6);
    dcon_lines_added(&g, 6, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(5, k.calls);
    TEST_ASSERT_EQUAL_MEMORY("x=1, y=2\n", k.out + 58, 9);
}

TEST_CASE("console lines: a line longer than the gatherer goes out in parts of its size", "[diag_console]")
{
    static sink_t k;
    memset(&k, 0, sizeof(k));
    char buf[16];
    dcon_lines_t g;
    dcon_lines_init(&g, buf, sizeof(buf));
    char line[41];
    memset(line, 'x', 40);
    line[40] = '\n';
    dcon_lines_put(&g, line, sizeof(line), sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(3, k.calls);   /* 16 + 16 + the last 9 with the line end */
    TEST_ASSERT_EQUAL_size_t(16, k.sizes[0]);
    TEST_ASSERT_EQUAL_size_t(16, k.sizes[1]);
    TEST_ASSERT_EQUAL_size_t(9, k.sizes[2]);
    TEST_ASSERT_EQUAL_MEMORY(line, k.out, sizeof(line));
    /* A full buffer that a reply's end must still end: the part, then the line end. */
    dcon_lines_put(&g, line, 16, sink_emit, &k);
    TEST_ASSERT_EQUAL_size_t(0, g.len);   /* handed over as soon as it was full */
    dcon_lines_put(&g, line, 15, sink_emit, &k);
    dcon_lines_flush(&g, true, sink_emit, &k);
    TEST_ASSERT_EQUAL_size_t(41 + 16 + 16, k.len);
    TEST_ASSERT_EQUAL_CHAR('\n', k.out[k.len - 1u]);
    /* No buffer at all: every piece as it comes. */
    dcon_lines_init(&g, NULL, 0);
    unsigned before = k.calls;
    dcon_lines_put(&g, "ab", 2, sink_emit, &k);
    TEST_ASSERT_EQUAL_UINT(before + 1u, k.calls);
}

TEST_CASE("console: other text after a log line fragment starts on its own line", "[diag_console]")
{
    static world_t w;
    world_init(&w, 16u * 1024u, 4096u, false);
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "BENCH a\n", 8));   /* at a line start: no break */
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "", 0));
    log_text(&w, "I (1140) wifi:", 14);                            /* a fragment went out: mid-line */
    TEST_ASSERT_TRUE(w.con.mid_line);
    TEST_ASSERT_TRUE(dcon_other_text(&w.con, "BENCH b\n", 8));
    TEST_ASSERT_FALSE(w.con.mid_line);
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "BENCH c\n", 8));
    log_text(&w, "I (1150) wifi:", 14);
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "\nBENCH d\n", 9));   /* brings its own line break */
    /* A prompt leaves the console at a line start as far as the log is concerned: the typed input's echo
     * ends its line, so the next reply needs no break of its own. */
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "bench> ", 7));
    TEST_ASSERT_FALSE(w.con.mid_line);
    TEST_ASSERT_FALSE(dcon_other_text(&w.con, "BENCH e\n", 8));
    world_free(&w);
}

/* The console as diag_console_write drives it: under the console's lock the
 * whole text goes to the UART (waiting for room as the driver would), a line
 * break first when a log line was left unfinished. */
static world_t *s_cw;

static void console_write_model(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    world_t *w = s_cw;
    if (dcon_other_text(&w->con, s, len)) {
        while (m_room(&w->uart) < 1u) {
            m_drain(&w->uart, 115);
        }
        m_write(&w->uart, "\n", 1);
    }
    size_t wire = dcon_wire_len(s, len, w->uart.crlf);
    while (m_room(&w->uart) < wire) {
        m_drain(&w->uart, 115);   /* the write blocks until the driver took it all */
    }
    m_write(&w->uart, s, len);
}

/* Every occurrence of `line` (with its line end) in the output starts a line. */
static size_t whole_lines(const char *out, size_t out_len, const char *line)
{
    size_t n = 0;
    size_t ll = strlen(line);
    for (size_t i = 0; i + ll <= out_len; i++) {
        if (memcmp(out + i, line, ll) == 0 && (i == 0 || out[i - 1u] == '\n')) {
            n++;
        }
    }
    return n;
}

TEST_CASE("console: a reply printed in pieces while lines are logged reaches the console line by line, whole",
          "[diag_console]")
{
    static const char *const pieces[] = {
        "BENCH state 3 (streaming), receiver ", "UM982 R4.10Build18975", ", journal none (state 0 none, ",
        "phase 0 none, 0 transfer(s)), flashing built in\n", "BENCH progress: phase 0 (idle), 0/0 bytes",
        ", 0/0 frames, 0 retries; last: outcome 0 (none), rule 0, reason none\n",
    };
    static const char *const want[] = {
        "BENCH state 3 (streaming), receiver UM982 R4.10Build18975, journal none (state 0 none, phase 0 none, 0 "
        "transfer(s)), flashing built in\n",
        "BENCH progress: phase 0 (idle), 0/0 bytes, 0/0 frames, 0 retries; last: outcome 0 (none), rule 0, reason "
        "none\n",
    };
    static world_t w;
    for (int gathered = 0; gathered <= 1; gathered++) {
        world_init(&w, 64u * 1024u, 4096u, true);
        s_cw = &w;
        char buf[256];
        dcon_lines_t g;
        dcon_lines_init(&g, buf, sizeof(buf));
        unsigned n = 0;
        for (unsigned round = 0; round < 20; round++) {
            for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
                if (gathered) {
                    dcon_lines_put(&g, pieces[p], strlen(pieces[p]), console_write_model, NULL);
                } else {
                    console_write_model(NULL, pieces[p], strlen(pieces[p]));   /* each piece as it comes */
                }
                log_line(&w, n++, 70);   /* another task's line comes between the pieces */
                if ((n % 7u) == 0) {
                    m_drain(&w.uart, 400);
                    (void)writer(&w, NULL);
                }
            }
            if (gathered) {
                dcon_lines_flush(&g, true, console_write_model, NULL);
            }
        }
        run_until_idle(&w, NULL);
        size_t whole0 = whole_lines(w.uart.out, w.uart.out_len, want[0]);
        size_t whole1 = whole_lines(w.uart.out, w.uart.out_len, want[1]);
        if (gathered) {
            TEST_ASSERT_EQUAL_size_t(20, whole0);
            TEST_ASSERT_EQUAL_size_t(20, whole1);
            /* ... and every log line whole, in order: take the replies out and the log is what was logged. */
            static char rest[OUT_MAX];
            size_t rl = 0;
            for (size_t i = 0; i < w.uart.out_len;) {
                size_t l0 = strlen(want[0]);
                size_t l1 = strlen(want[1]);
                if (i + l0 <= w.uart.out_len && memcmp(w.uart.out + i, want[0], l0) == 0) {
                    i += l0;
                } else if (i + l1 <= w.uart.out_len && memcmp(w.uart.out + i, want[1], l1) == 0) {
                    i += l1;
                } else {
                    rest[rl++] = w.uart.out[i++];
                }
            }
            TEST_ASSERT_EQUAL_size_t(w.expect_len, rl);
            TEST_ASSERT_EQUAL_MEMORY(w.expect, rest, rl);
        } else {
            /* Without the gatherer (printf's way, a piece at a time) log lines land inside the replies
             * (except where the log lines waited in the ring meanwhile). */
            TEST_ASSERT_TRUE(whole0 + whole1 < 20u);
        }
        world_free(&w);
    }
}
