/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM3-only scanner (gnss_rtcm_scan.h): the same frames as gnss_framer on
 * every public capture at several feed sizes, a cost that crafted input
 * cannot raise, held-input accounting byte by byte, a frame behind a
 * truncated one, reset, and the one documented difference from the framer
 * (two overlapping CRC-valid frames). Also gnss_framer's own held-input
 * report and unit start offsets.
 */
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "gnss_framing.h"
#include "gnss_rtcm3.h"
#include "gnss_rtcm_scan.h"
#include "rtcm_vectors.h"
#include "unity.h"

typedef struct {
    uint64_t start;
    size_t len;
    uint64_t at_bytes_in;   /* bytes fed when it came out */
} got_t;

typedef struct {
    gnss_rtcm_scanner_t *s;
    const uint8_t *buf;     /* the stream, to check each frame against (NULL: not checked) */
    got_t got[4096];
    size_t n;
} sink_t;

static gnss_rtcm_scanner_t s_scan;
static sink_t s_sink;

static void on_frame(const gnss_frame_t *fr, void *ctx)
{
    sink_t *k = ctx;
    TEST_ASSERT_EQUAL(GNSS_FRAME_RTCM3, fr->kind);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(fr->data, fr->len));
    TEST_ASSERT_EQUAL_UINT16(gnss_rtcm3_msg_type(fr->data, fr->len), fr->rtcm_msg_type);
    if (k->buf != NULL) {
        TEST_ASSERT_EQUAL_MEMORY(k->buf + fr->start, fr->data, fr->len);
    }
    gnss_rtcm_scan_stats_t st;
    gnss_rtcm_scan_get_stats(k->s, &st);
    TEST_ASSERT_LESS_THAN_size_t(sizeof(k->got) / sizeof(k->got[0]), k->n);
    k->got[k->n++] = (got_t){ .start = fr->start, .len = fr->len, .at_bytes_in = st.bytes_in };
}

static sink_t *scan_init(const uint8_t *buf)
{
    memset(&s_sink, 0, sizeof(s_sink));
    s_sink.s = &s_scan;
    s_sink.buf = buf;
    gnss_rtcm_scan_init(&s_scan, on_frame, &s_sink);
    return &s_sink;
}

/* bytes_in == discarded + emitted + held, the held bytes the newest. */
static void check_accounting(const gnss_rtcm_scanner_t *s, const sink_t *k)
{
    gnss_rtcm_scan_stats_t st;
    gnss_rtcm_scan_get_stats(s, &st);
    uint64_t first;
    size_t held = gnss_rtcm_scan_held(s, &first);
    TEST_ASSERT_EQUAL_UINT64(st.bytes_in - held, first);
    uint64_t emitted = 0;
    for (size_t i = 0; i < k->n; i++) {
        emitted += k->got[i].len;
    }
    TEST_ASSERT_EQUAL_UINT64(st.bytes_in, st.bytes_discarded + emitted + held);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(GNSS_RTCM_SCAN_OPEN, s->n_open);
}

static void feed_pieces(gnss_rtcm_scanner_t *s, const uint8_t *buf, size_t len, size_t piece)
{
    for (size_t i = 0; i < len; i += piece) {
        gnss_rtcm_scan_feed(s, buf + i, (len - i < piece) ? len - i : piece);
        check_accounting(s, &s_sink);
    }
}

TEST_CASE("rtcm scan: the same frames as the framer on every public capture, at any feed size", "[rtcm_scan]")
{
    size_t files = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, fx_index())
    {
        if (cJSON_GetObjectItemCaseSensitive(e, "frames_ok") == NULL) {
            continue;
        }
        files++;
        size_t len;
        uint8_t *buf = fx_stream(e->string, &len);
        static const size_t k_pieces[] = { 1, 7, 100, 1029, 0 };
        for (size_t p = 0; p < sizeof(k_pieces) / sizeof(k_pieces[0]); p++) {
            size_t piece = k_pieces[p] ? k_pieces[p] : (len ? len : 1);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s, pieces of %zu", e->string, piece);
            fx_units_t u;
            fx_frame(buf, len, piece, &u);
            sink_t *k = scan_init(buf);
            feed_pieces(&s_scan, buf, len, piece);
            size_t r = 0;
            for (size_t i = 0; i < u.n; i++) {
                if (u.u[i].kind != GNSS_FRAME_RTCM3) {
                    continue;
                }
                TEST_ASSERT_LESS_THAN_size_t_MESSAGE(k->n, r, msg);
                TEST_ASSERT_EQUAL_UINT64_MESSAGE(u.u[i].off, k->got[r].start, msg);
                TEST_ASSERT_EQUAL_size_t_MESSAGE(u.u[i].len, k->got[r].len, msg);
                r++;
            }
            TEST_ASSERT_EQUAL_size_t_MESSAGE(r, k->n, msg);
            TEST_ASSERT_EQUAL_size_t_MESSAGE((size_t)fx_int(e, "frames_ok"), k->n, msg);
            fx_units_free(&u);
        }
        free(buf);
    }
    TEST_ASSERT_GREATER_OR_EQUAL(25, files);
}

TEST_CASE("rtcm scan: crafted nested lines cost nothing more than plain bytes", "[rtcm_scan]")
{
    /* The input that makes the full framer nest failing lines: '#A'
     * repeated, a bad checksum, 504 bytes as one phone write. */
    static uint8_t crafted[504];
    for (size_t i = 0; i + 5 < sizeof(crafted); i += 2) {
        crafted[i] = '#';
        crafted[i + 1] = 'A';
    }
    memcpy(crafted + sizeof(crafted) - 5, "*FF\r\n", 5);
    static uint8_t plain[504];
    memset(plain, 'x', sizeof(plain));

    gnss_rtcm_scan_stats_t a, b;
    scan_init(NULL);
    gnss_rtcm_scan_feed(&s_scan, crafted, sizeof(crafted));
    gnss_rtcm_scan_get_stats(&s_scan, &a);
    scan_init(NULL);
    gnss_rtcm_scan_feed(&s_scan, plain, sizeof(plain));
    gnss_rtcm_scan_get_stats(&s_scan, &b);
    TEST_ASSERT_EQUAL_MEMORY(&b, &a, sizeof(a));   /* nothing opened, nothing checked: identical */
    TEST_ASSERT_EQUAL_UINT64(0, a.candidates);
    TEST_ASSERT_EQUAL_UINT64(sizeof(crafted), a.bytes_discarded);
    TEST_ASSERT_EQUAL_size_t(0, gnss_rtcm_scan_held(&s_scan, NULL));
}

TEST_CASE("rtcm scan: a flood of false candidates is bounded and a real frame behind it still comes out",
          "[rtcm_scan]")
{
    /* D3 00 D3 00 ...: a header at every other byte (payload 0xD3 = 211). */
    static uint8_t buf[8192 + sizeof(k_v1074)];
    size_t n = 8192;
    for (size_t i = 0; i < n; i += 2) {
        buf[i] = 0xD3;
        buf[i + 1] = 0x00;
    }
    memcpy(buf + n, k_v1074, sizeof(k_v1074));
    size_t len = n + sizeof(k_v1074);
    sink_t *k = scan_init(buf);
    feed_pieces(&s_scan, buf, len, 1);
    gnss_rtcm_scan_stats_t st;
    gnss_rtcm_scan_get_stats(&s_scan, &st);
    /* One header test and at most one closing CRC check per candidate, at
     * most one candidate per two bytes, never more than OPEN open. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(len / 2 + 1, st.candidates);
    TEST_ASSERT_GREATER_THAN_UINT64(0, st.dropped);
    TEST_ASSERT_EQUAL_UINT64(st.candidates, st.crc_errors + st.overlaps + st.dropped + st.rtcm_frames + s_scan.n_open);
    TEST_ASSERT_EQUAL_size_t(1, k->n);
    TEST_ASSERT_EQUAL_UINT64(n, k->got[0].start);
    TEST_ASSERT_EQUAL_size_t(sizeof(k_v1074), k->got[0].len);

    /* D3 03 FF ...: every candidate claims the longest frame. */
    for (size_t i = 0; i + 2 < n; i += 3) {
        buf[i] = 0xD3;
        buf[i + 1] = 0x03;
        buf[i + 2] = 0xFF;
    }
    k = scan_init(buf);
    feed_pieces(&s_scan, buf, len, 64);
    gnss_rtcm_scan_get_stats(&s_scan, &st);
    TEST_ASSERT_EQUAL_size_t(1, k->n);
    TEST_ASSERT_EQUAL_UINT64(n, k->got[0].start);
}

TEST_CASE("rtcm scan: held input and frame offsets, byte by byte; a frame behind a truncated one comes out at once",
          "[rtcm_scan]")
{
    /* 3 noise bytes, the first 20 bytes of a 1074 (cut: its length claims 144),
     * a whole 1005, then the 1074 whole. */
    static uint8_t buf[3 + 20 + sizeof(k_v1005) + sizeof(k_v1074)];
    size_t o = 0;
    memcpy(buf + o, "xyz", 3);
    o += 3;
    memcpy(buf + o, k_v1074, 20);
    o += 20;
    const size_t at1005 = o;
    memcpy(buf + o, k_v1005, sizeof(k_v1005));
    o += sizeof(k_v1005);
    const size_t at1074 = o;
    memcpy(buf + o, k_v1074, sizeof(k_v1074));
    o += sizeof(k_v1074);

    sink_t *k = scan_init(buf);
    for (size_t i = 0; i < o; i++) {
        gnss_rtcm_scan_feed(&s_scan, buf + i, 1);
        check_accounting(&s_scan, k);
        uint64_t first;
        size_t held = gnss_rtcm_scan_held(&s_scan, &first);
        if (i == 2) {
            TEST_ASSERT_EQUAL_size_t(0, held);   /* noise: discarded at once */
        }
        if (i == at1005 - 1) {
            TEST_ASSERT_EQUAL_UINT64(3, first);   /* the cut candidate is still open */
        }
    }
    TEST_ASSERT_EQUAL_size_t(2, k->n);
    TEST_ASSERT_EQUAL_UINT64(at1005, k->got[0].start);
    /* Out with its own last byte, not after the cut candidate failed. */
    TEST_ASSERT_EQUAL_UINT64(at1005 + sizeof(k_v1005), k->got[0].at_bytes_in);
    TEST_ASSERT_EQUAL_UINT64(at1074, k->got[1].start);
    TEST_ASSERT_EQUAL_size_t(0, gnss_rtcm_scan_held(&s_scan, NULL));
    gnss_rtcm_scan_stats_t st;
    gnss_rtcm_scan_get_stats(&s_scan, &st);
    TEST_ASSERT_EQUAL_UINT64(3 + 20, st.bytes_discarded);
    /* Dropped under the 1005: the cut candidate, and a false header inside
     * the 1005's own payload (a 0xD3 at offset 28 claiming 520 bytes). */
    TEST_ASSERT_EQUAL_UINT64(2, st.overlaps);

    /* The framer finds the same two frames, the 1005 only once the cut one failed. */
    fx_units_t u;
    fx_frame(buf, o, 1, &u);
    size_t rt = 0;
    for (size_t i = 0; i < u.n; i++) {
        rt += u.u[i].kind == GNSS_FRAME_RTCM3;
    }
    TEST_ASSERT_EQUAL_size_t(2, rt);
    fx_units_free(&u);
}

TEST_CASE("rtcm scan: reset drops what is held; nothing straddles it", "[rtcm_scan]")
{
    sink_t *k = scan_init(NULL);
    gnss_rtcm_scan_feed(&s_scan, k_v1005, 10);
    TEST_ASSERT_EQUAL_size_t(10, gnss_rtcm_scan_held(&s_scan, NULL));
    gnss_rtcm_scan_reset(&s_scan);
    TEST_ASSERT_EQUAL_size_t(0, gnss_rtcm_scan_held(&s_scan, NULL));
    check_accounting(&s_scan, k);
    gnss_rtcm_scan_feed(&s_scan, k_v1005 + 10, sizeof(k_v1005) - 10);
    TEST_ASSERT_EQUAL_size_t(0, k->n);
    gnss_rtcm_scan_feed(&s_scan, k_v1005, sizeof(k_v1005));
    TEST_ASSERT_EQUAL_size_t(1, k->n);
    TEST_ASSERT_EQUAL_UINT64(sizeof(k_v1005), k->got[0].start);
    check_accounting(&s_scan, k);
    gnss_rtcm_scan_stats_t st;
    gnss_rtcm_scan_get_stats(&s_scan, &st);
    TEST_ASSERT_EQUAL_UINT64(sizeof(k_v1005), st.bytes_discarded);
}

TEST_CASE("rtcm scan: of two overlapping valid frames it keeps the one that ends first (the framer the first)",
          "[rtcm_scan]")
{
    /* SYNTHETIC: a 1005 inside the payload of an outer frame of message 4095
     * whose CRC is computed over it: both CRC-valid, overlapping. */
    static uint8_t payload[2 + 10 + sizeof(k_v1005) + 10];
    memset(payload, 0x55, sizeof(payload));
    payload[0] = 0xFF;   /* message number 4095 */
    payload[1] = 0xF0;
    memcpy(payload + 12, k_v1005, sizeof(k_v1005));
    static uint8_t outer[sizeof(payload) + 6];
    size_t olen = rtcm_make_frame(outer, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_size_t(sizeof(outer), olen);
    TEST_ASSERT_TRUE(gnss_rtcm3_frame_ok(outer, olen));

    sink_t *k = scan_init(outer);
    gnss_rtcm_scan_feed(&s_scan, outer, olen);
    TEST_ASSERT_EQUAL_size_t(1, k->n);
    TEST_ASSERT_EQUAL_UINT64(3 + 12, k->got[0].start);   /* the inner 1005 */
    check_accounting(&s_scan, k);

    fx_units_t u;
    fx_frame(outer, olen, 0, &u);
    TEST_ASSERT_EQUAL_size_t(1, u.n);
    TEST_ASSERT_EQUAL_size_t(olen, u.u[0].len);   /* the framer: the outer one */
    fx_units_free(&u);
}

TEST_CASE("rtcm scan: CRC continued over pieces equals the CRC of the whole", "[rtcm_scan]")
{
    uint32_t c = 0;
    c = gnss_rtcm3_crc24q_update(c, k_v1074, 50);
    c = gnss_rtcm3_crc24q_update(c, k_v1074 + 50, sizeof(k_v1074) - 50);
    TEST_ASSERT_EQUAL_HEX32(gnss_rtcm3_crc24q(k_v1074, sizeof(k_v1074)), c);
    TEST_ASSERT_EQUAL_HEX32(0, c);   /* a whole frame, its CRC included */
}

/* ---- gnss_framer: held input and unit offsets ------------------------------- */

typedef struct {
    gnss_framer_t f;
    const uint8_t *buf;
    uint64_t covered;       /* bytes in emitted units, terminators included */
    bool cr_line;           /* the last unit was a line that ended at a CR, its LF not fed yet */
    uint64_t cr_at;
    uint64_t due[16];       /* LFs of CR LFs fed already, scanned later in the same feed */
    size_t n_due;
} acct_t;

/* The due LFs before `at` have been scanned: they joined their lines. */
static void settle_due(acct_t *a, uint64_t at)
{
    for (size_t i = 0; i < a->n_due;) {
        if (a->due[i] < at) {
            a->covered++;
            a->due[i] = a->due[--a->n_due];
        } else {
            i++;
        }
    }
}

static void check_identity(acct_t *a, const char *what)
{
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&a->f, &st);
    uint64_t first;
    size_t held = gnss_framer_held(&a->f, &first);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(st.bytes_in - held, first, what);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(st.bytes_in, st.bytes_discarded + a->covered + held, what);
}

static acct_t s_acct;

static void on_unit_acct(const gnss_frame_t *fr, void *ctx)
{
    acct_t *a = ctx;
    if (fr->kind == GNSS_FRAME_PROMPT) {
        return;
    }
    TEST_ASSERT_EQUAL_MEMORY(a->buf + fr->start, fr->data, fr->len);
    settle_due(a, fr->start);
    a->covered += fr->len;
    a->cr_line = false;
    if (fr->kind == GNSS_FRAME_TEXT) {
        uint64_t term = fr->start + fr->len;   /* the byte that ended it, fed already */
        a->covered += 1;
        if (a->buf[term] == '\r') {
            /* Its LF, if one follows, belongs to it: fed already (a line out
             * of a rescan: still waiting, held, until scanned) or the next. */
            gnss_framing_stats_t st;
            gnss_framer_get_stats(&a->f, &st);
            if (term + 1 < st.bytes_in) {
                if (a->buf[term + 1] == '\n') {
                    TEST_ASSERT_LESS_THAN_size_t(16, a->n_due);
                    a->due[a->n_due++] = term + 1;
                }
            } else {
                a->cr_line = true;
                a->cr_at = term;
            }
        }
    }
    check_identity(a, "inside the callback");
}

/* Feed one byte at a time; after each: bytes_in == discarded + covered + held. */
static void framer_account(const uint8_t *buf, size_t len, const char *what)
{
    acct_t *a = &s_acct;
    memset(a, 0, sizeof(*a));
    a->buf = buf;
    gnss_framer_init(&a->f, on_unit_acct, a);
    for (size_t i = 0; i < len; i++) {
        bool lf_of_crlf = buf[i] == '\n' && a->cr_line && a->cr_at + 1 == i;
        gnss_framer_feed(&a->f, buf + i, 1);
        if (lf_of_crlf) {
            a->covered += 1;   /* the LF of a CR LF belongs to its line */
            a->cr_line = false;
        }
        settle_due(a, UINT64_MAX);   /* every rescan was drained before the feed returned */
        check_identity(a, what);
    }
}

TEST_CASE("framer: held input accounts for every byte, rescans, CR LF and skips included", "[gnss_framing]")
{
    /* Every public capture. */
    const cJSON *e;
    cJSON_ArrayForEach(e, fx_index())
    {
        size_t len;
        uint8_t *buf = fx_stream(e->string, &len);
        framer_account(buf, len, e->string);
        free(buf);
    }
    /* SYNTHETIC: nested failing lines, an overlong line, a binary log, a cut
     * frame before a whole one, CR LF and bare LF lines. */
    static uint8_t buf[2048];
    size_t o = 0;
    for (int i = 0; i < 40; i++) {
        buf[o++] = '#';
        buf[o++] = 'A';
    }
    memcpy(buf + o, "*FF\r\n", 5);
    o += 5;
    memset(buf + o, 'L', 600);   /* longer than GNSS_TEXT_MAX_LINE */
    buf[o] = '$';
    o += 600;
    buf[o++] = '\r';
    buf[o++] = '\n';
    static const uint8_t bin[24 + 4] = { 0xAA, 0x44, 0xB5, 0, 0, 0, 0, 0 };   /* body length 0 */
    memcpy(buf + o, bin, sizeof(bin));
    o += sizeof(bin);
    memcpy(buf + o, k_v1074, 20);
    o += 20;
    memcpy(buf + o, k_v1005, sizeof(k_v1005));
    o += sizeof(k_v1005);
    static const char lines[] = "$devicename,COM1*67\r\n$devicename,COM1*67\n$devicename,COM1*67\r";
    memcpy(buf + o, lines, sizeof(lines) - 1);
    o += sizeof(lines) - 1;
    const size_t before_filler = o;
    memset(buf + o, 'x', 100);   /* takes the cut 1074 past its claimed 144 bytes */
    o += 100;
    TEST_ASSERT_LESS_THAN_size_t(sizeof(buf), o);
    framer_account(buf, before_filler, "synthetic, the cut frame still open");
    /* The cut 1074 still holds the 1005 and the lines behind it. */
    TEST_ASSERT_EQUAL_size_t(20 + sizeof(k_v1005) + sizeof(lines) - 1, gnss_framer_held(&s_acct.f, NULL));
    framer_account(buf, o, "synthetic");
    /* Once it failed, everything behind it was scanned again: nothing held. */
    TEST_ASSERT_EQUAL_size_t(0, gnss_framer_held(&s_acct.f, NULL));
}

TEST_CASE("framer: a CR LF line out of a rescan: its LF is held in the callback, then joins it", "[gnss_framing]")
{
    /* A false RTCM candidate whose rescan yields "$A*41\r\n" (SYNTHETIC). */
    static const uint8_t in[] = { 0xD3, 0x00, 0x0A, '$', 'A', '*', '4', '1', '\r', '\n', 0, 0, 0, 0, 0, 0 };
    framer_account(in, sizeof(in), "rescan CR LF");
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&s_acct.f, &st);
    TEST_ASSERT_EQUAL_UINT64(1, st.text_lines);
    TEST_ASSERT_EQUAL_size_t(0, gnss_framer_held(&s_acct.f, NULL));
}

TEST_CASE("framer: held input is the unit being collected, from its first byte", "[gnss_framing]")
{
    acct_t *a = &s_acct;
    memset(a, 0, sizeof(*a));
    a->buf = k_v1074;
    gnss_framer_init(&a->f, on_unit_acct, a);
    uint64_t first = 99;
    TEST_ASSERT_EQUAL_size_t(0, gnss_framer_held(&a->f, &first));
    TEST_ASSERT_EQUAL_UINT64(0, first);
    gnss_framer_feed(&a->f, k_v1074, 100);
    TEST_ASSERT_EQUAL_size_t(100, gnss_framer_held(&a->f, &first));
    TEST_ASSERT_EQUAL_UINT64(0, first);
    gnss_framer_feed(&a->f, k_v1074 + 100, sizeof(k_v1074) - 100);
    TEST_ASSERT_EQUAL_size_t(0, gnss_framer_held(&a->f, &first));
    TEST_ASSERT_EQUAL_UINT64(sizeof(k_v1074), first);
    TEST_ASSERT_EQUAL_UINT64(sizeof(k_v1074), a->covered);
    /* A reset drops it: counted as discarded, no longer held. */
    gnss_framer_feed(&a->f, k_v1074, 10);
    gnss_framer_reset(&a->f);
    TEST_ASSERT_EQUAL_size_t(0, gnss_framer_held(&a->f, NULL));
    gnss_framing_stats_t st;
    gnss_framer_get_stats(&a->f, &st);
    TEST_ASSERT_EQUAL_UINT64(10, st.bytes_discarded);
}
