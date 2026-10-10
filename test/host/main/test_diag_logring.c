/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_crc.h"
#include "diag_logring.h"
#include "unity.h"

#define MEM_SIZE (sizeof(logring_hdr_t) + 512u)

/* What the memory would hold after a reset: only the ranges the ring has
 * synced (the target's cache write-back). */
typedef struct {
    uint8_t *work;
    uint8_t *durable;
    size_t size;
    uint32_t syncs;
    uint32_t hdr_syncs;
} shadow_t;

static void shadow_sync(void *ctx, const void *addr, size_t len)
{
    shadow_t *s = (shadow_t *)ctx;
    size_t off = (size_t)((const uint8_t *)addr - s->work);
    TEST_ASSERT_TRUE(off + len <= s->size);
    memcpy(s->durable + off, addr, len);
    s->syncs++;
    if (off == 0) {
        s->hdr_syncs++;
    }
}

static void line_text(uint32_t n, char *out, size_t cap)
{
    /* varied lengths, so records straddle the end of the data area */
    snprintf(out, cap, "I (%u) test: line %u %.*s\n", (unsigned)n, (unsigned)n, (int)(n % 23u),
             "abcdefghijklmnopqrstuvwxyz");
}

static size_t read_all(logring_t *r, uint32_t *cursor, char *out, size_t cap, logring_read_t *info)
{
    return logring_read(r, cursor, out, cap, info);
}

TEST_CASE("diag checksums match the published check values", "[diag]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, diag_crc32("123456789", 9));
    TEST_ASSERT_EQUAL_HEX16(0x29B1u, diag_crc16_update(DIAG_CRC16_INIT, "123456789", 9));
    uint32_t c = diag_crc32_update(DIAG_CRC32_INIT, "1234", 4);
    c = diag_crc32_update(c, "56789", 5);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, diag_crc32_final(c));
}

TEST_CASE("log ring: append, read with a cursor, nothing new", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    TEST_ASSERT_TRUE(logring_header_ok(&r));
    TEST_ASSERT_EQUAL_UINT32(1, logring_append(&r, "a\n", 2));
    TEST_ASSERT_EQUAL_UINT32(2, logring_append(&r, "bb\n", 3));
    TEST_ASSERT_EQUAL_UINT32(0, logring_append(&r, "", 0));
    TEST_ASSERT_EQUAL_UINT32(2, logring_count(&r));

    char out[64];
    uint32_t cur = 0;
    logring_read_t info;
    TEST_ASSERT_EQUAL_size_t(5, read_all(&r, &cur, out, sizeof(out), &info));
    TEST_ASSERT_EQUAL_STRING("a\nbb\n", out);
    TEST_ASSERT_EQUAL_UINT32(2, cur);
    TEST_ASSERT_FALSE(info.gap);
    TEST_ASSERT_FALSE(info.more);

    TEST_ASSERT_EQUAL_size_t(0, read_all(&r, &cur, out, sizeof(out), &info));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT32(2, cur);

    logring_append(&r, "ccc\n", 4);
    TEST_ASSERT_EQUAL_size_t(4, read_all(&r, &cur, out, sizeof(out), &info));
    TEST_ASSERT_EQUAL_STRING("ccc\n", out);
    TEST_ASSERT_EQUAL_UINT32(3, cur);
}

TEST_CASE("log ring: whole records only, small buffers advance, more is reported", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    logring_append(&r, "first line\n", 11);
    logring_append(&r, "second\n", 7);

    char out[16];
    uint32_t cur = 0;
    logring_read_t info;
    /* 15 bytes of room: the first record fits, the second does not */
    TEST_ASSERT_EQUAL_size_t(11, logring_read(&r, &cur, out, sizeof(out), &info));
    TEST_ASSERT_TRUE(info.more);
    TEST_ASSERT_EQUAL_UINT32(1, cur);
    TEST_ASSERT_EQUAL_size_t(7, logring_read(&r, &cur, out, sizeof(out), &info));
    TEST_ASSERT_FALSE(info.more);

    /* a record longer than the whole buffer is cut so the reader advances */
    char tiny[5];
    cur = 0;
    TEST_ASSERT_EQUAL_size_t(4, logring_read(&r, &cur, tiny, sizeof(tiny), &info));
    TEST_ASSERT_EQUAL_STRING("firs", tiny);
    TEST_ASSERT_EQUAL_UINT32(1, cur);
    TEST_ASSERT_TRUE(info.more);
}

TEST_CASE("log ring: overwriting drops the oldest lines and the reader sees a gap", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 7, 100);
    char text[64];
    char out[600];
    uint32_t cur = 0;
    logring_read_t info;

    for (uint32_t i = 0; i < 5; i++) {
        line_text(i, text, sizeof(text));
        logring_append(&r, text, strlen(text));
    }
    read_all(&r, &cur, out, sizeof(out), &info);
    TEST_ASSERT_EQUAL_UINT32(104, cur);

    /* far more than 512 bytes: the ring wraps several times */
    for (uint32_t i = 5; i < 200; i++) {
        line_text(i, text, sizeof(text));
        TEST_ASSERT_EQUAL_UINT32(100 + i, logring_append(&r, text, strlen(text)));
    }
    TEST_ASSERT_TRUE(r.hdr->lost > 0);
    TEST_ASSERT_TRUE(r.hdr->first_seq > 105);
    size_t n = read_all(&r, &cur, out, sizeof(out), &info);
    TEST_ASSERT_TRUE(info.gap);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT32(299, cur);
    /* the text is the newest lines, in order, ending with the last one */
    line_text(199, text, sizeof(text));
    TEST_ASSERT_EQUAL_STRING(text, out + n - strlen(text));
    line_text(r.hdr->first_seq - 100u, text, sizeof(text));
    TEST_ASSERT_EQUAL_MEMORY(text, out, strlen(text));

    /* a cursor from another boot (beyond the newest) restarts from the oldest */
    cur = 5000;
    n = read_all(&r, &cur, out, sizeof(out), &info);
    TEST_ASSERT_TRUE(info.gap);
    TEST_ASSERT_EQUAL_UINT32(299, cur);
    TEST_ASSERT_EQUAL_MEMORY(text, out, strlen(text));
}

static size_t count_nl(const char *s, size_t n)
{
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c += (s[i] == '\n');
    }
    return c;
}

TEST_CASE("log ring: a sliced read through a large buffer never cuts a line", "[diag][logring]")
{
    /* 200 lines of about 60 bytes read 4 KB at a time in 1025-byte lock
     * slices (the web's log download and WebSocket push): every chunk ends
     * on a whole line and nothing is lost. */
    static uint8_t mem[sizeof(logring_hdr_t) + 65536u];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    static char expect[200 * 80];
    size_t elen = 0;
    for (uint32_t i = 0; i < 200; i++) {
        char text[80];
        snprintf(text, sizeof(text), "I (%05u) tag: line number %u of the test, some words\n", (unsigned)i,
                 (unsigned)i);
        logring_append(&r, text, strlen(text));
        memcpy(expect + elen, text, strlen(text));
        elen += strlen(text);
    }
    static char got[200 * 80];
    size_t glen = 0;
    uint32_t cur = 0;
    char buf[4096];
    for (int guard = 0; guard < 100; guard++) {
        logring_read_t info;
        size_t n = logring_read_sliced(&r, &cur, buf, sizeof(buf), LOGRING_MAX_TEXT + 1u, NULL, NULL, NULL, &info);
        TEST_ASSERT_EQUAL_size_t(strlen(buf), n);
        if (n == 0) {
            TEST_ASSERT_FALSE(info.more);
            break;
        }
        TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
        TEST_ASSERT_EQUAL_UINT32(count_nl(buf, n), info.records);
        memcpy(got + glen, buf, n);
        glen += n;
    }
    TEST_ASSERT_EQUAL_size_t(elen, glen);
    TEST_ASSERT_EQUAL_MEMORY(expect, got, elen);
    TEST_ASSERT_EQUAL_size_t(200, count_nl(got, glen));
    TEST_ASSERT_EQUAL_UINT32(200, cur);

    /* A buffer smaller than a record still advances (that record is cut). */
    char tiny[16];
    cur = 0;
    logring_read_t info;
    TEST_ASSERT_EQUAL_size_t(15, logring_read_sliced(&r, &cur, tiny, sizeof(tiny), 1025, NULL, NULL, NULL, &info));
    TEST_ASSERT_EQUAL_UINT32(1, cur);
}

static unsigned s_locks;
static void test_lock(void *ctx)
{
    TEST_ASSERT_EQUAL_PTR(&s_locks, ctx);
    s_locks++;
}
static void test_unlock(void *ctx)
{
    (void)ctx;
}

TEST_CASE("log ring: a reader that continues where it stopped does not walk the ring", "[diag][logring]")
{
    static uint8_t mem[sizeof(logring_hdr_t) + 65536u];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    char text[80];
    uint32_t cur = 0;
    char buf[4096];
    /* Fill and wrap the ring, a few lines between two reads, as the
     * WebSocket push does every 500 ms. */
    for (uint32_t i = 0; i < 3000; i++) {
        line_text(i, text, sizeof(text));
        logring_append(&r, text, strlen(text));
        if (i % 3 == 2) {
            s_locks = 0;
            logring_read_t info;
            logring_read_sliced(&r, &cur, buf, sizeof(buf), LOGRING_MAX_TEXT + 1u, test_lock, test_unlock, &s_locks,
                                &info);
            TEST_ASSERT_EQUAL_UINT32(1, s_locks);
            TEST_ASSERT_EQUAL_UINT32(i + 1u, cur);
            TEST_ASSERT_EQUAL_size_t(3, count_nl(buf, strlen(buf)));
        }
    }
    TEST_ASSERT_TRUE(r.hdr->lost > 0);
    TEST_ASSERT_EQUAL_UINT32(0, r.seek_steps);

    /* A reader far behind (another cursor) still reads correctly, with a gap. */
    uint32_t old = 5;
    logring_read_t info;
    size_t n = logring_read(&r, &old, buf, sizeof(buf), &info);
    TEST_ASSERT_TRUE(info.gap);
    line_text(r.hdr->first_seq - 1u, text, sizeof(text));
    TEST_ASSERT_EQUAL_MEMORY(text, buf, strlen(text));
    TEST_ASSERT_TRUE(n > 0);
    /* Paging on from there uses the hint the last page left. */
    uint32_t steps = r.seek_steps;
    n = logring_read(&r, &old, buf, sizeof(buf), &info);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT32(steps, r.seek_steps);
    line_text(old - 1u, text, sizeof(text));
    TEST_ASSERT_EQUAL_MEMORY(text, buf + n - strlen(text), strlen(text));
}

TEST_CASE("log ring: random appends and readers with hints read exactly what was written", "[diag][logring]")
{
    static uint8_t mem[sizeof(logring_hdr_t) + 2048u];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    srand(7);
    uint32_t cur[3] = { 0, 0, 0 };
    char buf[700];
    char text[200];
    for (uint32_t i = 0; i < 4000; i++) {
        uint32_t seq = r.hdr->next_seq;
        int len = snprintf(text, sizeof(text), "%u:%.*s\n", (unsigned)seq, rand() % 120,
                           "0123456789012345678901234567890123456789012345678901234567890123456789"
                           "012345678901234567890123456789012345678901234567890123456789");
        logring_append(&r, text, (size_t)len);
        int who = rand() % 4;
        if (who < 3) {
            size_t cap = (size_t)(40 + rand() % 600);
            logring_read_t info;
            size_t n = logring_read_sliced(&r, &cur[who], buf, cap, LOGRING_MAX_TEXT + 1u, NULL, NULL, NULL, &info);
            /* every line read is whole and numbered in order */
            size_t p = 0;
            uint32_t last = 0;
            while (p < n) {
                char *nl = memchr(buf + p, '\n', n - p);
                if (nl == NULL) {
                    /* only a record longer than the whole buffer is cut, alone */
                    TEST_ASSERT_EQUAL_size_t(0, p);
                    TEST_ASSERT_EQUAL_size_t(cap - 1u, n);
                    TEST_ASSERT_EQUAL_UINT32(1, info.records);
                    last = (uint32_t)strtoul(buf, NULL, 10);
                    break;
                }
                uint32_t num = (uint32_t)strtoul(buf + p, NULL, 10);
                if (last != 0) {
                    TEST_ASSERT_EQUAL_UINT32(last + 1u, num);
                }
                last = num;
                p = (size_t)(nl - buf) + 1u;
            }
            if (last != 0) {
                TEST_ASSERT_EQUAL_UINT32(last, cur[who]);
            }
        }
    }
}

TEST_CASE("log ring: a block of another bank or image is refused", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    static const uint8_t img_a[LOGRING_IMAGE_ID] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const uint8_t img_b[LOGRING_IMAGE_ID] = { 1, 2, 3, 4, 5, 6, 7, 9 };
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_set_owner(&r, 1, img_a);
    logring_format(&r, 4, 1);
    logring_append(&r, "x\n", 2);

    logring_t same;
    logring_attach(&same, mem, sizeof(mem), NULL, NULL);
    logring_set_owner(&same, 1, img_a);
    TEST_ASSERT_TRUE(logring_recover(&same));
    TEST_ASSERT_EQUAL_UINT32(1, logring_count(&same));

    /* the same bytes found where bank 0 lives (the image moved them) */
    logring_t other_bank;
    logring_attach(&other_bank, mem, sizeof(mem), NULL, NULL);
    logring_set_owner(&other_bank, 0, img_a);
    TEST_ASSERT_FALSE(logring_recover(&other_bank));

    /* written by another image */
    logring_t other_image;
    logring_attach(&other_image, mem, sizeof(mem), NULL, NULL);
    logring_set_owner(&other_image, 1, img_b);
    TEST_ASSERT_FALSE(logring_recover(&other_image));
}

TEST_CASE("log ring: a long line is cut to what the ring can hold", "[diag][logring]")
{
    static uint8_t mem[sizeof(logring_hdr_t) + 64u];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    char big[200];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL_UINT32(1, logring_append(&r, big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT32(64u, r.hdr->used);
    TEST_ASSERT_EQUAL_UINT32(2, logring_append(&r, "ok\n", 3));
    TEST_ASSERT_EQUAL_UINT32(1, logring_count(&r));
    TEST_ASSERT_TRUE(logring_recover(&r));
    TEST_ASSERT_EQUAL_UINT32(1, logring_count(&r));
}

TEST_CASE("log ring: survives a reset; a damaged tail record ends it", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 3, 1);
    char text[64];
    for (uint32_t i = 0; i < 40; i++) {
        line_text(i, text, sizeof(text));
        logring_append(&r, text, strlen(text));
    }
    uint32_t first = r.hdr->first_seq;
    uint32_t next = r.hdr->next_seq;

    /* the next boot attaches to the same memory */
    logring_t again;
    logring_attach(&again, mem, sizeof(mem), NULL, NULL);
    TEST_ASSERT_TRUE(logring_recover(&again));
    TEST_ASSERT_EQUAL_UINT32(first, again.hdr->first_seq);
    TEST_ASSERT_EQUAL_UINT32(next, again.hdr->next_seq);
    TEST_ASSERT_EQUAL_UINT32(3, again.hdr->boot_id);

    /* damage the newest record's text: it and nothing else is dropped */
    uint32_t head = (again.hdr->tail + again.hdr->used) % again.data_size;
    uint32_t last_text = (head + again.data_size - 2u) % again.data_size;
    again.data[last_text] ^= 0x5Au;
    TEST_ASSERT_TRUE(logring_recover(&again));
    TEST_ASSERT_EQUAL_UINT32(next - 1u, again.hdr->next_seq);
    TEST_ASSERT_TRUE(logring_header_ok(&again));

    /* damage the oldest record's header: nothing usable is left */
    again.data[again.hdr->tail] ^= 0xFFu;
    TEST_ASSERT_TRUE(logring_recover(&again));
    TEST_ASSERT_EQUAL_UINT32(0, logring_count(&again));
    TEST_ASSERT_EQUAL_UINT32(0, again.hdr->used);
}

TEST_CASE("log ring: a header from another layout, size or garbage is discarded", "[diag][logring]")
{
    static uint8_t mem[MEM_SIZE];
    logring_t r;
    logring_attach(&r, mem, sizeof(mem), NULL, NULL);
    logring_format(&r, 1, 1);
    logring_append(&r, "x\n", 2);
    TEST_ASSERT_TRUE(logring_recover(&r));

    /* same memory, seen by an image with a smaller ring */
    logring_t smaller;
    logring_attach(&smaller, mem, sizeof(mem) - 64u, NULL, NULL);
    TEST_ASSERT_FALSE(logring_recover(&smaller));

    /* another layout version, even with a matching CRC */
    r.hdr->layout = LOGRING_LAYOUT + 1u;
    r.hdr->crc = diag_crc32(r.hdr, offsetof(logring_hdr_t, crc));
    TEST_ASSERT_FALSE(logring_recover(&r));

    /* random memory, as after a power cut */
    srand(1234);
    for (size_t i = 0; i < sizeof(mem); i++) {
        mem[i] = (uint8_t)rand();
    }
    TEST_ASSERT_FALSE(logring_recover(&r));

    /* a valid CRC over inconsistent offsets */
    logring_format(&r, 1, 1);
    r.hdr->tail = r.data_size;
    r.hdr->crc = diag_crc32(r.hdr, offsetof(logring_hdr_t, crc));
    TEST_ASSERT_FALSE(logring_recover(&r));
}

/* Check that a ring recovered from `durable` holds a gap-free run of the
 * expected lines. */
static void check_durable(const uint8_t *durable, size_t size, uint32_t first_line_seq)
{
    static uint8_t copy[MEM_SIZE];
    memcpy(copy, durable, size);
    logring_t r;
    logring_attach(&r, copy, size, NULL, NULL);
    if (!logring_recover(&r)) {
        TEST_FAIL_MESSAGE("durable copy not recoverable");
    }
    uint32_t cur = 0;
    char out[1024];
    char text[64];
    logring_read_t info;
    size_t n = logring_read(&r, &cur, out, sizeof(out), &info);
    size_t pos = 0;
    for (uint32_t seq = r.hdr->first_seq; seq < r.hdr->next_seq; seq++) {
        line_text(seq - first_line_seq, text, sizeof(text));
        size_t len = strlen(text);
        TEST_ASSERT_TRUE(pos + len <= n);
        TEST_ASSERT_EQUAL_MEMORY(text, out + pos, len);
        pos += len;
    }
    TEST_ASSERT_EQUAL_size_t(n, pos);
}

TEST_CASE("log ring: every synced state recovers to whole lines (reset at any write-back)", "[diag][logring]")
{
    static uint8_t work[MEM_SIZE];
    static uint8_t durable[MEM_SIZE];
    memset(work, 0xA5, sizeof(work));
    memset(durable, 0xA5, sizeof(durable));
    shadow_t sh = { .work = work, .durable = durable, .size = sizeof(work) };
    logring_t r;
    logring_attach(&r, work, sizeof(work), shadow_sync, &sh);
    logring_format(&r, 1, 1);
    char text[64];
    for (uint32_t i = 0; i < 120; i++) {
        line_text(i, text, sizeof(text));
        uint32_t before = sh.syncs;
        logring_append(&r, text, strlen(text));
        TEST_ASSERT_TRUE(sh.syncs > before);
        /* the last write-back of an append is always the header */
        check_durable(durable, sizeof(durable), 1);
    }
    TEST_ASSERT_TRUE(r.hdr->lost > 0);
}

/* Write-backs that stop after `budget` calls: a reset in the middle of an append. */
typedef struct {
    uint8_t *work;
    uint8_t *durable;
    uint32_t budget;
    uint32_t calls;
} cut_t;

static void cut_sync(void *ctx, const void *addr, size_t len)
{
    cut_t *c = (cut_t *)ctx;
    c->calls++;
    if (c->budget == 0) {
        return;
    }
    c->budget--;
    memcpy(c->durable + ((const uint8_t *)addr - c->work), addr, len);
}

TEST_CASE("log ring: a reset between write-backs of one append keeps whole lines", "[diag][logring]")
{
    static uint8_t work[MEM_SIZE];
    static uint8_t durable[MEM_SIZE];
    static uint8_t saved[MEM_SIZE];
    memset(work, 0, sizeof(work));
    logring_t r;
    logring_attach(&r, work, sizeof(work), NULL, NULL);
    logring_format(&r, 1, 1);
    char text[64];
    /* Full ring: every further append drops old records first. */
    for (uint32_t i = 0; i < 60; i++) {
        line_text(i, text, sizeof(text));
        logring_append(&r, text, strlen(text));
    }
    memcpy(saved, work, sizeof(saved));
    line_text(60, text, sizeof(text));

    cut_t probe = { .work = work, .durable = durable, .budget = UINT32_MAX };
    memcpy(durable, saved, sizeof(durable));
    logring_attach(&r, work, sizeof(work), cut_sync, &probe);
    logring_append(&r, text, strlen(text));
    uint32_t total = probe.calls;
    TEST_ASSERT_TRUE(total >= 3);   /* header (drop), record, header (add) */

    for (uint32_t stop = 0; stop <= total; stop++) {
        memcpy(work, saved, sizeof(work));
        memcpy(durable, saved, sizeof(durable));
        cut_t c = { .work = work, .durable = durable, .budget = stop };
        logring_attach(&r, work, sizeof(work), cut_sync, &c);
        logring_append(&r, text, strlen(text));
        check_durable(durable, sizeof(durable), 1);
    }
}
