/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "diag_journal_core.h"
#include "diag_task_core.h"
#include "local_caster.h"
#include "unity.h"

/* NOR flash in RAM: erase sets 0xFF, a write can only clear bits. A "power
 * cut" makes the operation number `cut_at` fail (a write then lands only
 * half of its bytes) and every later one too, until the next mount. */
typedef struct {
    uint8_t mem[16 * DJ_SECTOR_SIZE];
    size_t size;
    uint32_t erases[16];
    uint32_t ops;
    uint32_t cut_at;          /* 0 = never */
    bool dead;
    uint32_t overwrites;      /* writes onto bytes that were not erased */
} nor_t;

static int nor_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    nor_t *f = (nor_t *)ctx;
    TEST_ASSERT_TRUE(off + len <= f->size);
    memcpy(buf, f->mem + off, len);
    return 0;
}

static bool nor_cut(nor_t *f)
{
    f->ops++;
    if (f->cut_at != 0 && f->ops >= f->cut_at) {
        f->dead = true;
    }
    return f->dead;
}

static int nor_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    nor_t *f = (nor_t *)ctx;
    TEST_ASSERT_TRUE(off + len <= f->size);
    bool cut = nor_cut(f);
    size_t n = cut ? len / 2u : len;
    for (size_t i = 0; i < n; i++) {
        if (f->mem[off + i] != 0xFFu) {
            f->overwrites++;
        }
        f->mem[off + i] &= ((const uint8_t *)buf)[i];
    }
    return cut ? -1 : 0;
}

static int nor_erase(void *ctx, uint32_t off, size_t len)
{
    nor_t *f = (nor_t *)ctx;
    TEST_ASSERT_EQUAL_UINT32(0, off % DJ_SECTOR_SIZE);
    TEST_ASSERT_EQUAL_size_t(DJ_SECTOR_SIZE, len);
    if (nor_cut(f)) {
        memset(f->mem + off, 0x5A, len / 3u);   /* half-erased garbage */
        return -1;
    }
    memset(f->mem + off, 0xFF, len);
    f->erases[off / DJ_SECTOR_SIZE]++;
    return 0;
}

static nor_t s_nor;

static void nor_reset(uint32_t sectors)
{
    memset(&s_nor, 0, sizeof(s_nor));
    s_nor.size = sectors * DJ_SECTOR_SIZE;
    memset(s_nor.mem, 0xFF, s_nor.size);
}

static void mount(dj_t *j)
{
    s_nor.dead = false;
    s_nor.cut_at = 0;
    dj_flash_t fl = { .read = nor_read, .write = nor_write, .erase = nor_erase, .ctx = &s_nor };
    TEST_ASSERT_EQUAL_INT(DJ_OK, dj_mount(j, &fl, (uint32_t)s_nor.size));
}

static int append(dj_t *j, uint16_t kind, int32_t a)
{
    dj_entry_t e = { .uptime_s = (uint32_t)a, .unix_s = 1760000000 + a, .kind = kind, .a = a, .b = -a };
    return dj_append(j, &e);
}

/* Read everything; check the records are consecutive from `first` and match append(). */
static uint32_t read_check(dj_t *j, uint32_t first, uint32_t expect_count)
{
    static dj_entry_t out[2048];
    uint32_t cur = 0;
    bool more = false;
    size_t n = dj_read(j, &cur, out, 2048, &more);
    TEST_ASSERT_FALSE(more);
    TEST_ASSERT_EQUAL_UINT32(expect_count, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(first + i, out[i].seq);
        TEST_ASSERT_EQUAL_INT32(-out[i].a, out[i].b);
        TEST_ASSERT_EQUAL_INT64(1760000000 + out[i].a, out[i].unix_s);
    }
    return (uint32_t)n;
}

static dj_t s_j;

TEST_CASE("journal codecs: record and sector header round trip, free and damaged slots", "[diag][journal]")
{
    dj_entry_t e = { .seq = 77, .uptime_s = 3, .unix_s = 0, .kind = 10, .a = -1, .b = INT32_MIN };
    uint8_t buf[DJ_SLOT_SIZE];
    dj_encode_record(&e, buf);
    dj_entry_t d;
    TEST_ASSERT_EQUAL_INT(1, dj_decode_record(buf, &d));
    TEST_ASSERT_EQUAL_UINT32(77, d.seq);
    TEST_ASSERT_EQUAL_UINT32(3, d.uptime_s);
    TEST_ASSERT_EQUAL_INT64(0, d.unix_s);
    TEST_ASSERT_EQUAL_UINT16(10, d.kind);
    TEST_ASSERT_EQUAL_INT32(-1, d.a);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, d.b);

    e.unix_s = 4102444800LL;   /* 2100: needs more than 32 bits */
    dj_encode_record(&e, buf);
    TEST_ASSERT_EQUAL_INT(1, dj_decode_record(buf, &d));
    TEST_ASSERT_EQUAL_INT64(4102444800LL, d.unix_s);

    buf[9] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-1, dj_decode_record(buf, &d));
    memset(buf, 0xFF, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, dj_decode_record(buf, &d));

    dj_sector_t s;
    dj_encode_sector_hdr(9, 1000, DJ_REC_FORMAT, buf);
    TEST_ASSERT_EQUAL_INT(1, dj_decode_sector_hdr(buf, &s));
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_EQUAL_UINT32(9, s.sector_seq);
    TEST_ASSERT_EQUAL_UINT32(1000, s.first_seq);
    TEST_ASSERT_EQUAL_UINT16(DJ_REC_FORMAT, s.rec_format);
    buf[12] ^= 0x80;
    TEST_ASSERT_EQUAL_INT(-1, dj_decode_sector_hdr(buf, &s));
    TEST_ASSERT_FALSE(s.valid);
}

TEST_CASE("journal: empty partition, append, read back, continue after a remount", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    TEST_ASSERT_EQUAL_INT32(-1, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(0, dj_oldest_seq(&s_j));
    TEST_ASSERT_TRUE(dj_needs_open(&s_j));
    uint32_t cur = 0;
    dj_entry_t out[4];
    TEST_ASSERT_EQUAL_size_t(0, dj_read(&s_j, &cur, out, 4, NULL));

    for (int i = 1; i <= 3; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    TEST_ASSERT_FALSE(dj_needs_open(&s_j));
    read_check(&s_j, 1, 3);
    TEST_ASSERT_EQUAL_UINT32(1, s_nor.erases[0]);   /* erased before use, blank or not */

    mount(&s_j);
    TEST_ASSERT_EQUAL_INT32(0, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(4, s_j.cur_slot);
    TEST_ASSERT_EQUAL_UINT32(4, s_j.next_seq);
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 2, 4));
    read_check(&s_j, 1, 4);
    TEST_ASSERT_EQUAL_UINT32(1, dj_oldest_seq(&s_j));
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
}

TEST_CASE("journal: wraps one sector at a time, every sector erased alike", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    TEST_ASSERT_EQUAL_UINT32(4 * DJ_RECS_PER_SECTOR, dj_capacity(&s_j));
    const uint32_t total = DJ_RECS_PER_SECTOR * 4u * 5u + 50u;
    for (uint32_t i = 1; i <= total; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, (int32_t)i));
        if (i % 97u == 0) {
            mount(&s_j);   /* remounts anywhere in the ring change nothing */
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
    /* three full sectors plus the 50 records of the current one */
    uint32_t held = 3u * DJ_RECS_PER_SECTOR + 50u;
    read_check(&s_j, total - held + 1u, held);
    TEST_ASSERT_EQUAL_UINT32(total - held + 1u, dj_oldest_seq(&s_j));
    uint32_t lo = UINT32_MAX;
    uint32_t hi = 0;
    for (int k = 0; k < 4; k++) {
        lo = (s_nor.erases[k] < lo) ? s_nor.erases[k] : lo;
        hi = (s_nor.erases[k] > hi) ? s_nor.erases[k] : hi;
    }
    TEST_ASSERT_TRUE(hi - lo <= 1u);
    TEST_ASSERT_EQUAL_UINT32(21u, s_j.top_sector_seq);   /* 20 sectors filled, the 21st open */
}

TEST_CASE("journal: reads in batches with a cursor; a stale cursor starts over", "[diag][journal]")
{
    nor_reset(3);
    mount(&s_j);
    for (int i = 1; i <= 300; i++) {
        append(&s_j, 3, i);
    }
    dj_entry_t out[64];
    uint32_t cur = 0;
    uint32_t seen = 0;
    bool more = true;
    uint32_t first = dj_oldest_seq(&s_j);
    while (more) {
        size_t n = dj_read(&s_j, &cur, out, 64, &more);
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_UINT32(first + seen, out[i].seq);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(300u - first + 1u, seen);
    TEST_ASSERT_EQUAL_UINT32(300, cur);
    TEST_ASSERT_EQUAL_size_t(0, dj_read(&s_j, &cur, out, 64, &more));

    /* a cursor from before the journal was started over */
    cur = 100000;
    size_t n = dj_read(&s_j, &cur, out, 64, &more);
    TEST_ASSERT_EQUAL_size_t(64, n);
    TEST_ASSERT_EQUAL_UINT32(first, out[0].seq);
    TEST_ASSERT_TRUE(more);
}

TEST_CASE("journal: a power cut during a record write leaves one skipped slot", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 5; i++) {
        append(&s_j, 1, i);
    }
    s_nor.cut_at = s_nor.ops + 1u;
    TEST_ASSERT_EQUAL_INT(DJ_ERR_IO, append(&s_j, 1, 6));
    mount(&s_j);
    TEST_ASSERT_EQUAL_UINT32(1, s_j.bad_slots);
    TEST_ASSERT_EQUAL_UINT32(7, s_j.cur_slot);
    TEST_ASSERT_EQUAL_UINT32(6, s_j.next_seq);
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 6));
    read_check(&s_j, 1, 6);
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
}

TEST_CASE("journal: a power cut while opening a sector is repaired on the next write", "[diag][journal]")
{
    for (uint32_t cut = 1; cut <= 3; cut++) {
        nor_reset(3);
        mount(&s_j);
        /* fill all three sectors; the next append wraps to sector 0 */
        for (uint32_t i = 1; i <= 3u * DJ_RECS_PER_SECTOR; i++) {
            append(&s_j, 1, (int32_t)i);
        }
        uint32_t ops = s_nor.ops;
        /* the next append opens sector 0: erase, header write, record write */
        s_nor.cut_at = ops + cut;
        (void)append(&s_j, 1, (int32_t)(3u * DJ_RECS_PER_SECTOR + 1u));
        mount(&s_j);
        /* cut before the header was complete: sector 2 is still the newest;
         * cut in the record write: sector 0 is open, its first slot damaged */
        TEST_ASSERT_EQUAL_INT32(cut < 3u ? 2 : 0, s_j.cur);
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, (int32_t)(3u * DJ_RECS_PER_SECTOR + 1u)));
        TEST_ASSERT_EQUAL_INT32(0, s_j.cur);
        read_check(&s_j, DJ_RECS_PER_SECTOR + 1u, 2u * DJ_RECS_PER_SECTOR + 1u);
        TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
    }
}

TEST_CASE("journal: a sector that reads blank after a cut erase is erased again before use", "[diag][journal]")
{
    nor_reset(3);
    mount(&s_j);
    for (uint32_t i = 1; i <= 3u * DJ_RECS_PER_SECTOR; i++) {
        append(&s_j, 1, (int32_t)i);
    }
    /* The ring is full; the next append opens sector 0 again. Its erase was
     * cut by a power loss in a way that left it reading blank. */
    memset(s_nor.mem, 0xFF, DJ_SECTOR_SIZE);
    mount(&s_j);
    uint32_t before = s_nor.erases[0];
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, (int32_t)(3u * DJ_RECS_PER_SECTOR + 1u)));
    TEST_ASSERT_EQUAL_INT32(0, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, s_nor.erases[0]);
    read_check(&s_j, DJ_RECS_PER_SECTOR + 1u, 2u * DJ_RECS_PER_SECTOR + 1u);
}

TEST_CASE("journal: a sector in another record format keeps its place but is not read", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 10; i++) {
        append(&s_j, 1, i);
    }
    /* A newer firmware opened sector 1 with records this one cannot read. */
    uint8_t hdr[DJ_SLOT_SIZE];
    dj_encode_sector_hdr(2, 11, DJ_REC_FORMAT + 1u, hdr);
    memcpy(s_nor.mem + DJ_SECTOR_SIZE, hdr, sizeof(hdr));
    memset(s_nor.mem + DJ_SECTOR_SIZE + DJ_SLOT_SIZE, 0x11, 10u * DJ_SLOT_SIZE);
    mount(&s_j);
    TEST_ASSERT_EQUAL_INT32(1, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(DJ_SLOTS, s_j.cur_slot);           /* counts as full */
    TEST_ASSERT_EQUAL_UINT32(11u + DJ_RECS_PER_SECTOR, s_j.next_seq);
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 1000));
    TEST_ASSERT_EQUAL_INT32(2, s_j.cur);
    dj_entry_t out[32];
    uint32_t cur = 0;
    size_t n = dj_read(&s_j, &cur, out, 32, NULL);
    TEST_ASSERT_EQUAL_size_t(11, n);
    TEST_ASSERT_EQUAL_UINT32(10, out[9].seq);
    TEST_ASSERT_EQUAL_UINT32(11u + DJ_RECS_PER_SECTOR, out[10].seq);
}

TEST_CASE("journal: a partition full of garbage is started over", "[diag][journal]")
{
    nor_reset(4);
    srand(77);
    for (size_t i = 0; i < s_nor.size; i++) {
        s_nor.mem[i] = (uint8_t)rand();
    }
    mount(&s_j);
    TEST_ASSERT_EQUAL_INT32(-1, s_j.cur);
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 1));
    TEST_ASSERT_EQUAL_UINT32(1, s_nor.erases[0]);
    read_check(&s_j, 1, 1);
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);

    dj_flash_t fl = { .read = nor_read, .write = nor_write, .erase = nor_erase, .ctx = &s_nor };
    TEST_ASSERT_EQUAL_INT(DJ_ERR_SIZE, dj_mount(&s_j, &fl, DJ_SECTOR_SIZE + 1u));
    TEST_ASSERT_EQUAL_INT(DJ_ERR_SIZE, dj_mount(&s_j, &fl, DJ_SECTOR_SIZE));
    dj_entry_t e = { 0 };
    TEST_ASSERT_EQUAL_INT(DJ_ERR_STATE, dj_append(&s_j, &e));
}

/* Every record readable from cursor 0: strictly increasing, never a number twice; returns the count, *last the newest. */
static uint32_t read_rising(dj_t *j, uint32_t *last)
{
    static dj_entry_t out[2048];
    uint32_t cur = 0;
    size_t n = dj_read(j, &cur, out, 2048, NULL);
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_TRUE(out[i].seq > out[i - 1].seq);
    }
    *last = n ? out[n - 1].seq : 0;
    return (uint32_t)n;
}

static int clear_all(dj_t *j)
{
    int r = dj_clear_start(j);
    if (r != DJ_OK) {
        return r;
    }
    int steps = 0;
    int left;
    while ((left = dj_clear_step(j)) != 0) {
        steps++;
        TEST_ASSERT_TRUE(steps <= (int)DJ_MAX_SECTORS);
    }
    return DJ_OK;
}

TEST_CASE("journal clear: every record gone at once, numbering goes on, every sector erased once", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 300; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    TEST_ASSERT_EQUAL_INT32(2, s_j.cur);
    uint32_t before[4];
    memcpy(before, s_nor.erases, sizeof(before));
    uint32_t reader = 250;   /* a reader in the middle of the records */

    TEST_ASSERT_EQUAL_INT(DJ_OK, dj_clear_start(&s_j));
    /* Gone from every read at once, before any old sector is erased. */
    TEST_ASSERT_EQUAL_INT32(3, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(301, s_j.next_seq);
    TEST_ASSERT_EQUAL_UINT32(0, dj_oldest_seq(&s_j));
    uint32_t last = 0;
    TEST_ASSERT_EQUAL_UINT32(0, read_rising(&s_j, &last));
    dj_entry_t out[8];
    TEST_ASSERT_EQUAL_size_t(0, dj_read(&s_j, &reader, out, 8, NULL));
    TEST_ASSERT_EQUAL_UINT32(250, reader);
    TEST_ASSERT_EQUAL_UINT32(before[0], s_nor.erases[0]);

    int steps = 0;
    while (dj_clear_step(&s_j) != 0) {
        steps++;
    }
    TEST_ASSERT_EQUAL_INT(2, steps);   /* three old sectors: two steps say more follow, the third ends it */
    for (int k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(before[k] + 1u, s_nor.erases[k]);
    }
    TEST_ASSERT_EQUAL_INT(0, dj_clear_step(&s_j));
    /* top + the sector count: the newest number over the sector count still counts each sector's erases */
    TEST_ASSERT_EQUAL_UINT32(3u + 4u, s_j.top_sector_seq);

    /* The next record goes on from the old numbering; the old cursor finds it. */
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 301));
    TEST_ASSERT_EQUAL_size_t(1, dj_read(&s_j, &reader, out, 8, NULL));
    TEST_ASSERT_EQUAL_UINT32(301, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(301, dj_oldest_seq(&s_j));
    mount(&s_j);
    TEST_ASSERT_EQUAL_INT32(3, s_j.cur);
    TEST_ASSERT_EQUAL_UINT32(302, s_j.next_seq);
    read_check(&s_j, 301, 1);
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);

    /* A journal never written to has nothing to clear. */
    nor_reset(4);
    mount(&s_j);
    TEST_ASSERT_EQUAL_INT(DJ_OK, clear_all(&s_j));
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.ops);
    TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 1));
    read_check(&s_j, 1, 1);
}

TEST_CASE("journal clear: a sector the ring opens again during the clear keeps its new records", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 300; i++) {
        append(&s_j, 1, i);
    }
    TEST_ASSERT_EQUAL_INT(DJ_OK, dj_clear_start(&s_j));
    /* 128 records: the new sector (3) fills, the last opens sector 0, which the clear still had to erase */
    for (int i = 301; i <= 428; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    TEST_ASSERT_EQUAL_INT32(0, s_j.cur);
    uint32_t e0 = s_nor.erases[0];
    while (dj_clear_step(&s_j) != 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(e0, s_nor.erases[0]);
    read_check(&s_j, 301, 128);
    mount(&s_j);
    read_check(&s_j, 301, 128);
    TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
}

TEST_CASE("journal clear: a power cut at any point leaves a ring that numbers on", "[diag][journal]")
{
    /* ops of a clear of three used sectors out of four: erase and header of the new sector, then three erases */
    for (uint32_t cut = 1; cut <= 6; cut++) {
        nor_reset(4);
        mount(&s_j);
        for (int i = 1; i <= 300; i++) {
            append(&s_j, 1, i);
        }
        s_nor.cut_at = s_nor.ops + cut;
        if (dj_clear_start(&s_j) == DJ_OK) {
            while (dj_clear_step(&s_j) != 0) {
            }
        }
        mount(&s_j);
        TEST_ASSERT_EQUAL_UINT32(301, s_j.next_seq);
        uint32_t last = 0;
        uint32_t n = read_rising(&s_j, &last);
        if (cut <= 2) {
            /* the new header never made it: nothing else changed */
            TEST_ASSERT_EQUAL_INT32(2, s_j.cur);
            TEST_ASSERT_EQUAL_UINT32(300, n);
        } else {
            /* the new sector is current; the sectors not erased yet came back with their older records */
            TEST_ASSERT_EQUAL_INT32(3, s_j.cur);
            TEST_ASSERT_TRUE(n < 300u);
        }
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, 301));
        TEST_ASSERT_EQUAL_UINT32(n + 1u, read_rising(&s_j, &last));
        TEST_ASSERT_EQUAL_UINT32(301, last);
        TEST_ASSERT_EQUAL_UINT32(0, s_nor.overwrites);
    }
}

static dj_queue_t s_q;

static void queue(dj_queue_t *q, int32_t a)
{
    dj_entry_t e = { .uptime_s = (uint32_t)a, .unix_s = 1760000000 + a, .kind = 1, .a = a, .b = -a };
    TEST_ASSERT_TRUE(dj_queue_push(q, &e));
}

/* The writer: everything queued goes to the journal, oldest first. */
static uint32_t write_queued(dj_t *j, dj_queue_t *q)
{
    uint32_t n = 0;
    dj_entry_t e;
    while (dj_queue_pop(q, &e)) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, dj_append(j, &e));
        n++;
    }
    return n;
}

TEST_CASE("journal queue: oldest first, full at its length, round the ring", "[diag][journal]")
{
    dj_queue_reset(&s_q);
    dj_entry_t e;
    TEST_ASSERT_FALSE(dj_queue_pop(&s_q, &e));
    for (int round = 0; round < 3; round++) {
        for (uint32_t i = 0; i < DJ_QUEUE_LEN; i++) {
            queue(&s_q, (int32_t)(round * 1000 + (int)i));
        }
        dj_entry_t extra = { .a = -1 };
        TEST_ASSERT_FALSE(dj_queue_push(&s_q, &extra));   /* full: refused, nothing overwritten */
        TEST_ASSERT_EQUAL_UINT32(DJ_QUEUE_LEN, s_q.count);
        for (uint32_t i = 0; i < DJ_QUEUE_LEN - 5u; i++) {
            TEST_ASSERT_TRUE(dj_queue_pop(&s_q, &e));
            TEST_ASSERT_EQUAL_INT32(round * 1000 + (int)i, e.a);
        }
        for (uint32_t i = DJ_QUEUE_LEN - 5u; i < DJ_QUEUE_LEN; i++) {
            TEST_ASSERT_TRUE(dj_queue_pop(&s_q, &e));
            TEST_ASSERT_EQUAL_INT32(round * 1000 + (int)i, e.a);
        }
        TEST_ASSERT_FALSE(dj_queue_pop(&s_q, &e));
        queue(&s_q, 7);   /* the head moves on: the next round wraps */
        TEST_ASSERT_TRUE(dj_queue_pop(&s_q, &e));
    }
}

TEST_CASE("journal clear: a start that fails clears nothing and keeps the queued records", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 300; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    dj_queue_reset(&s_q);
    for (int i = 301; i <= 303; i++) {
        queue(&s_q, i);   /* written before the clear, still waiting for the writer */
    }
    uint32_t queued = s_q.count;
    s_nor.cut_at = s_nor.ops + 1u;   /* the erase of the sector the clear opens first fails */
    int r = dj_clear_start(&s_j);
    TEST_ASSERT_EQUAL_INT(DJ_ERR_IO, r);
    dj_queue_clear_started(&s_q, queued, r);
    /* Nothing was cleared: the records stay, the queued ones are written as usual. */
    TEST_ASSERT_EQUAL_UINT32(3, s_q.count);
    s_nor.dead = false;
    s_nor.cut_at = 0;
    TEST_ASSERT_EQUAL_UINT32(3, write_queued(&s_j, &s_q));
    read_check(&s_j, 1, 303);
    mount(&s_j);
    read_check(&s_j, 1, 303);
}

TEST_CASE("journal clear: a start that fails on a full ring loses only the sector the ring drops next", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 600; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    /* Full: the next sector to open holds the oldest 127 records. */
    uint32_t oldest = dj_oldest_seq(&s_j);
    TEST_ASSERT_TRUE(oldest > 1u);
    read_check(&s_j, oldest, 600u - oldest + 1u);
    s_nor.cut_at = s_nor.ops + 1u;   /* the erase of the sector the clear opens first fails */
    TEST_ASSERT_EQUAL_INT(DJ_ERR_IO, dj_clear_start(&s_j));
    s_nor.dead = false;
    s_nor.cut_at = 0;
    /* Not cleared: the rest stays, only that sector's records are gone, as
     * when the ring moves on (its cells hold no defined contents now). */
    TEST_ASSERT_EQUAL_UINT32(oldest + 127u, dj_oldest_seq(&s_j));
    read_check(&s_j, oldest + 127u, 600u - oldest - 126u);
    /* The records go on with their numbers; opening that sector again erases it. */
    for (int i = 601; i <= 800; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    uint32_t now_oldest = dj_oldest_seq(&s_j);
    read_check(&s_j, now_oldest, 800u - now_oldest + 1u);
    mount(&s_j);
    TEST_ASSERT_EQUAL_UINT32(now_oldest, dj_oldest_seq(&s_j));
    read_check(&s_j, now_oldest, 800u - now_oldest + 1u);
}

TEST_CASE("journal clear: one that starts takes the records queued before it, not those queued since", "[diag][journal]")
{
    nor_reset(4);
    mount(&s_j);
    for (int i = 1; i <= 300; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_OK, append(&s_j, 1, i));
    }
    dj_queue_reset(&s_q);
    for (int i = 0; i < (int)DJ_QUEUE_LEN - 2; i++) {
        queue(&s_q, 1000 + i);   /* round the ring: the head is not at 0 */
    }
    write_queued(&s_j, &s_q);
    for (int i = 301; i <= 303; i++) {
        queue(&s_q, 2000 + i);
    }
    uint32_t queued = s_q.count;
    int r = dj_clear_start(&s_j);
    TEST_ASSERT_EQUAL_INT(DJ_OK, r);
    queue(&s_q, 1);   /* appended while the clear started: after it */
    queue(&s_q, 2);
    dj_queue_clear_started(&s_q, queued, r);
    TEST_ASSERT_EQUAL_UINT32(2, s_q.count);
    while (dj_clear_step(&s_j) != 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(2, write_queued(&s_j, &s_q));
    static dj_entry_t out[8];
    uint32_t cur = 0;
    TEST_ASSERT_EQUAL_size_t(2, dj_read(&s_j, &cur, out, 8, NULL));
    TEST_ASSERT_EQUAL_INT32(1, out[0].a);
    TEST_ASSERT_EQUAL_INT32(2, out[1].a);
    TEST_ASSERT_EQUAL_UINT32(300 + DJ_QUEUE_LEN - 2u + 1u, out[0].seq);   /* numbering goes on */
}

TEST_CASE("journal rate limit: per kind, exempt kinds, across the clock wrap", "[diag][journal]")
{
    dj_rate_t r;
    dj_rate_init(&r, 1000, 0, 1u << 3);
    uint32_t t = UINT32_MAX - 500u;
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 9, t));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check(&r, 9, t + 999u));   /* wrapped */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 2, t + 999u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 9, t + 1000u));
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 3, t + 1000u));   /* exempt */
    }
    TEST_ASSERT_EQUAL_UINT32(1, r.refused_kind);
    /* kinds beyond the table share one entry */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 200, t + 5000u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check(&r, 300, t + 5001u));
}

TEST_CASE("journal rate limit: hourly budget with a ten-minute burst", "[diag][journal]")
{
    dj_rate_t r;
    dj_rate_init(&r, 0, 600, 1u << 3);   /* 600/h = one per 6 s; burst 100 */
    uint32_t t = 12345;
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 1, t));
    }
    TEST_ASSERT_EQUAL_INT(DJ_RATE_BUDGET, dj_rate_check(&r, 1, t));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 3, t));            /* exempt still passes */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_BUDGET, dj_rate_check(&r, 2, t + 5999u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check(&r, 2, t + 6000u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_BUDGET, dj_rate_check(&r, 2, t + 6001u));
    /* refills in many small steps add up exactly (no rounding drift) */
    uint32_t ok = 0;
    for (uint32_t ms = 6001u; ms <= 6001u + 60000u; ms += 5u) {
        ok += dj_rate_check(&r, 4, t + ms) == DJ_RATE_OK;
    }
    TEST_ASSERT_EQUAL_UINT32(10, ok);
    TEST_ASSERT_TRUE(r.refused_budget > 0);
    /* a long pause refills to the burst, not beyond */
    t += 10u * 3600u * 1000u;
    uint32_t burst = 0;
    while (dj_rate_check(&r, 1, t) == DJ_RATE_OK) {
        burst++;
    }
    TEST_ASSERT_EQUAL_UINT32(100, burst);
}

/* The kinds diag_journal.c keys by slot or connection, as it sets them up. */
#define KEYED ((1u << DIAG_EV_CASTER_DOWN) | (1u << DIAG_EV_CASTER_UP) | (1u << DIAG_EV_CASTER_STOP) | \
               (1u << DIAG_EV_ROVER_UP) | (1u << DIAG_EV_ROVER_DOWN))
#define EXEMPT ((1u << DIAG_EV_LADDER_STEP) | (1u << DIAG_EV_GNSS_FLASH) | (1u << DIAG_EV_RX_BOOT))

TEST_CASE("journal rate limit: per kind alone, a restart keeps one caster_down and one rover_disconnect",
          "[diag][journal]")
{
    /* The interval as it was (per kind only): four caster slots and two
     * rovers end in the same restart, within a few milliseconds. */
    dj_rate_t r;
    dj_rate_init(&r, 1000, 1800, EXEMPT);
    uint32_t t = 50000;
    uint32_t ok = 0;
    for (uint32_t slot = 0; slot < 4; slot++) {
        ok += dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(slot, 0, 0), t + slot) == DJ_RATE_OK;
    }
    TEST_ASSERT_EQUAL_UINT32(1, ok);   /* slots 1-3 are refused, and the restart follows before 1 s */
    ok = 0;
    for (uint32_t conn = 0; conn < 2; conn++) {
        ok += dj_rate_check_rec(&r, DIAG_EV_ROVER_DOWN, diag_rover_down_a(conn, LOCAL_CASTER_END_RESTART),
                                t + 10u + conn) == DJ_RATE_OK;
    }
    TEST_ASSERT_EQUAL_UINT32(1, ok);
    TEST_ASSERT_EQUAL_UINT32(4, r.refused_kind);
}

TEST_CASE("journal rate limit: caster and rover kinds hold the interval per slot or connection", "[diag][journal]")
{
    dj_rate_t r;
    dj_rate_init(&r, 1000, 1800, EXEMPT);
    dj_rate_key_kinds(&r, KEYED);
    uint32_t t = UINT32_MAX - 300u;   /* across the clock wrap */
    for (uint32_t slot = 0; slot < 8; slot++) {
        TEST_ASSERT_EQUAL_INT(DJ_RATE_OK,
                              dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(slot, 0, slot), t + slot));
    }
    for (uint32_t conn = 0; conn < LOCAL_CASTER_MAX_CONNS; conn++) {
        TEST_ASSERT_EQUAL_INT(DJ_RATE_OK,
                              dj_rate_check_rec(&r, DIAG_EV_ROVER_DOWN,
                                                diag_rover_down_a(conn, LOCAL_CASTER_END_RESTART), t + 20u));
    }
    /* the same slot or connection again within the interval: refused, as before */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(3, 2, 0), t + 500u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check_rec(&r, DIAG_EV_ROVER_DOWN,
                                                          diag_rover_down_a(1, LOCAL_CASTER_END_CLOSED), t + 999u));
    /* the interval runs from the record that opened it (slot 0 at t), not from the later slots'; the
     * next one opens a new interval with only its own slot in it */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(3, 2, 0), t + 1000u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(3, 0, 0), t + 1001u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_DOWN, diag_caster_a(7, 0, 0), t + 1001u));
    /* an up and a stop of other slots are their own kinds, keyed the same way */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_UP, diag_caster_a(0, 0, 1), t + 1002u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_UP, diag_caster_a(1, 0, 1), t + 1002u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_STOP, 2 | (1 << 8), t + 1002u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_CASTER_STOP, 3 | (1 << 8), t + 1002u));

    /* every other kind keeps one record per interval, whatever its `a` */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_WIFI_UP, 1, t + 2000u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check_rec(&r, DIAG_EV_WIFI_UP, 2, t + 2001u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_DATA_LOSS, 3, t + 2001u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_KIND, dj_rate_check_rec(&r, DIAG_EV_DATA_LOSS, 4, t + 2002u));
    /* exempt kinds pass, keyed or not */
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_LADDER_STEP, 0, t + 2003u));
    TEST_ASSERT_EQUAL_INT(DJ_RATE_OK, dj_rate_check_rec(&r, DIAG_EV_LADDER_STEP, 0, t + 2003u));

    /* the hourly budget still counts every keyed record */
    dj_rate_init(&r, 1000, 60, 0);   /* burst 10 */
    dj_rate_key_kinds(&r, KEYED);
    uint32_t ok = 0;
    for (uint32_t conn = 0; conn < 16; conn++) {
        ok += dj_rate_check_rec(&r, DIAG_EV_ROVER_UP, diag_rover_up_a(conn, 2, false, 10, 0), 7000u) == DJ_RATE_OK;
    }
    TEST_ASSERT_EQUAL_UINT32(10, ok);
    TEST_ASSERT_EQUAL_UINT32(6, r.refused_budget);
}

TEST_CASE("task CPU shares from run-time counters, two cores", "[diag]")
{
    dts_state_t st;
    memset(&st, 0, sizeof(st));
    dts_sample_t s[3] = { { 1, 1000 }, { 2, 3000 }, { 3, 0 } };
    uint8_t pct[3];
    /* first snapshot: since boot (now = 10 000, two cores = 20 000) */
    dts_update(&st, s, 3, 10000, 2, pct);
    TEST_ASSERT_EQUAL_UINT8(5, pct[0]);
    TEST_ASSERT_EQUAL_UINT8(15, pct[1]);
    TEST_ASSERT_EQUAL_UINT8(0, pct[2]);

    /* task 2 busies one core for the whole next interval; task 3 is gone, 4 is new */
    dts_sample_t t[3] = { { 1, 1000 }, { 2, 13000 }, { 4, 500 } };
    dts_update(&st, t, 3, 20000, 2, pct);
    TEST_ASSERT_EQUAL_UINT8(0, pct[0]);
    TEST_ASSERT_EQUAL_UINT8(50, pct[1]);
    TEST_ASSERT_EQUAL_UINT8(3, pct[2]);   /* 500 of 20 000 */

    /* a counter that went backwards (task number reused) reads 0, never wraps */
    dts_sample_t u[1] = { { 2, 100 } };
    dts_update(&st, u, 1, 30000, 2, pct);
    TEST_ASSERT_EQUAL_UINT8(0, pct[0]);
}
