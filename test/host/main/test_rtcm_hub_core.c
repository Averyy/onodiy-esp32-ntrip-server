/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "rtcm_hub_core.h"
#include "rtcm_test_frames.h"
#include "unity.h"

#define RING_MAX   (128u * 1024u)
#define GOT_MAX    8192u
#define READ_MAX   16384u

/* ---- fixture ------------------------------------------------------------- */

typedef struct fx fx_t;
struct fx {
    rtcm_core_t core;
    uint8_t ring[RING_MAX];
    uint64_t now;
    uint64_t next_at;        /* earliest start of the next burst */
    uint32_t serial;         /* next frame serial (test-side numbering) */
    uint32_t tow;            /* MSM epoch time of the current burst */
    uint32_t bursts;
    /* per serial / per burst, while below rec_cap */
    uint32_t rec_cap;
    uint64_t *t_pub;
    uint32_t *burst_of;
    uint16_t *type_of;
    uint32_t *burst_first;
    /* lock instrumentation */
    int depth;
    int max_depth;
    bool hook_armed;
    bool in_hook;
    bool hook_fired;
    unsigned unlocks;
    unsigned hook_at;
    void (*hook)(fx_t *x);
};

static void fx_lock(void *ctx)
{
    fx_t *x = ctx;
    x->depth++;
    if (x->depth > x->max_depth) {
        x->max_depth = x->depth;
    }
}

/* The hook runs right after the lock is released at the hook_at-th unlock:
 * the "other core" acts between two steps of the consumer. */
static void fx_unlock(void *ctx)
{
    fx_t *x = ctx;
    x->depth--;
    if (x->hook_armed && !x->in_hook && x->depth == 0 && x->unlocks++ == x->hook_at) {
        x->in_hook = true;
        x->hook(x);
        x->in_hook = false;
        x->hook_fired = true;
    }
}

static fx_t *fx_new(size_t ring_len, unsigned max_consumers, uint32_t default_lag, uint32_t first_epoch,
                    uint32_t rec_cap)
{
    TEST_ASSERT_TRUE(ring_len <= RING_MAX);
    fx_t *x = calloc(1, sizeof(*x));
    TEST_ASSERT_NOT_NULL(x);
    const rtcm_core_cfg_t cfg = {
        .gap_ms = 250,
        .default_lag_ms = default_lag,
        .max_consumers = max_consumers,
        .first_epoch = first_epoch,
    };
    const rtcm_core_lock_t lock = { .lock = fx_lock, .unlock = fx_unlock, .ctx = x };
    TEST_ASSERT_TRUE(rtcm_core_init(&x->core, x->ring, ring_len, &cfg, &lock));
    x->now = 1000000;
    x->tow = 300000;
    x->rec_cap = rec_cap;
    if (rec_cap > 0) {
        x->t_pub = calloc(rec_cap, sizeof(*x->t_pub));
        x->burst_of = calloc(rec_cap, sizeof(*x->burst_of));
        x->type_of = calloc(rec_cap, sizeof(*x->type_of));
        x->burst_first = calloc(rec_cap, sizeof(*x->burst_first));
        TEST_ASSERT_NOT_NULL(x->t_pub);
        TEST_ASSERT_NOT_NULL(x->burst_of);
        TEST_ASSERT_NOT_NULL(x->type_of);
        TEST_ASSERT_NOT_NULL(x->burst_first);
    }
    return x;
}

static void fx_free(fx_t *x)
{
    TEST_ASSERT_EQUAL_UINT64(0, x->core.corrupt);
    TEST_ASSERT_EQUAL_INT(0, x->depth);
    TEST_ASSERT_TRUE(x->max_depth <= 1);   /* the core never nests its lock */
    free(x->t_pub);
    free(x->burst_of);
    free(x->type_of);
    free(x->burst_first);
    free(x);
}

static uint32_t fx_pub(fx_t *x, uint16_t type, size_t plen, bool more)
{
    uint8_t f[GNSS_RTCM3_MAX_FRAME];
    uint32_t serial = x->serial++;
    size_t len = tf_build(f, type, plen, x->tow, more, serial);
    void *wake[RTCM_CORE_MAX_CONSUMERS];
    size_t n = 99;
    TEST_ASSERT_TRUE(rtcm_core_publish(&x->core, f, len, type, x->now, wake, &n));
    if (n > 0) {
        rtcm_core_wake_done(&x->core);
    }
    if (serial < x->rec_cap) {
        x->t_pub[serial] = x->now;
        x->burst_of[serial] = x->bursts;
        x->type_of[serial] = type;
    }
    return serial;
}

/* One second of receiver output and exactly one epoch: optional ephemeris
 * and station messages, then four MSMs, the last with the bit cleared (so
 * the next burst's first frame starts the next epoch). Frames 5 ms apart;
 * the clock is left at the last frame. Bursts start at least 1 s apart. */
static void fx_burst(fx_t *x, bool station, bool eph, size_t plen)
{
    if (x->now < x->next_at) {
        x->now = x->next_at;
    }
    x->next_at = x->now + 1000;
    if (x->bursts < x->rec_cap) {
        x->burst_first[x->bursts] = x->serial;
    }
    bool first = true;
#define FX_STEP()          \
    do {                   \
        if (!first) {      \
            x->now += 5;   \
        }                  \
        first = false;     \
    } while (0)
    if (eph) {
        FX_STEP();
        fx_pub(x, 1019, 61, false);
    }
    if (station) {
        FX_STEP();
        fx_pub(x, 1005, 19, false);
        FX_STEP();
        fx_pub(x, 1033, 60, false);
        FX_STEP();
        fx_pub(x, 1230, 12, false);
    }
    static const uint16_t msm[] = { 1077, 1087, 1097, 1127 };
    for (unsigned i = 0; i < 4; i++) {
        FX_STEP();
        fx_pub(x, msm[i], plen + 37u * i, i < 3);
    }
#undef FX_STEP
    x->bursts++;
    x->tow += 1000;
}

typedef struct {
    uint32_t serial[GOT_MAX];
    uint16_t type[GOT_MAX];
    size_t n;
} got_t;

static got_t *got_new(void)
{
    got_t *g = calloc(1, sizeof(*g));
    TEST_ASSERT_NOT_NULL(g);
    return g;
}

/* One read; every delivered byte must belong to a whole, intact frame. */
static size_t fx_read(fx_t *x, rtcm_core_consumer_t *k, size_t max_len, got_t *g)
{
    static uint8_t buf[READ_MAX];
    TEST_ASSERT_TRUE(max_len <= sizeof(buf));
    memset(buf, 0xEE, max_len);
    size_t n = rtcm_core_read(&x->core, k, buf, max_len, x->now);
    TEST_ASSERT_TRUE(n <= max_len);
    size_t off = 0;
    while (off < n) {
        uint32_t s;
        uint16_t t;
        size_t len = tf_check(buf + off, n - off, &s, &t);
        TEST_ASSERT_TRUE_MESSAGE(len > 0, "delivered bytes are not whole intact frames");
        if (g != NULL) {
            TEST_ASSERT_TRUE(g->n < GOT_MAX);
            g->serial[g->n] = s;
            g->type[g->n] = t;
            g->n++;
        }
        off += len;
    }
    return n;
}

static void fx_drain(fx_t *x, rtcm_core_consumer_t *k, size_t max_len, got_t *g)
{
    for (int i = 0; i < 100000 && fx_read(x, k, max_len, g) > 0; i++) {
    }
}

static rtcm_core_consumer_stats_t cst(fx_t *x, rtcm_core_consumer_t *k)
{
    rtcm_core_consumer_stats_t s;
    rtcm_core_get_consumer_stats(&x->core, k, x->now, &s);
    return s;
}

/* Every frame published since attach is delivered, dropped or skipped once
 * (consumer drained). */
static void check_accounted(fx_t *x, rtcm_core_consumer_t *k, uint64_t frames_at_attach)
{
    rtcm_core_consumer_stats_t s = cst(x, k);
    uint64_t live = s.c.frames_out - s.c.sticky_out;
    TEST_ASSERT_EQUAL_UINT64(x->core.frames_in - frames_at_attach,
                             live + s.c.frames_dropped + s.c.frames_skipped_idle);
}

static void expect_serials(const got_t *g, const uint32_t *want, size_t n)
{
    TEST_ASSERT_EQUAL_UINT(n, g->n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(want[i], g->serial[i]);
    }
}

static void expect_range(const got_t *g, uint32_t first, size_t n)
{
    TEST_ASSERT_EQUAL_UINT(n, g->n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT32(first + i, g->serial[i]);
    }
}

/* The hub's very first epoch is unsure and never sent (see the unsure-epoch
 * tests at the end). Tests of the other rules begin the stream with this
 * lead-in, a terminating MSM published before their consumers attach, so
 * their first burst is a sure epoch. Test serials restart at 0 after it. */
#define LEAD_FRAMES 1u

static void fx_lead_in(fx_t *x)
{
    fx_pub(x, 1127, 20, false);
    x->serial = 0;
    x->tow += 1000;
    x->next_at = x->now + 1000;
}

/* ---- tests --------------------------------------------------------------- */

TEST_CASE("hub: init and input limits", "[rtcm_hub]")
{
    static rtcm_core_t c;
    static uint8_t ring[RTCM_CORE_MIN_RING + 3];
    rtcm_core_cfg_t cfg = { .gap_ms = 250, .default_lag_ms = 3000, .max_consumers = 3, .first_epoch = 1 };
    TEST_ASSERT_FALSE(rtcm_core_init(&c, ring, RTCM_CORE_MIN_RING - 4, &cfg, NULL));
    TEST_ASSERT_TRUE(rtcm_core_init(&c, ring, sizeof(ring), &cfg, NULL));
    TEST_ASSERT_EQUAL_UINT(RTCM_CORE_MIN_RING, c.cap);   /* rounded down to a multiple of 4 */
    cfg.max_consumers = 0;
    TEST_ASSERT_FALSE(rtcm_core_init(&c, ring, sizeof(ring), &cfg, NULL));
    cfg.max_consumers = RTCM_CORE_MAX_CONSUMERS + 1;
    TEST_ASSERT_FALSE(rtcm_core_init(&c, ring, sizeof(ring), &cfg, NULL));
    cfg.max_consumers = 3;
    cfg.default_lag_ms = RTCM_CORE_LAG_MIN_MS - 1;
    TEST_ASSERT_FALSE(rtcm_core_init(&c, ring, sizeof(ring), &cfg, NULL));

    fx_t *x = fx_new(RTCM_CORE_MIN_RING, 3, 3000, 1, 0);
    rtcm_core_t *h = &x->core;
    void *wake[RTCM_CORE_MAX_CONSUMERS];
    size_t n;
    uint8_t f[GNSS_RTCM3_MAX_FRAME + 1];
    size_t len = tf_build(f, 1077, 100, 0, false, 1);
    TEST_ASSERT_FALSE(rtcm_core_publish(h, NULL, len, 1077, x->now, wake, &n));
    TEST_ASSERT_FALSE(rtcm_core_publish(h, f, RTCM_CORE_MIN_FRAME - 1, 1077, x->now, wake, &n));
    TEST_ASSERT_FALSE(rtcm_core_publish(h, f, GNSS_RTCM3_MAX_FRAME + 1, 1077, x->now, wake, &n));
    TEST_ASSERT_TRUE(rtcm_core_publish(h, f, len, 1077, x->now, wake, &n));
    rtcm_core_stats_t st;
    rtcm_core_get_stats(h, x->now, &st);
    TEST_ASSERT_EQUAL_UINT64(3, st.publish_failures);
    TEST_ASSERT_EQUAL_UINT64(1, st.frames_in);
    TEST_ASSERT_EQUAL_UINT64(len, st.bytes_in);

    /* Consumer slots: at most max_consumers; a freed slot is reused. */
    rtcm_core_consumer_t *a = rtcm_core_attach(h, "a-very-long-consumer-name");
    rtcm_core_consumer_t *b = rtcm_core_attach(h, "b");
    rtcm_core_consumer_t *c3 = rtcm_core_attach(h, NULL);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c3);
    TEST_ASSERT_EQUAL_STRING("a-very-long-con", a->name);
    TEST_ASSERT_NULL(rtcm_core_attach(h, "d"));
    rtcm_core_get_stats(h, x->now, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.consumers);
    uint32_t gen;
    TEST_ASSERT_FALSE(rtcm_core_detach(h, b, &gen));
    rtcm_core_get_stats(h, x->now, &st);
    TEST_ASSERT_EQUAL_UINT32(2, st.consumers);
    TEST_ASSERT_EQUAL_PTR(b, rtcm_core_attach(h, "d"));

    /* Lag limits and the read buffer minimum. */
    TEST_ASSERT_EQUAL_UINT32(3000, cst(x, a).max_lag_ms);
    TEST_ASSERT_FALSE(rtcm_core_set_max_lag(h, a, RTCM_CORE_LAG_MIN_MS - 1));
    TEST_ASSERT_FALSE(rtcm_core_set_max_lag(h, a, RTCM_CORE_LAG_MAX_MS + 1));
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(h, a, RTCM_CORE_LAG_MIN_MS));
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(h, a, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, cst(x, a).max_lag_ms);
    uint8_t small[GNSS_RTCM3_MAX_FRAME - 1];
    TEST_ASSERT_EQUAL_UINT(0, rtcm_core_read(h, a, small, sizeof(small), x->now));
    fx_free(x);
}

TEST_CASE("hub: frames come out whole and in order across many ring wraps", "[rtcm_hub]")
{
    /* A ring a few records long and frame sizes from minimal to maximal, so
     * wrap markers land everywhere; frame sequence numbers about to wrap. */
    fx_t *x = fx_new(4800, 4, 3000, 1, 4096);
    x->core.frames_in = 0xFFFFFF00u;
    /* The very first epoch is unsure (never a consumer's start): one frame
     * before the consumers attach, so they start at the first frame below. */
    fx_pub(x, 1077, 30, false);
    x->now += 3;
    x->serial = 0;
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "big");
    rtcm_core_consumer_t *m = rtcm_core_attach(&x->core, "min");
    uint64_t at_attach = x->core.frames_in;
    uint64_t bytes_at_attach = x->core.bytes_in;
    got_t *g = got_new();

    for (uint32_t i = 0; i < 1200; i++) {
        size_t plen = TF_MIN_PAYLOAD + (i * 389u) % (GNSS_RTCM3_MAX_PAYLOAD - TF_MIN_PAYLOAD + 1u);
        if (i % 50 == 7) {
            plen = GNSS_RTCM3_MAX_PAYLOAD;   /* a maximum frame fills the minimum buffer exactly */
        }
        uint32_t s = fx_pub(x, (uint16_t)(1077 + 10 * (i % 3)), plen, false);   /* each frame its own epoch */
        x->now += 3;
        /* The age of the single unread frame, also when it sits behind a wrap marker. */
        TEST_ASSERT_EQUAL_UINT32(3, cst(x, m).lag_ms);

        g->n = 0;
        fx_drain(x, k, 4096, g);
        expect_range(g, s, 1);
        g->n = 0;
        fx_drain(x, m, GNSS_RTCM3_MAX_FRAME, g);
        expect_range(g, s, 1);
        TEST_ASSERT_EQUAL_UINT32(0, cst(x, m).lag_ms);
    }

    /* Batches: several frames per read, whole frames only, nothing lost. */
    for (uint32_t round = 0; round < 50; round++) {
        uint32_t first = x->serial;
        for (uint32_t j = 0; j < 3; j++) {
            fx_pub(x, 1097, 200 + (round * 131u + j * 17u) % 800u, j < 2);
            x->now += 2;
        }
        g->n = 0;
        fx_drain(x, k, 4096, g);
        expect_range(g, first, 3);
        g->n = 0;
        fx_drain(x, m, GNSS_RTCM3_MAX_FRAME, g);
        expect_range(g, first, 3);
    }

    for (int i = 0; i < 2; i++) {
        rtcm_core_consumer_t *c = i ? m : k;
        rtcm_core_consumer_stats_t s = cst(x, c);
        TEST_ASSERT_EQUAL_UINT64(1350, s.c.frames_out);
        TEST_ASSERT_EQUAL_UINT64(x->core.bytes_in - bytes_at_attach, s.c.bytes_out);
        TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
        TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_skipped_idle);
        TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
        TEST_ASSERT_EQUAL_UINT64(1199 + 50, s.c.epochs_out);
        check_accounted(x, c, at_attach);
    }
    TEST_ASSERT_TRUE(x->core.write_off > 60u * x->core.cap);   /* wrapped many times */
    free(g);
    fx_free(x);
}

TEST_CASE("hub: a new consumer gets the sticky copies first, then the next epoch start", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    fx_burst(x, true, false, 300);   /* serials 0..6 */
    fx_burst(x, true, false, 300);   /* 7..13 */

    /* Part of the third burst, then a caster attaches mid-epoch. */
    x->now = x->next_at;
    fx_pub(x, 1005, 19, false);       /* 14 */
    x->now += 5;
    fx_pub(x, 1033, 60, false);       /* 15 */
    x->now += 5;
    fx_pub(x, 1230, 12, false);       /* 16 */
    x->now += 5;
    fx_pub(x, 1077, 300, true);       /* 17 */
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "cst0");
    uint64_t at_attach = x->core.frames_in;
    got_t *g = got_new();

    fx_drain(x, k, 4096, g);
    const uint32_t sticky[] = { 14, 15, 16 };
    expect_serials(g, sticky, 3);

    /* The rest of that epoch is a tail: skipped, not sent. */
    x->now += 5;
    fx_pub(x, 1087, 300, true);
    x->now += 5;
    fx_pub(x, 1097, 300, true);
    x->now += 5;
    fx_pub(x, 1127, 300, false);
    g->n = 0;
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);
    x->bursts++;
    x->tow += 1000;
    x->next_at += 1000;

    /* The next epoch arrives whole, from its first frame. */
    fx_burst(x, true, false, 300);   /* 21..27 */
    fx_drain(x, k, 4096, g);
    expect_range(g, 21, 7);

    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(3, s.c.sticky_out);
    TEST_ASSERT_EQUAL_UINT64(10, s.c.frames_out);
    TEST_ASSERT_EQUAL_UINT64(3, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.epochs_dropped);
    check_accounted(x, k, at_attach);

    /* A consumer attached before anything was published skips the first
     * epoch (unsure: nothing tells where in an epoch the stream began) and
     * starts at the second one's first frame. */
    fx_t *y = fx_new(64 * 1024, 4, 3000, 1, 64);
    rtcm_core_consumer_t *e = rtcm_core_attach(&y->core, "early");
    g->n = 0;
    TEST_ASSERT_EQUAL_UINT(0, fx_read(y, e, 4096, g));
    fx_burst(y, false, false, 100);
    fx_drain(y, e, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);
    fx_burst(y, false, false, 100);
    fx_drain(y, e, 4096, g);
    expect_range(g, 4, 4);
    TEST_ASSERT_EQUAL_UINT64(4, cst(y, e).c.frames_skipped_idle);
    fx_free(y);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: sticky copies: 1006 replaces 1005, 30 s expiry, cleared on reset", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 6, 3000, 1, 1024);
    got_t *g = got_new();
    fx_burst(x, true, false, 100);    /* 1005 = 0, 1033 = 1, 1230 = 2 */
    uint64_t t1033 = x->t_pub[1];

    /* 20 s later a 1006 replaces the 1005 (one slot for both). */
    x->now += 20000;
    x->next_at = x->now;
    uint32_t s1006 = fx_pub(x, 1006, 21, false);
    x->now += 5;
    fx_pub(x, 1077, 100, false);

    /* 1033 and 1230 exactly 30 s old: still replayed, in slot order. */
    x->now = t1033 + RTCM_CORE_STICKY_MAX_AGE_MS;
    rtcm_core_consumer_t *a = rtcm_core_attach(&x->core, "a");
    fx_drain(x, a, 4096, g);
    const uint32_t want_a[] = { s1006, 1, 2 };
    expect_serials(g, want_a, 3);
    TEST_ASSERT_EQUAL_UINT16(1006, g->type[0]);

    /* One millisecond later the 1033 is never replayed; the 1230, sent 5 ms
     * after it, still is. */
    x->now = t1033 + RTCM_CORE_STICKY_MAX_AGE_MS + 1;
    rtcm_core_consumer_t *b = rtcm_core_attach(&x->core, "b");
    g->n = 0;
    fx_drain(x, b, 4096, g);
    const uint32_t want_b[] = { s1006, 2 };
    expect_serials(g, want_b, 2);
    x->now = x->t_pub[2] + RTCM_CORE_STICKY_MAX_AGE_MS + 1;
    rtcm_core_consumer_t *b2 = rtcm_core_attach(&x->core, "b2");
    g->n = 0;
    fx_drain(x, b2, 4096, g);
    expect_serials(g, want_b, 1);

    /* A buffer that cannot hold them all gets the rest on the next read. */
    x->now += 5;
    fx_pub(x, 1033, GNSS_RTCM3_MAX_PAYLOAD - 100, false);
    fx_pub(x, 1230, 900, false);
    rtcm_core_consumer_t *c = rtcm_core_attach(&x->core, "c");
    g->n = 0;
    TEST_ASSERT_TRUE(fx_read(x, c, GNSS_RTCM3_MAX_FRAME, g) > 0);
    TEST_ASSERT_EQUAL_UINT(2, g->n);          /* 1006 + the large 1033 */
    TEST_ASSERT_TRUE(fx_read(x, c, GNSS_RTCM3_MAX_FRAME, g) > 0);
    TEST_ASSERT_EQUAL_UINT(3, g->n);
    TEST_ASSERT_EQUAL_UINT16(1230, g->type[2]);
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, c, GNSS_RTCM3_MAX_FRAME, g));

    /* Framer reset: copies forgotten, and the next frame starts an epoch even
     * though it would otherwise continue the open one. That epoch is unsure
     * (its head may be lost), so it is not sent; the next one is. */
    x->now += 10;
    fx_pub(x, 1077, 100, true);
    rtcm_core_clear_sticky(&x->core);
    rtcm_core_consumer_t *d = rtcm_core_attach(&x->core, "d");
    uint64_t epochs = x->core.ep.epochs;
    g->n = 0;
    fx_drain(x, d, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);
    x->now += 5;
    fx_pub(x, 1087, 100, true);
    TEST_ASSERT_EQUAL_UINT64(epochs + 1, x->core.ep.epochs);
    TEST_ASSERT_EQUAL_UINT64(0, x->core.ep.by_time + x->core.ep.by_gap);
    x->now += 5;
    fx_pub(x, 1127, 100, false);
    fx_drain(x, d, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);
    x->now += 900;
    x->tow += 1000;
    uint32_t next = fx_pub(x, 1077, 100, true);
    fx_drain(x, d, 4096, g);
    expect_range(g, next, 1);

    /* After the reset the stream's next 1005 is the only one replayed. */
    uint32_t s1005 = fx_pub(x, 1005, 19, false);
    rtcm_core_consumer_t *e = rtcm_core_attach(&x->core, "e");
    g->n = 0;
    fx_drain(x, e, 4096, g);
    expect_range(g, s1005, 1);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: resync replays the sticky copies and skips to the next epoch start as idle", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    fx_lead_in(x);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "cst0");
    uint64_t at_attach = x->core.frames_in;
    got_t *g = got_new();

    fx_burst(x, true, false, 200);
    fx_burst(x, true, false, 200);
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3 + 14, g->n);   /* first-read sticky replay, then both epochs */

    /* Caster down for three bursts (not reading), then reconnects. */
    fx_burst(x, true, false, 200);
    fx_burst(x, true, false, 200);
    fx_burst(x, true, false, 200);
    /* ... and the next burst has started when it resyncs. */
    x->now = x->next_at;
    uint32_t s1005 = fx_pub(x, 1005, 19, false);
    x->now += 5;
    fx_pub(x, 1033, 60, false);
    rtcm_core_resync(&x->core, k);
    x->now += 5;
    fx_pub(x, 1230, 12, false);
    x->now += 5;
    fx_pub(x, 1077, 200, false);
    x->bursts++;
    x->tow += 1000;
    x->next_at += 1000;

    g->n = 0;
    fx_drain(x, k, 4096, g);
    /* The newest copies at read time (all three from the open epoch, whose
     * rest is a tail), then nothing until the next epoch starts. */
    expect_range(g, s1005, 3);

    fx_burst(x, true, false, 200);
    g->n = 0;
    fx_drain(x, k, 4096, g);
    expect_range(g, s1005 + 4, 7);

    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
    /* Three unread bursts and the open epoch's first two frames at the
     * resync, then its last two frames while waiting. */
    TEST_ASSERT_EQUAL_UINT64(21 + 2 + 2, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(6, s.c.sticky_out);
    /* Epoch 1 complete; epoch 2 was still open at the resync and is not
     * counted; the newest is open. */
    TEST_ASSERT_EQUAL_UINT64(1, s.c.epochs_out);
    check_accounted(x, k, at_attach);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: the lag limit drops whole oldest epochs and finishes a partly sent one", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    fx_lead_in(x);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "onocoy");
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, k, 1000));
    got_t *g = got_new();
    const size_t one = GNSS_RTCM3_MAX_FRAME;   /* MSMs of 600+ bytes: one frame per read */

    fx_burst(x, false, false, 600);           /* epoch of serials 0..3 */
    fx_read(x, k, one, g);
    expect_range(g, 0, 1);                    /* partly delivered */

    for (int i = 0; i < 5; i++) {
        fx_burst(x, false, false, 600);       /* serials 4..23, the oldest 5 s old */
    }
    /* The open epoch is finished first although it is old ... */
    rtcm_core_consumer_stats_t st;
    for (uint32_t s = 1; s <= 3; s++) {
        g->n = 0;
        fx_read(x, k, one, g);
        expect_range(g, s, 1);
        if (s == 1) {
            st = cst(x, k);   /* age of the oldest unread frame: serial 2 */
            TEST_ASSERT_EQUAL_UINT32((uint32_t)(x->now - x->t_pub[2]), st.lag_ms);
        }
    }
    /* The read that sent the epoch's last frame went on and dropped the
     * stale epochs behind it: the oldest unread frame is now fresh. */
    st = cst(x, k);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(x->now - x->t_pub[20]), st.lag_ms);
    TEST_ASSERT_EQUAL_UINT64(4, st.c.epochs_dropped);
    /* ... then the epochs over the limit go, whole, and the fresh one comes. */
    for (uint32_t s = 20; s <= 23; s++) {
        g->n = 0;
        fx_read(x, k, one, g);
        expect_range(g, s, 1);
    }
    g->n = 0;
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, k, one, g));
    st = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(8, st.c.frames_out);
    TEST_ASSERT_EQUAL_UINT64(16, st.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(4, st.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(1, st.c.lag_drops);   /* one event */
    TEST_ASSERT_EQUAL_UINT64(1, st.c.epochs_out);
    TEST_ASSERT_EQUAL_UINT64(0, st.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT32(0, st.lag_ms);

    /* An epoch dropped while still arriving is dropped to its end. */
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, k, 100));
    uint64_t start = x->next_at;
    x->now = start;
    fx_pub(x, 1077, 600, true);
    x->now += 5;
    fx_pub(x, 1087, 600, true);
    x->now = start + 205;
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, k, one, g));
    x->now = start + 210;                     /* within the burst gap: same epoch */
    fx_pub(x, 1097, 600, true);
    x->now += 5;
    fx_pub(x, 1127, 600, false);
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, k, one, g));
    x->bursts++;
    x->tow += 1000;
    x->next_at = start + 1000;
    uint32_t next = x->serial;
    fx_burst(x, false, false, 600);
    for (uint32_t s = next; s < next + 4; s++) {
        g->n = 0;
        fx_read(x, k, one, g);
        expect_range(g, s, 1);
    }
    st = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(20, st.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(5, st.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(2, st.c.lag_drops);
    TEST_ASSERT_EQUAL_UINT64(2, st.c.epochs_out);
    check_accounted(x, k, LEAD_FRAMES);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: epochs already over the limit while waiting for a start are skipped as idle", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "late");
    got_t *g = got_new();
    for (int i = 0; i < 6; i++) {
        fx_burst(x, false, false, 300);   /* the oldest 5 s old when read */
    }
    fx_drain(x, k, 4096, g);
    expect_range(g, 12, 12);              /* bursts 3..5: within 3000 ms */
    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(12, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, s.c.lag_drops);
    check_accounted(x, k, 0);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: a lapped consumer counts the loss and starts over with sticky copies", "[rtcm_hub]")
{
    fx_t *x = fx_new(8192, 4, 3000, 1, 1024);
    fx_lead_in(x);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "stalled");
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, k, 60000));
    got_t *g = got_new();

    fx_burst(x, true, false, 300);        /* epoch 1: serials 0..6 */
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3 + 7, g->n);
    for (int i = 0; i < 10; i++) {
        fx_burst(x, true, false, 300);    /* about twice the ring */
    }
    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(x->now - x->t_pub[6]), s.lag_ms);   /* estimate: unread data is gone */

    g->n = 0;
    fx_drain(x, k, 4096, g);
    const uint32_t sticky[] = { 70, 71, 72 };   /* the newest copies, then wait for the next epoch */
    expect_serials(g, sticky, 3);
    s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT32(1, s.c.laps);
    TEST_ASSERT_EQUAL_UINT64(70, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(10, s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_skipped_idle);

    fx_burst(x, true, false, 300);
    g->n = 0;
    fx_drain(x, k, 4096, g);
    expect_range(g, 77, 7);
    check_accounted(x, k, LEAD_FRAMES);

    /* A consumer that never read since attach is not streaming: its lap is idle. */
    rtcm_core_consumer_t *idle = rtcm_core_attach(&x->core, "idle");
    uint64_t at_attach = x->core.frames_in;
    for (int i = 0; i < 10; i++) {
        fx_burst(x, true, false, 300);
    }
    g->n = 0;
    fx_drain(x, idle, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3, g->n);
    s = cst(x, idle);
    TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(70, s.c.frames_skipped_idle);
    check_accounted(x, idle, at_attach);

    /* Lapped while dropping an epoch for the lag limit: that epoch is counted
     * once, then the ten after it. */
    rtcm_core_consumer_t *d = rtcm_core_attach(&x->core, "dropper");
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, d, 100));
    at_attach = x->core.frames_in;
    fx_burst(x, true, false, 300);
    g->n = 0;
    fx_drain(x, d, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3 + 7, g->n);      /* sticky replay, then the burst: streaming */
    uint64_t t0 = x->next_at;
    x->now = t0;
    fx_pub(x, 1005, 19, false);
    x->now += 5;
    fx_pub(x, 1033, 60, false);
    x->now = t0 + 205;
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, d, 4096, NULL));   /* epoch start 205 ms old: dropped */
    TEST_ASSERT_EQUAL_UINT64(1, cst(x, d).c.epochs_dropped);
    fx_pub(x, 1230, 12, false);               /* the rest of that epoch, within the burst gap */
    static const uint16_t msm[] = { 1077, 1087, 1097, 1127 };
    for (unsigned i = 0; i < 4; i++) {
        x->now += 5;
        fx_pub(x, msm[i], 300, i < 3);
    }
    x->bursts++;
    x->tow += 1000;
    x->next_at = t0 + 1000;
    for (int i = 0; i < 10; i++) {
        fx_burst(x, true, false, 300);
    }
    g->n = 0;
    fx_drain(x, d, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3, g->n);          /* lapped: sticky copies, then wait */
    s = cst(x, d);
    TEST_ASSERT_EQUAL_UINT32(1, s.c.laps);
    TEST_ASSERT_EQUAL_UINT64(11, s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT64(77, s.c.frames_dropped);
    check_accounted(x, d, at_attach);
    free(g);
    fx_free(x);
}

/* Producer activity injected between two steps of a consumer read. */
static void hook_lap(fx_t *x)
{
    for (int i = 0; i < 4; i++) {
        fx_burst(x, true, false, 200);
    }
}

TEST_CASE("hub: a frame overwritten while a consumer copies it is never delivered", "[rtcm_hub]")
{
    unsigned fired = 0;
    unsigned lapped = 0;
    for (unsigned at = 0; at < 400; at++) {
        fx_t *x = fx_new(4800, 4, 60000, 1, 1024);
        fx_lead_in(x);
        rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "victim");
        fx_burst(x, true, false, 200);
        fx_drain(x, k, 4096, NULL);           /* streaming, sticky replay done */
        fx_burst(x, true, false, 200);
        fx_burst(x, true, false, 200);

        x->hook = hook_lap;
        x->hook_at = at;
        x->unlocks = 0;
        x->hook_armed = true;
        fx_read(x, k, 4096, NULL);            /* asserts whole, intact frames */
        x->hook_armed = false;
        if (!x->hook_fired) {
            fx_free(x);
            break;                            /* past the last lock step of the read */
        }
        fired++;
        fx_drain(x, k, 4096, NULL);
        rtcm_core_consumer_stats_t s = cst(x, k);
        if (s.c.laps > 0) {
            lapped++;
        }
        TEST_ASSERT_TRUE(s.c.laps <= 1);
        check_accounted(x, k, LEAD_FRAMES);
        fx_free(x);
    }
    TEST_ASSERT_GREATER_THAN_UINT(20, fired);
    TEST_ASSERT_EQUAL_UINT(fired, lapped);
}

/* Per-consumer expectations in the fan-out simulation. */
typedef struct {
    rtcm_core_consumer_t *k;
    unsigned period_s;
    size_t buf;
    uint32_t lag;
    uint64_t at_attach;
    bool in_epoch;
    uint32_t next_serial;
    uint64_t epochs_seen;   /* epochs whose first frame was delivered */
    uint64_t epochs_cut;    /* delivered epochs left open by a resync */
} sim_t;

static void sim_read(fx_t *x, sim_t *c, got_t *g)
{
    for (int guard = 0; guard < 1000; guard++) {
        uint32_t laps_before = cst(x, c->k).c.laps;
        g->n = 0;
        if (fx_read(x, c->k, c->buf, g) == 0) {
            return;
        }
        bool lapped = cst(x, c->k).c.laps != laps_before;
        for (size_t i = 0; i < g->n; i++) {
            uint32_t s = g->serial[i];
            bool start = x->burst_first[x->burst_of[s]] == s;
            if (c->in_epoch && s != c->next_serial) {
                TEST_ASSERT_TRUE_MESSAGE(lapped, "an epoch was cut without a lap");
                c->in_epoch = false;
            }
            if (!c->in_epoch) {
                TEST_ASSERT_TRUE_MESSAGE(start, "delivery did not begin at an epoch start");
                TEST_ASSERT_TRUE_MESSAGE(x->now - x->t_pub[s] <= c->lag, "epoch start over the lag limit");
                c->epochs_seen++;
            }
            c->in_epoch = g->type[i] != 1127;
            c->next_serial = s + 1;
        }
    }
    TEST_FAIL_MESSAGE("read did not drain");
}

static void sim_attach(fx_t *x, sim_t *c, unsigned i)
{
    c->k = rtcm_core_attach(&x->core, "cst");
    TEST_ASSERT_NOT_NULL(c->k);
    c->period_s = 1 + (i * 5u) % 20u;
    c->lag = (i % 3 == 0) ? 1000 : (i % 3 == 1) ? 3000 : 10000;
    c->buf = GNSS_RTCM3_MAX_FRAME + (i * 811u) % 3072u;
    c->at_attach = x->core.frames_in;
    c->in_epoch = false;
    c->epochs_seen = 0;
    c->epochs_cut = 0;
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, c->k, c->lag));
}

TEST_CASE("hub: many consumers at different paces get whole epochs within their limits", "[rtcm_hub]")
{
    const unsigned seconds = 600;
    const unsigned stall = RTCM_CORE_MAX_CONSUMERS - 1;
    fx_t *x = fx_new(64 * 1024, RTCM_CORE_MAX_CONSUMERS, 3000, 1, seconds * 5 + 16);
    got_t *g = got_new();
    sim_t c[RTCM_CORE_MAX_CONSUMERS];
    memset(c, 0, sizeof(c));
    for (unsigned i = 0; i < RTCM_CORE_MAX_CONSUMERS; i++) {
        sim_attach(x, &c[i], i);
    }
    /* One consumer streams for 10 s, then stalls far longer than the ring holds. */
    c[stall].period_s = 90;
    c[stall].lag = 60000;
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, c[stall].k, c[stall].lag));
    TEST_ASSERT_NULL(rtcm_core_attach(&x->core, "one too many"));

    for (unsigned t = 1; t <= seconds; t++) {
        fx_burst(x, false, t % 10 == 0, 150 + (t * 53u) % 400u);
        for (unsigned i = 0; i < RTCM_CORE_MAX_CONSUMERS; i++) {
            bool due = t % c[i].period_s == 0 || (i == stall && t <= 10);
            if (c[i].k != NULL && due) {
                sim_read(x, &c[i], g);
            }
        }
        if (t % 120 == 0) {
            /* Two casters reconnect. */
            for (unsigned i = 5; i <= 6; i++) {
                if (c[i].k->streaming && c[i].k->epoch_open) {
                    c[i].epochs_cut++;
                }
                rtcm_core_resync(&x->core, c[i].k);
                c[i].in_epoch = false;
            }
        }
        if (t == 200) {
            uint32_t gen;
            TEST_ASSERT_FALSE(rtcm_core_detach(&x->core, c[7].k, &gen));
            c[7].k = NULL;
            TEST_ASSERT_EQUAL_UINT32(RTCM_CORE_MAX_CONSUMERS - 1, x->core.attached);
        }
        if (t == 300) {
            sim_attach(x, &c[7], 7);   /* takes the freed slot, counters from zero */
            TEST_ASSERT_EQUAL_UINT64(0, cst(x, c[7].k).c.frames_out);
        }
    }

    for (unsigned i = 0; i < RTCM_CORE_MAX_CONSUMERS; i++) {
        sim_read(x, &c[i], g);
        check_accounted(x, c[i].k, c[i].at_attach);
        rtcm_core_consumer_stats_t s = cst(x, c[i].k);
        if (i == stall) {
            TEST_ASSERT_GREATER_THAN_UINT32(0, s.c.laps);
            TEST_ASSERT_GREATER_THAN_UINT64(0, s.c.frames_dropped);
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
        /* Every epoch delivered from its start was delivered to its end and
         * counted when the next began, except those a resync cut and the
         * last one if it is still open. */
        uint64_t open = (c[i].k->streaming && c[i].k->epoch_open) ? 1 : 0;
        TEST_ASSERT_EQUAL_UINT64(c[i].epochs_seen - open - c[i].epochs_cut, s.c.epochs_out);
        if (c[i].period_s * 1000u > c[i].lag + 1000u) {
            TEST_ASSERT_GREATER_THAN_UINT32(0, s.c.lag_drops);
            TEST_ASSERT_GREATER_THAN_UINT64(0, s.c.epochs_dropped);
        } else if (c[i].period_s * 1000u + 100u < c[i].lag) {
            TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
        }
    }
    TEST_ASSERT_EQUAL_UINT64(10, c[5].epochs_cut + c[6].epochs_cut);   /* five resyncs each */
    TEST_ASSERT_EQUAL_UINT64(seconds, x->core.ep.epochs);
    TEST_ASSERT_EQUAL_UINT64(seconds - 1, x->core.ep.by_mmb);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: two-day run across the millisecond and epoch number wraps", "[rtcm_hub]")
{
    fx_t *x = fx_new(32 * 1024, 4, 1000, UINT32_MAX - 1000u, 0);
    x->now = (uint64_t)UINT32_MAX - 10000u;   /* rx_ms wraps 10 s in */
    fx_lead_in(x);
    rtcm_core_consumer_t *fast = rtcm_core_attach(&x->core, "fast");
    rtcm_core_consumer_t *slow = rtcm_core_attach(&x->core, "slow");
    TEST_ASSERT_TRUE(rtcm_core_set_max_lag(&x->core, slow, 3000));
    const uint32_t seconds = 2u * 86400u;
    for (uint32_t t = 1; t <= seconds; t++) {
        fx_burst(x, false, false, 30);
        fx_drain(x, fast, 4096, NULL);
        if (t % 7 == 0) {
            fx_drain(x, slow, 4096, NULL);
        }
    }
    fx_drain(x, slow, 4096, NULL);

    rtcm_core_consumer_stats_t f = cst(x, fast);
    TEST_ASSERT_EQUAL_UINT64(4ull * seconds, f.c.frames_out);
    TEST_ASSERT_EQUAL_UINT64(0, f.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(seconds - 1, f.c.epochs_out);
    rtcm_core_consumer_stats_t s = cst(x, slow);
    TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
    /* Read every 7 s with a 3 s limit: of seven epochs the four oldest go.
     * The first read was not streaming yet (idle skips), the final one saw
     * seconds % 7 epochs. */
    const uint32_t reads = seconds / 7u;
    const uint32_t rem = seconds % 7u;
    TEST_ASSERT_EQUAL_UINT64(4ull * (reads - 1u) + (rem > 3u ? rem - 3u : 0u), s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(reads, s.c.lag_drops);
    TEST_ASSERT_EQUAL_UINT64(16, s.c.frames_skipped_idle);
    check_accounted(x, fast, LEAD_FRAMES);
    check_accounted(x, slow, LEAD_FRAMES);
    TEST_ASSERT_EQUAL_UINT64(LEAD_FRAMES + seconds, x->core.ep.epochs);
    TEST_ASSERT_EQUAL_UINT64(seconds, x->core.ep.by_mmb);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1000u + seconds, x->core.committed_epoch);

    rtcm_core_stats_t st;
    rtcm_core_get_stats(&x->core, x->now, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.last_frame_age_ms);
    rtcm_core_get_stats(&x->core, x->now + 10ull * 86400000ull, &st);
    TEST_ASSERT_EQUAL_UINT32(864000000u, st.last_frame_age_ms);
    rtcm_core_get_stats(&x->core, x->now + 60ull * 86400000ull, &st);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1u, st.last_frame_age_ms);

    /* Unread data 50 days old (its 32-bit stamp has wrapped) is stale, not fresh. */
    fx_burst(x, false, false, 30);
    x->now += 50ull * 86400000ull;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cst(x, fast).lag_ms);
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, fast, 4096, NULL));
    fx_burst(x, false, false, 30);
    TEST_ASSERT_TRUE(fx_read(x, fast, 4096, NULL) > 0);
    fx_free(x);
}

TEST_CASE("hub: waiters are handed back once; detach waits for a running wake pass", "[rtcm_hub]")
{
    fx_t *x = fx_new(16 * 1024, 4, 3000, 1, 0);
    rtcm_core_t *h = &x->core;
    rtcm_core_consumer_t *a = rtcm_core_attach(h, "a");
    rtcm_core_consumer_t *b = rtcm_core_attach(h, "b");
    void *wa = (void *)(uintptr_t)0xA0;
    void *wb = (void *)(uintptr_t)0xB0;
    uint8_t f[64];
    size_t len = tf_build(f, 1077, 30, 0, false, 1);
    void *wake[RTCM_CORE_MAX_CONSUMERS];
    size_t n;

    /* Sticky replay still pending right after attach: read first. */
    TEST_ASSERT_FALSE(rtcm_core_prepare_wait(h, a, wa));
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, a, 4096, NULL));
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, b, 4096, NULL));
    TEST_ASSERT_TRUE(rtcm_core_prepare_wait(h, a, wa));
    TEST_ASSERT_TRUE(rtcm_core_prepare_wait(h, b, wb));

    TEST_ASSERT_TRUE(rtcm_core_publish(h, f, len, 1077, x->now, wake, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_TRUE((wake[0] == wa && wake[1] == wb) || (wake[0] == wb && wake[1] == wa));
    /* A detach during the wake pass must wait for its end. */
    uint32_t gen;
    TEST_ASSERT_TRUE(rtcm_core_detach(h, b, &gen));
    TEST_ASSERT_FALSE(rtcm_core_wake_passed(h, gen));
    rtcm_core_wake_done(h);
    TEST_ASSERT_TRUE(rtcm_core_wake_passed(h, gen));

    /* Handed back once: no registration, no wake. */
    TEST_ASSERT_TRUE(rtcm_core_publish(h, f, len, 1077, x->now, wake, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    /* Data pending: no registration. */
    TEST_ASSERT_FALSE(rtcm_core_prepare_wait(h, a, wa));
    TEST_ASSERT_TRUE(fx_read(x, a, 4096, NULL) > 0);
    TEST_ASSERT_TRUE(rtcm_core_prepare_wait(h, a, wa));
    rtcm_core_cancel_wait(h, a);
    TEST_ASSERT_TRUE(rtcm_core_publish(h, f, len, 1077, x->now, wake, &n));
    TEST_ASSERT_EQUAL_UINT(0, n);
    /* No wake pass running: detach returns at once. */
    TEST_ASSERT_FALSE(rtcm_core_detach(h, a, &gen));
    fx_free(x);
}

TEST_CASE("hub: counters for epochs, frames and ages", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_stats_t st;
    rtcm_core_get_stats(&x->core, x->now, &st);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, st.last_frame_age_ms);
    TEST_ASSERT_EQUAL_UINT(64 * 1024, st.capacity);
    TEST_ASSERT_EQUAL_UINT64(0, st.epochs_in);

    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "k");
    TEST_ASSERT_EQUAL_UINT32(0, cst(x, k).lag_ms);
    fx_burst(x, true, false, 100);
    uint64_t first = x->t_pub[0];
    x->now += 700;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(x->now - first), cst(x, k).lag_ms);
    rtcm_core_get_stats(&x->core, x->now, &st);
    TEST_ASSERT_EQUAL_UINT32(700, st.last_frame_age_ms);
    TEST_ASSERT_EQUAL_UINT64(7, st.frames_in);
    TEST_ASSERT_EQUAL_UINT64(1, st.epochs_in);
    TEST_ASSERT_EQUAL_UINT32(1, st.consumers);

    /* Lost terminating MSM, then a receiver that never clears the bit. */
    x->now = x->next_at;
    fx_pub(x, 1077, 100, true);
    x->now += 5;
    fx_pub(x, 1127, 100, true);
    x->tow += 1000;
    x->now += 600;
    fx_pub(x, 1005, 19, false);           /* gap rule */
    x->now += 100;
    fx_pub(x, 1077, 100, true);
    x->tow += 1000;
    x->now += 100;
    fx_pub(x, 1077, 100, true);           /* time rule */
    rtcm_core_get_stats(&x->core, x->now, &st);
    TEST_ASSERT_EQUAL_UINT64(4, st.epochs_in);
    TEST_ASSERT_EQUAL_UINT64(1, st.epochs_by_mmb);
    TEST_ASSERT_EQUAL_UINT64(1, st.epochs_by_gap);
    TEST_ASSERT_EQUAL_UINT64(1, st.epochs_by_time);
    TEST_ASSERT_EQUAL_UINT64(0, st.publish_failures);

    fx_drain(x, k, 4096, NULL);
    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT32(0, s.lag_ms);
    /* The sticky copies, then everything after the first epoch (unsure: the
     * very first frames, skipped as idle). */
    TEST_ASSERT_EQUAL_UINT64(3 + 5, s.c.frames_out);
    TEST_ASSERT_EQUAL_UINT64(7, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(2, s.c.epochs_out);
    check_accounted(x, k, 0);
    fx_free(x);
}

/* ---- unsure epochs (framer resets) --------------------------------------- */

/* The tail of a burst whose head was lost: 1087, 1097, 1127 (bit cleared),
 * 5 ms apart, starting `dt` ms after the previous frame. */
static void fx_tail(fx_t *x, uint32_t dt)
{
    x->now += dt;
    fx_pub(x, 1087, 120, true);
    x->now += 5;
    fx_pub(x, 1097, 120, true);
    x->now += 5;
    fx_pub(x, 1127, 120, false);
    x->bursts++;
    x->tow += 1000;
    x->next_at = x->now + 1000;
}

TEST_CASE("hub: a resync after a framer reset starts at a whole epoch, not at the tail it sees first",
          "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "cst0");
    got_t *g = got_new();
    fx_burst(x, true, false, 200);
    fx_burst(x, true, false, 200);
    fx_drain(x, k, 4096, g);

    /* A pause (the caster stops reading), the receiver line is handed back
     * and the framer resets; the bytes up to the middle of an epoch are lost
     * (its 1077 among them). */
    x->now = x->next_at;
    fx_pub(x, 1005, 19, false);
    rtcm_core_clear_sticky(&x->core);
    rtcm_core_resync(&x->core, k);
    uint64_t skipped_before = cst(x, k).c.frames_skipped_idle;
    fx_tail(x, 30);
    g->n = 0;
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);   /* not 1087, 1097, 1127 */

    /* The next epoch is whole: delivered from its 1077 (its station
     * messages come first in the burst and belong to it). */
    fx_burst(x, false, false, 200);
    uint32_t first = x->burst_first[x->bursts - 1];
    fx_drain(x, k, 4096, g);
    expect_range(g, first, 4);
    TEST_ASSERT_EQUAL_UINT16(1077, g->type[0]);

    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(3, s.c.frames_skipped_idle - skipped_before);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.epochs_dropped);
    TEST_ASSERT_EQUAL_UINT32(0, s.c.laps);
    check_accounted(x, k, 0);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: a consumer attached at boot, with the receiver mid-epoch, starts at an epoch's first frame",
          "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "boot");
    got_t *g = got_new();
    TEST_ASSERT_EQUAL_UINT(0, fx_read(x, k, 4096, g));

    /* The first bytes the station sees are an epoch's tail. */
    fx_tail(x, 0);
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);

    fx_burst(x, true, false, 200);   /* 1005, 1033, 1230, 1077 .. 1127 */
    uint32_t first = x->burst_first[x->bursts - 1];
    fx_burst(x, true, false, 200);
    fx_drain(x, k, 4096, g);
    expect_range(g, first, 14);
    TEST_ASSERT_EQUAL_UINT16(1005, g->type[0]);

    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(3, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(1, s.c.epochs_out);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.sticky_out);
    check_accounted(x, k, 0);

    /* A consumer attached after that tail, mid-way through the next epoch,
     * waits for the next one as before. */
    rtcm_core_consumer_t *m = rtcm_core_attach(&x->core, "late");
    fx_burst(x, false, false, 200);
    g->n = 0;
    fx_drain(x, m, 4096, g);
    TEST_ASSERT_EQUAL_UINT(3 + 4, g->n);   /* sticky copies, then the new epoch */
    free(g);
    fx_free(x);
}

TEST_CASE("hub: a streaming consumer never gets the part of an epoch after a framer reset", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "cst0");
    got_t *g = got_new();
    fx_burst(x, false, false, 200);   /* unsure (first frames ever): skipped */
    fx_burst(x, false, false, 200);
    fx_drain(x, k, 4096, g);
    uint32_t b1 = x->burst_first[1];
    expect_range(g, b1, 4);

    /* The next epoch is being sent when the framer resets. */
    x->now = x->next_at;
    uint32_t h0 = fx_pub(x, 1077, 200, true);
    x->now += 5;
    fx_pub(x, 1087, 200, true);
    g->n = 0;
    fx_drain(x, k, 4096, g);
    expect_range(g, h0, 2);
    rtcm_core_clear_sticky(&x->core);
    /* The rest of it (1097 lost), then whole epochs again. */
    x->now += 30;
    fx_pub(x, 1127, 200, false);
    x->bursts++;
    x->tow += 1000;
    x->next_at = x->now + 1000;
    g->n = 0;
    fx_drain(x, k, 4096, g);
    TEST_ASSERT_EQUAL_UINT(0, g->n);

    fx_burst(x, false, false, 200);
    uint32_t b3 = x->burst_first[x->bursts - 1];
    fx_burst(x, false, false, 200);
    fx_drain(x, k, 4096, g);
    expect_range(g, b3, 8);

    rtcm_core_consumer_stats_t s = cst(x, k);
    /* Four frames of the first (unsure) epoch and the 1127 after the reset. */
    TEST_ASSERT_EQUAL_UINT64(4 + 1, s.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.epochs_dropped);
    /* b1 (ended by the next start), the epoch cut by the reset (sent from
     * its start; it ended at the unsure epoch) and b3. */
    TEST_ASSERT_EQUAL_UINT64(3, s.c.epochs_out);
    TEST_ASSERT_EQUAL_UINT64(4 + 2 + 8, s.c.frames_out);
    check_accounted(x, k, 0);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: a 1 Hz stream without MSMs still flows after a framer reset", "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "cst0");
    got_t *g = got_new();
    uint32_t s[8];
    for (int i = 0; i < 3; i++) {
        s[i] = fx_pub(x, 1019, 61, false);   /* ephemeris: not a sticky type */
        fx_drain(x, k, 4096, g);
        x->now += 1000;
    }
    /* The first frame is unsure; the gap after it makes the second sure. */
    const uint32_t want1[] = { s[1], s[2] };
    expect_serials(g, want1, 2);

    rtcm_core_clear_sticky(&x->core);
    for (int i = 3; i < 6; i++) {
        s[i] = fx_pub(x, 1019, 61, false);   /* ephemeris: not a sticky type */
        fx_drain(x, k, 4096, g);
        x->now += 1000;
    }
    const uint32_t want2[] = { s[1], s[2], s[4], s[5] };
    expect_serials(g, want2, 4);

    rtcm_core_consumer_stats_t st = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(2, st.c.frames_skipped_idle);
    TEST_ASSERT_EQUAL_UINT64(0, st.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(1, st.c.epochs_out);   /* the first sure epoch, ended by the reset */
    rtcm_core_stats_t hs;
    rtcm_core_get_stats(&x->core, x->now, &hs);
    TEST_ASSERT_EQUAL_UINT64(4, hs.epochs_in);
    TEST_ASSERT_EQUAL_UINT64(2, hs.epochs_by_gap);
    check_accounted(x, k, 0);
    free(g);
    fx_free(x);
}

TEST_CASE("hub: records carry the unsure flag; a waiting consumer after a lap skips unsure epochs as losses",
          "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    fx_pub(x, 1077, 50, false);
    fx_pub(x, 1077, 50, false);
    /* The stored type keeps the message number in its low 12 bits. */
    uint16_t t0, t1;
    memcpy(&t0, x->ring + 2, sizeof(t0));
    size_t r1 = RTCM_CORE_REC_HDR + ((50u + 6u + 3u) & ~3u);
    memcpy(&t1, x->ring + r1 + 2, sizeof(t1));
    TEST_ASSERT_EQUAL_HEX16(1077u | RTCM_CORE_REC_UNSURE, t0);
    TEST_ASSERT_EQUAL_HEX16(1077u, t1);
    TEST_ASSERT_EQUAL_UINT16(1077, t0 & RTCM_CORE_REC_TYPE_MASK);

    /* Waiting with losses counted (as after a lap): unsure frames are losses. */
    rtcm_core_consumer_t *k = rtcm_core_attach(&x->core, "k");
    k->wait_loss = true;
    rtcm_core_clear_sticky(&x->core);
    x->now += 2;
    fx_pub(x, 1087, 50, false);
    x->now += 900;
    uint32_t sure = fx_pub(x, 1077, 50, false);
    got_t *g = got_new();
    fx_drain(x, k, 4096, g);
    expect_range(g, sure, 1);
    rtcm_core_consumer_stats_t s = cst(x, k);
    TEST_ASSERT_EQUAL_UINT64(1, s.c.frames_dropped);
    TEST_ASSERT_EQUAL_UINT64(0, s.c.frames_skipped_idle);
    free(g);
    fx_free(x);
}

/* ---- pausing between epochs ------------------------------------------------ */

TEST_CASE("hub: an epoch is in flight from its first announcing MSM to its last, or to the gap rule",
          "[rtcm_hub]")
{
    fx_t *x = fx_new(64 * 1024, 4, 3000, 1, 1024);
    uint32_t age = 0;
    TEST_ASSERT_FALSE(rtcm_core_epoch_in_flight(&x->core, x->now, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, age);
    fx_pub(x, 1005, 19, false);              /* station messages only: nothing to cut */
    TEST_ASSERT_FALSE(rtcm_core_epoch_in_flight(&x->core, x->now, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(0, age);
    x->now += 5;
    fx_pub(x, 1077, 100, true);
    x->now += 5;
    fx_pub(x, 1087, 100, true);
    TEST_ASSERT_TRUE(rtcm_core_epoch_in_flight(&x->core, x->now + 10, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(10, age);
    x->now += 5;
    fx_pub(x, 1127, 100, false);             /* its last MSM */
    TEST_ASSERT_FALSE(rtcm_core_epoch_in_flight(&x->core, x->now, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(0, age);
    /* The next epoch's station messages: still between MSM runs. */
    x->now += 800;
    x->tow += 1000;
    fx_pub(x, 1033, 60, false);
    TEST_ASSERT_FALSE(rtcm_core_epoch_in_flight(&x->core, x->now, 250, NULL));
    /* Its closing MSM is lost: in flight until the gap rule would end it. */
    x->now += 5;
    fx_pub(x, 1077, 100, true);
    TEST_ASSERT_TRUE(rtcm_core_epoch_in_flight(&x->core, x->now + 249, 250, NULL));
    TEST_ASSERT_FALSE(rtcm_core_epoch_in_flight(&x->core, x->now + 250, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(250, age);
    /* A clock that reads earlier than the newest frame: age 0, not a wrap. */
    TEST_ASSERT_TRUE(rtcm_core_epoch_in_flight(&x->core, x->now - 3, 250, &age));
    TEST_ASSERT_EQUAL_UINT32(0, age);
    fx_free(x);
}
