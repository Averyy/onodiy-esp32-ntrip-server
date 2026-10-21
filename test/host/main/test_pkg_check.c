/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver package rules and the LZO1X decoder. The real package in
 * firmware/um982/ (a data file) must pass every rule; one mutated copy per
 * rule must fail exactly that rule. A small synthetic package (built here,
 * literal-only compressed blocks) covers what the real one cannot: an exact
 * multiple of 1024 bytes and the decoder's error paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fl_crc.h"
#include "lzo1x.h"
#include "pkg_check.h"
#include "unity.h"

#ifndef FL_TEST_PKG_PATH
#define FL_TEST_PKG_PATH ""
#endif

#define REAL_BUILD 18975u

static uint8_t *s_work;

static uint8_t *work(void)
{
    if (s_work == NULL) {
        s_work = malloc(PKG_LZO_BLOCK_MAX);
    }
    return s_work;
}

static uint8_t *load_real(uint32_t *len)
{
    FILE *f = fopen(FL_TEST_PKG_PATH, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

/* SHA-256 is the caller's job: the tests pass a digest and its copy. */
static const uint8_t k_sha[32] = { 0x9a, 0xc2, 0x5d, 0x1d };
static const uint8_t k_sha_other[32] = { 0x01 };

static pkg_check_in_t inputs(uint32_t len)
{
    pkg_check_in_t in = {
        .live_model = "UM982",
        .receiver_build = REAL_BUILD,
        .package_build = REAL_BUILD,
        .expected_size = len,
        .expected_sha256 = k_sha,
        .actual_sha256 = k_sha,
        .cross_layout_ack = false,
        .cross_layout_allowed = false,
        .recovery = false,
        .work = work(),
        .work_cap = PKG_LZO_BLOCK_MAX,
    };
    return in;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Header offsets of the five blocks. */
static void block_offsets(const uint8_t *p, uint32_t off[5])
{
    uint32_t o = 0;
    for (int i = 0; i < 5; i++) {
        off[i] = o;
        o += 32u + fl_le32(p + o + 0x0C);
    }
}

/* Recompute a block's payload sum and header CRC after an edit. */
static void reseal(uint8_t *p, uint32_t hdr)
{
    uint32_t size = fl_le32(p + hdr + 0x0C);
    put32(p + hdr + 0x18, fl_sum32_le(p + hdr + 32, size));
    uint16_t crc = fl_crc16_xmodem(p + hdr, 0x1C);
    p[hdr + 0x1C] = (uint8_t)crc;
    p[hdr + 0x1D] = (uint8_t)(crc >> 8);
}

/* Recompute a compressed block's trailer (sum and check word) after an edit. */
static void retrail(uint8_t *p, uint32_t hdr)
{
    uint32_t size = fl_le32(p + hdr + 0x0C);
    uint8_t *pl = p + hdr + 32;
    uint32_t l = size - 16u;
    uint32_t w0 = fl_le32(pl + l), w1 = fl_le32(pl + l + 4);
    uint32_t w2 = fl_sum32_le(pl, l);
    put32(pl + l + 8, w2);
    put32(pl + l + 12, w0 + w1 + w2);
}

static uint8_t run(const uint8_t *p, uint32_t len, pkg_check_out_t *out)
{
    pkg_check_in_t in = inputs(len);
    return pkg_check(p, len, &in, out);
}

/* ---- LZO1X --------------------------------------------------------------------- */

TEST_CASE("lzo1x: literal-only stream and end marker", "[pkg]")
{
    uint8_t in[16] = { 17 + 5, 'h', 'e', 'l', 'l', 'o', 0x11, 0x00, 0x00 };
    uint8_t out[16];
    size_t n = 0;
    TEST_ASSERT_EQUAL(LZO1X_OK, lzo1x_decompress(in, 9, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_MEMORY("hello", out, 5);
    /* Empty stream: only the end marker. */
    TEST_ASSERT_EQUAL(LZO1X_OK, lzo1x_decompress(in + 6, 3, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

TEST_CASE("lzo1x: matches of every form, overlapping copies", "[pkg]")
{
    /* "abcd" literals (first byte 21 -> 4, state 4), then a 64..127 match
     * of length 3 at distance 4 ("abc"), then a 32..63 match of length 8
     * at distance 1 (run of 'c'), then end. */
    uint8_t in[] = {
        17 + 4, 'a', 'b', 'c', 'd',
        (uint8_t)(0x40 | (3u << 2) | 0), 0x00,   /* 010 LDDD SS: len 3, dist 1 + 3 + (0 << 3) = 4 */
        (uint8_t)(0x20 | 6u), 0x00, 0x00,        /* len 6 + 2 = 8, dist 1 + 0 */
        0x11, 0x00, 0x00,
    };
    uint8_t out[32];
    size_t n = 0;
    TEST_ASSERT_EQUAL(LZO1X_OK, lzo1x_decompress(in, sizeof(in), out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(4 + 3 + 8, n);
    TEST_ASSERT_EQUAL_MEMORY("abcdabccccccccc", out, n);

    /* A long literal run with a length extension (state 0: first byte 0). */
    uint8_t in2[400];
    size_t k = 0;
    in2[k++] = 0x00;          /* literal run, extended */
    in2[k++] = 0x00;          /* +255 */
    in2[k++] = 2;             /* 15 + 255 + 2 = 272, + 3 = 275 literals */
    for (int i = 0; i < 275; i++) {
        in2[k++] = (uint8_t)i;
    }
    in2[k++] = 0x11;
    in2[k++] = 0;
    in2[k++] = 0;
    uint8_t out2[400];
    TEST_ASSERT_EQUAL(LZO1X_OK, lzo1x_decompress(in2, k, out2, sizeof(out2), &n));
    TEST_ASSERT_EQUAL_size_t(275, n);
    TEST_ASSERT_EQUAL_UINT8(274 & 0xFF, out2[274]);

    /* After a literal run of four or more (state 4) a 0..15 opcode copies 3
     * bytes from 2049 + D + (H << 2); after 1..3 trailing literals it
     * copies 2 bytes from 1 + D + (H << 2). */
    static uint8_t big[2200];
    size_t m = 0;
    big[m++] = 0x00;          /* literal run: 15 + 255 * 8 + 42 + 3 = 2100 */
    for (int z = 0; z < 8; z++) {
        big[m++] = 0x00;
    }
    big[m++] = 42;
    for (int i = 0; i < 2100; i++) {
        big[m++] = (uint8_t)(i * 7);
    }
    big[m++] = 0x01;          /* state 4: 3 bytes from distance 2049, then 1 literal */
    big[m++] = 0x00;
    big[m++] = 'Z';
    big[m++] = 0x04;          /* state 1: 2 bytes from distance 1 + 1 = 2 */
    big[m++] = 0x00;
    big[m++] = 0x11;
    big[m++] = 0;
    big[m++] = 0;
    static uint8_t bout[2200];
    TEST_ASSERT_EQUAL(LZO1X_OK, lzo1x_decompress(big, m, bout, sizeof(bout), &n));
    TEST_ASSERT_EQUAL_size_t(2106, n);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(51 * 7), bout[2100]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(52 * 7), bout[2101]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(53 * 7), bout[2102]);
    TEST_ASSERT_EQUAL_UINT8('Z', bout[2103]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(53 * 7), bout[2104]);
    TEST_ASSERT_EQUAL_UINT8('Z', bout[2105]);

    /* A 16..31 match (distance 16384 + ...) needs that much output first. */
    uint8_t far[] = { 17 + 1, 'q', 0x11 | 0x08, 0x00, 0x00 };   /* H = 1: distance 32768, not the end marker */
    TEST_ASSERT_EQUAL(LZO1X_LOOKBEHIND, lzo1x_decompress(far, sizeof(far), out2, sizeof(out2), &n));
}

TEST_CASE("lzo1x: damaged streams fail without leaving their buffers", "[pkg]")
{
    uint8_t out[8];
    size_t n = 0;
    uint8_t lookbehind[] = { 17 + 1, 'a', 0x20 | 2u, 0x10, 0x00, 0x11, 0, 0 };   /* distance 5 > 1 byte of output */
    TEST_ASSERT_EQUAL(LZO1X_LOOKBEHIND, lzo1x_decompress(lookbehind, sizeof(lookbehind), out, sizeof(out), &n));
    uint8_t overrun_in[] = { 17 + 9, 'a', 'b' };
    TEST_ASSERT_EQUAL(LZO1X_INPUT_OVERRUN, lzo1x_decompress(overrun_in, sizeof(overrun_in), out, sizeof(out), &n));
    uint8_t overrun_out[] = { 17 + 9, '1', '2', '3', '4', '5', '6', '7', '8', '9', 0x11, 0, 0 };
    TEST_ASSERT_EQUAL(LZO1X_OUTPUT_OVERRUN, lzo1x_decompress(overrun_out, sizeof(overrun_out), out, sizeof(out), &n));
    uint8_t left[] = { 17 + 1, 'a', 0x11, 0, 0, 0x55 };
    TEST_ASSERT_EQUAL(LZO1X_INPUT_LEFT, lzo1x_decompress(left, sizeof(left), out, sizeof(out), &n));
    uint8_t noend[] = { 17 + 1, 'a' };
    TEST_ASSERT_EQUAL(LZO1X_NO_END, lzo1x_decompress(noend, sizeof(noend), out, sizeof(out), &n));
    uint8_t ext_overrun[] = { 0x00, 0x00, 0x00 };
    TEST_ASSERT_EQUAL(LZO1X_INPUT_OVERRUN, lzo1x_decompress(ext_overrun, sizeof(ext_overrun), out, sizeof(out), &n));
}

TEST_CASE("checksums: CRC-16/XMODEM check value, Adler-32, sums", "[pkg]")
{
    TEST_ASSERT_EQUAL_HEX16(0x31C3, fl_crc16_xmodem((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x091E01DE, fl_adler32(1, (const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(1, fl_adler32(1, NULL, 0));
    static uint8_t ff[20000];
    memset(ff, 0xFF, sizeof(ff));
    /* Long input across the modulo boundary (reference: zlib.adler32). */
    TEST_ASSERT_EQUAL_HEX32(0x9F51D664, fl_adler32(1, ff, sizeof(ff)));
    uint8_t w[8] = { 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_HEX32(0, fl_sum32_le(w, 8));
    TEST_ASSERT_EQUAL_HEX8(0xFE, fl_sum8(w + 4, 2));
}

/* ---- the real package ---------------------------------------------------------- */

TEST_CASE("package rules: the real UM982 package passes all twelve", "[pkg]")
{
    uint32_t len = 0;
    uint8_t *p = load_real(&len);
    if (p == NULL) {
        TEST_IGNORE_MESSAGE("firmware/um982 package not found");
    }
    pkg_check_out_t out;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, run(p, len, &out), out.why);
    TEST_ASSERT_EQUAL_UINT32(19u + 49u, out.lzo_blocks);
    TEST_ASSERT_FALSE(out.cross_layout);
    TEST_ASSERT_EQUAL_STRING("UM982", out.facts.model);
    TEST_ASSERT_EQUAL_INT8(1, out.facts.layout);
    TEST_ASSERT_EQUAL_UINT32(18975, out.facts.layout_build);
    TEST_ASSERT_TRUE(out.facts.chain_ok);
    TEST_ASSERT_EQUAL_HEX32(0x00033450u, out.facts.stamps[0]);
    TEST_ASSERT_EQUAL_HEX32(0x24122417u, out.facts.stamps[1]);
    TEST_ASSERT_EQUAL_HEX32(0x25040914u, out.facts.stamps[4]);

    /* The same facts through the reader (upload page). */
    pkg_facts_t f;
    pkg_inspect_mem(p, len, &f);
    TEST_ASSERT_EQUAL_STRING("UM982", f.model);
    TEST_ASSERT_EQUAL_INT8(1, f.layout);

    /* Recovery gate: rule 12 is not run (a lower receiver build passes). */
    pkg_check_in_t in = inputs(len);
    in.recovery = true;
    in.receiver_build = 99999;
    TEST_ASSERT_EQUAL_UINT8(0, pkg_check(p, len, &in, &out));
    free(p);
}

TEST_CASE("package rules: one mutated copy per rule fails exactly that rule", "[pkg]")
{
    uint32_t len = 0;
    uint8_t *orig = load_real(&len);
    if (orig == NULL) {
        TEST_IGNORE_MESSAGE("firmware/um982 package not found");
    }
    uint8_t *p = malloc(len + 4096);
    uint32_t off[5];
    block_offsets(orig, off);
    pkg_check_out_t out;

    /* 1: wrong magic of the second block; a trailing byte; k2 size. */
    memcpy(p, orig, len);
    p[off[1]] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(1, run(p, len, &out));
    memcpy(p, orig, len);
    p[len] = 0;
    TEST_ASSERT_EQUAL_UINT8(1, run(p, len + 1, &out));
    memcpy(p, orig, len);
    put32(p + off[2], 0xA0A1A2A3u + 4u * 0x01010101u);   /* type 4 where type 3 belongs */
    TEST_ASSERT_EQUAL_UINT8(1, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + off[3] + 0x0C, fl_le32(p + off[3] + 0x0C) - 2);   /* size not a multiple of 4 */
    TEST_ASSERT_EQUAL_UINT8(1, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + 0x00 + 0x0C, 0xA0);
    TEST_ASSERT_EQUAL_UINT8(1, run(p, len, &out));

    /* 2: header CRC, and the zero byte at +0x1E. */
    memcpy(p, orig, len);
    p[off[4] + 0x1C] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(2, run(p, len, &out));
    memcpy(p, orig, len);
    p[off[4] + 0x1E] = 1;
    TEST_ASSERT_EQUAL_UINT8(2, run(p, len, &out));

    /* 3: flash address, load address, attributes (headers resealed). */
    memcpy(p, orig, len);
    put32(p + off[2] + 0x04, 0x00031000u);
    reseal(p, off[2]);
    TEST_ASSERT_EQUAL_UINT8(3, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + off[4] + 0x08, 0x10001000u);
    reseal(p, off[4]);
    TEST_ASSERT_EQUAL_UINT8(3, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + off[1] + 0x14, 0x07u);
    reseal(p, off[1]);
    TEST_ASSERT_EQUAL_UINT8(3, run(p, len, &out));

    /* 4: a payload byte of the type-4 block (no trailer, not compressed). */
    memcpy(p, orig, len);
    p[off[3] + 32 + 100] ^= 0x40;
    TEST_ASSERT_EQUAL_UINT8(4, run(p, len, &out));

    /* 5: trailer words, with the block sum fixed so rules 1-4 pass. */
    memcpy(p, orig, len);
    {
        uint32_t size = fl_le32(p + off[1] + 0x0C);
        uint8_t *t = p + off[1] + 32 + size - 16;
        put32(t + 4, 0x131);
        reseal(p, off[1]);
    }
    TEST_ASSERT_EQUAL_UINT8(5, run(p, len, &out));
    memcpy(p, orig, len);
    {
        uint32_t size = fl_le32(p + off[4] + 0x0C);
        uint8_t *t = p + off[4] + 32 + size - 16;
        put32(t + 12, fl_le32(t + 12) + 1);
        reseal(p, off[4]);
    }
    TEST_ASSERT_EQUAL_UINT8(5, run(p, len, &out));

    /* 6: two words swapped inside the compressed data: every additive check
     * (block sum, trailer sum) still passes; only decompression catches it. */
    memcpy(p, orig, len);
    {
        uint8_t *a = p + off[4] + 32 + 0x1000;
        uint8_t *b = p + off[4] + 32 + 0x1010;
        uint8_t tmp[4];
        memcpy(tmp, a, 4);
        memcpy(a, b, 4);
        memcpy(b, tmp, 4);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(6, run(p, len, &out), out.why);
    /* 6: the container's header checksum. */
    memcpy(p, orig, len);
    p[off[2] + 32 + 0x2C] ^= 1;
    retrail(p, off[2]);
    reseal(p, off[2]);
    TEST_ASSERT_EQUAL_UINT8(6, run(p, len, &out));
    /* 6: a nonzero alignment pad byte, or a terminator that ends early. */
    memcpy(p, orig, len);
    {
        uint8_t *pl = p + off[2] + 32;
        put32be(pl + 9 + 6 + 2, fl_be32(pl + 9 + 6 + 2) | 0x00000800u);   /* F_H_FILTER */
        put32be(pl + 0x2C, fl_adler32(1, pl + 9, 0x2C - 9));
        retrail(p, off[2]);
        reseal(p, off[2]);
    }
    TEST_ASSERT_EQUAL_UINT8(6, run(p, len, &out));
    /* 6: a work buffer smaller than a block is refused, never overrun. */
    {
        pkg_check_in_t in = inputs(len);
        in.work_cap = 4096;
        TEST_ASSERT_EQUAL_UINT8(6, pkg_check(orig, len, &in, &out));
    }

    /* 7: loader baud and an unknown partition table (block resealed). */
    memcpy(p, orig, len);
    put32(p + 0x20, 115200u);
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(7, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + 0x30 + 8 * 5 + 4, 0x00020000u);   /* type-5 backup address: not a flashed type, still a different table */
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(7, run(p, len, &out));
    TEST_ASSERT_EQUAL_INT8(-1, out.facts.layout);

    /* 8: model string variants and the live model. */
    memcpy(p, orig, len);
    memcpy(p + 0x9C, "UM982C", 7);
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(8, run(p, len, &out));
    memcpy(p, orig, len);
    memcpy(p + 0x9C, "um982", 6);
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(8, run(p, len, &out));
    memcpy(p, orig, len);
    memset(p + 0x9C, 'U', 16);   /* no terminator */
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(8, run(p, len, &out));
    {
        pkg_check_in_t in = inputs(len);
        in.live_model = "UM980";
        TEST_ASSERT_EQUAL_UINT8(8, pkg_check(orig, len, &in, &out));
        in.live_model = "UM982C";
        TEST_ASSERT_EQUAL_UINT8(8, pkg_check(orig, len, &in, &out));
        in.live_model = NULL;
        TEST_ASSERT_EQUAL_UINT8(8, pkg_check(orig, len, &in, &out));
    }

    /* 9: product tags (header byte, boot-cfg byte, boot-cfg word). */
    memcpy(p, orig, len);
    p[off[3] + 0x1F] = 0x41;
    TEST_ASSERT_EQUAL_UINT8(9, run(p, len, &out));
    memcpy(p, orig, len);
    p[0x7F] = 0x41;
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(9, run(p, len, &out));
    memcpy(p, orig, len);
    put32(p + 0xAC, 0);
    reseal(p, 0);
    TEST_ASSERT_EQUAL_UINT8(9, run(p, len, &out));

    /* 11: size and digest. */
    {
        pkg_check_in_t in = inputs(len);
        in.expected_size = len + 4;
        TEST_ASSERT_EQUAL_UINT8(11, pkg_check(orig, len, &in, &out));
        in = inputs(len);
        in.actual_sha256 = k_sha_other;
        TEST_ASSERT_EQUAL_UINT8(11, pkg_check(orig, len, &in, &out));
        in.actual_sha256 = NULL;
        TEST_ASSERT_EQUAL_UINT8(11, pkg_check(orig, len, &in, &out));
    }

    /* 12: downgrade, unknown builds, cross-layout. */
    {
        pkg_check_in_t in = inputs(len);
        in.receiver_build = REAL_BUILD + 1;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        in = inputs(len);
        in.package_build = 0;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        in = inputs(len);
        in.receiver_build = 0;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        /* Receiver on the older layout: cross-layout, refused until allowed, then needs the confirmation. */
        in = inputs(len);
        in.receiver_build = 13495;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        TEST_ASSERT_TRUE(out.cross_layout);
        in.cross_layout_allowed = true;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        in.cross_layout_ack = true;
        TEST_ASSERT_EQUAL_UINT8(0, pkg_check(orig, len, &in, &out));
        /* An unknown current build counts as a different layout. */
        in = inputs(len);
        in.receiver_build = 17000;
        TEST_ASSERT_EQUAL_UINT8(12, pkg_check(orig, len, &in, &out));
        TEST_ASSERT_TRUE(out.cross_layout);
        /* A receiver on 23575, whose table is 18975's: the same layout, not a cross-layout flash. */
        in = inputs(len);
        in.receiver_build = 23575;
        in.package_build = 24000;
        TEST_ASSERT_EQUAL_UINT8(0, pkg_check(orig, len, &in, &out));
        TEST_ASSERT_FALSE(out.cross_layout);
    }
    free(p);
    free(orig);
}

TEST_CASE("package rules: the 23575 package passes all twelve on 18975 and 23575, never over 25164", "[pkg]")
{
    FILE *f = fopen(FL_TEST_PKG23575_PATH, "rb");
    if (f == NULL) {
        TEST_IGNORE_MESSAGE("firmware/um982 23575 package not found");
    }
    fseek(f, 0, SEEK_END);
    uint32_t len = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(len, fread(p, 1, len, f));
    fclose(f);
    pkg_facts_t facts;
    pkg_inspect_mem(p, len, &facts);
    TEST_ASSERT_TRUE(facts.chain_ok);
    TEST_ASSERT_EQUAL_STRING("UM982", facts.model);
    TEST_ASSERT_EQUAL_UINT32(18975, facts.layout_build);                  /* 18975's table (the first with it) */
    TEST_ASSERT_EQUAL_HEX32(0x26021116u, facts.stamps[4]);                /* built 2026-02-11 */
    static const uint32_t ok_on[] = { 18975, 23575 };
    for (size_t i = 0; i < sizeof(ok_on) / sizeof(ok_on[0]); i++) {
        pkg_check_in_t in = inputs(len);
        in.receiver_build = ok_on[i];
        in.package_build = 23575;
        pkg_check_out_t out;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, pkg_check(p, len, &in, &out), out.why);
        TEST_ASSERT_FALSE(out.cross_layout);
        TEST_ASSERT_EQUAL_UINT32(19u + 49u, out.lzo_blocks);
    }
    pkg_check_in_t in = inputs(len);
    in.receiver_build = 25164;                                            /* what boards ship with: never downgraded */
    in.package_build = 23575;
    pkg_check_out_t out;
    TEST_ASSERT_EQUAL_UINT8(12, pkg_check(p, len, &in, &out));
    free(p);
}

/* ---- a synthetic package --------------------------------------------------------- */

/* LZO1X stream of n bytes of "abcd" repeated (n a multiple of 4, >= 8):
 * four literals, one match from distance 4, the end marker. */
static size_t lzo_abcd(uint8_t *out, uint32_t n, uint8_t *plain)
{
    size_t k = 0;
    out[k++] = 17 + 4;
    memcpy(out + k, "abcd", 4);
    k += 4;
    uint32_t l = n - 4u - 2u;
    if (l <= 31u) {
        out[k++] = (uint8_t)(0x20u | l);
    } else {
        out[k++] = 0x20;
        uint32_t rem = l - 31u;
        while (rem > 255u) {
            out[k++] = 0;
            rem -= 255u;
        }
        out[k++] = (uint8_t)rem;
    }
    out[k++] = (uint8_t)((4u - 1u) << 2);
    out[k++] = 0;
    out[k++] = 0x11;
    out[k++] = 0;
    out[k++] = 0;
    for (uint32_t i = 0; i < n; i++) {
        plain[i] = (uint8_t)("abcd"[i % 4u]);
    }
    return k;
}

/* One lzop container: a compressed block of n_lzo bytes, then (if n_stored)
 * a stored block of n_stored bytes (its slen equals its dlen). */
static size_t lzop_payload(uint8_t *pl, uint32_t n_lzo, uint32_t n_stored)
{
    static const uint8_t magic[9] = { 0x89, 0x4C, 0x5A, 0x4F, 0x00, 0x0D, 0x0A, 0x1A, 0x0A };
    size_t k = 0;
    memcpy(pl, magic, 9);
    k = 9;
    pl[k++] = 0x10; pl[k++] = 0x40;   /* version */
    pl[k++] = 0x20; pl[k++] = 0xA0;   /* library */
    pl[k++] = 0x09; pl[k++] = 0x40;   /* needed */
    pl[k++] = 3;                      /* method */
    pl[k++] = 9;                      /* level */
    put32be(pl + k, 0x0E300031u);     /* flags: Adler-32 of the data only */
    k += 4;
    memset(pl + k, 0, 12);            /* mode, mtime */
    k += 12;
    pl[k++] = 10;
    memcpy(pl + k, "aaaaaaaaaa", 10);
    k += 10;
    put32be(pl + k, fl_adler32(1, pl + 9, k - 9));
    k += 4;
    /* block: dlen, slen, adler, data, pad */
    uint8_t plain[600];
    uint8_t comp[64];
    size_t clen = lzo_abcd(comp, n_lzo, plain);
    put32be(pl + k, n_lzo);
    put32be(pl + k + 4, (uint32_t)clen);
    put32be(pl + k + 8, fl_adler32(1, plain, n_lzo));
    k += 12;
    memcpy(pl + k, comp, clen);
    k += clen;
    while (k % 4 != 0) {
        pl[k++] = 0;
    }
    if (n_stored > 0) {
        for (uint32_t i = 0; i < n_stored; i++) {
            plain[i] = (uint8_t)(i * 11u);
        }
        put32be(pl + k, n_stored);
        put32be(pl + k + 4, n_stored);
        put32be(pl + k + 8, fl_adler32(1, plain, n_stored));
        k += 12;
        memcpy(pl + k, plain, n_stored);
        k += n_stored;
        while (k % 4 != 0) {
            pl[k++] = 0;
        }
    }
    put32be(pl + k, 0);
    k += 4;
    return k;
}

/* Header + payload for type k at *pos; returns the payload pointer. */
static uint8_t *block(uint8_t *p, uint32_t *pos, uint8_t k, uint32_t size, uint32_t load, uint32_t attr, uint8_t tag)
{
    uint8_t *h = p + *pos;
    memset(h, 0, 32);
    put32(h, 0xA0A1A2A3u + k * 0x01010101u);
    put32(h + 0x08, load);
    put32(h + 0x0C, size);
    put32(h + 0x10, 0x25040914u);
    put32(h + 0x14, attr);
    h[0x1F] = tag;
    *pos += 32u + size;
    return h + 32;
}

/* A small package that passes rules 1-9 (and 10 unless k4_size makes it a multiple of 1024). */
static uint32_t build_pkg(uint8_t *p, uint32_t k4_size)
{
    uint32_t pos = 0;
    const pkg_layout_t *lay = &pkg_known_layouts[1];
    uint8_t *cfg = block(p, &pos, 2, 0x9C, 0, 0, 0x40);
    memset(cfg, 0, 0x9C);
    put32(cfg, 460800u);
    put32(cfg + 4, 2);
    put32(cfg + 8, 5);
    for (int k = 0; k < 9; k++) {
        put32(cfg + 0x10 + 8 * k, lay->a[k]);
        put32(cfg + 0x14 + 8 * k, lay->b[k]);
    }
    put32(cfg + 0x5C, 0x4000009Cu);
    memcpy(cfg + 0x7C, "UM982", 6);
    put32(cfg + 0x8C, 660000000u);
    /* k0: payload with its trailer */
    uint32_t k0 = 64;
    uint8_t *b0 = block(p, &pos, 0, k0, 0x00440000u, 3, 0x00);
    memset(b0, 0x5A, k0);
    put32(b0 + k0 - 16, 0xB0B10003u);
    put32(b0 + k0 - 12, 0x130u);
    put32(b0 + k0 - 8, k0);
    put32(b0 + k0 - 4, 0xB0B10003u + 0x130u + k0);
    /* k3, k6: lzop containers + trailer */
    for (int t = 0; t < 2; t++) {
        uint8_t k = t == 0 ? 3 : 6;
        uint8_t tmp[800];
        size_t l = t == 0 ? lzop_payload(tmp, 120, 0) : lzop_payload(tmp, 560, 37);
        uint32_t size = (uint32_t)l + 16u;
        uint8_t *pl = block(p, &pos, k, size, k == 3 ? 0x00200000u : 0x10000000u, 0xE0, 0x40);
        memcpy(pl, tmp, l);
        put32(pl + l, 0xB1B2F000u);
        put32(pl + l + 4, (uint32_t)l);
        put32(pl + l + 8, fl_sum32_le(pl, l));
        put32(pl + l + 12, 0xB1B2F000u + (uint32_t)l + fl_sum32_le(pl, l));
        if (t == 0) {
            /* k4 goes between k3 and k6 */
            uint8_t *b4 = block(p, &pos, 4, k4_size, 0, 0, 0x40);
            for (uint32_t i = 0; i < k4_size; i++) {
                b4[i] = (uint8_t)(i ^ 0xA5);
            }
        }
    }
    /* Flash addresses from the table, then seal every header. */
    uint32_t off[5];
    block_offsets(p, off);
    static const uint8_t order[5] = { 2, 0, 3, 4, 6 };
    for (int i = 0; i < 5; i++) {
        put32(p + off[i] + 0x04, lay->a[order[i]]);
        reseal(p, off[i]);
    }
    return pos;
}

TEST_CASE("package rules: a synthetic package passes, an exact multiple of 1024 fails rule 10", "[pkg]")
{
    static uint8_t p[16384];
    uint32_t len = build_pkg(p, 1000);
    pkg_check_out_t out;
    TEST_ASSERT_NOT_EQUAL(0, len % 1024u);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, run(p, len, &out), out.why);
    TEST_ASSERT_EQUAL_UINT32(3, out.lzo_blocks);   /* one compressed in type 3; one compressed, one stored in type 6 */

    /* Grow the type-4 block until the file is an exact multiple of 1024. */
    uint32_t want = (len + 1023u) / 1024u * 1024u;
    uint32_t k4 = 1000u + (want - len);
    len = build_pkg(p, k4);
    TEST_ASSERT_EQUAL_UINT32(0, len % 1024u);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(10, run(p, len, &out), out.why);
}

TEST_CASE("package facts: unreadable packages report unknowns", "[pkg]")
{
    uint8_t junk[300];
    memset(junk, 0x33, sizeof(junk));
    pkg_facts_t f;
    pkg_inspect_mem(junk, sizeof(junk), &f);
    TEST_ASSERT_EQUAL_STRING("", f.model);
    TEST_ASSERT_EQUAL_INT8(-1, f.layout);
    TEST_ASSERT_FALSE(f.chain_ok);
    pkg_inspect_mem(junk, 10, &f);
    TEST_ASSERT_EQUAL_UINT32(10, f.size);
    TEST_ASSERT_EQUAL_INT(-1, pkg_layout_for_build(1));
    TEST_ASSERT_EQUAL_INT(0, pkg_layout_for_build(13495));
    TEST_ASSERT_EQUAL_INT(1, pkg_layout_for_build(18975));
    TEST_ASSERT_EQUAL_INT(2, pkg_layout_for_build(23575));
}

#ifndef FL_TEST_UM980_DIR
#define FL_TEST_UM980_DIR ""
#endif

static uint8_t *load_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

TEST_CASE("package rules: the UM980 packages pass all twelve for a UM980, never for a UM982", "[pkg]")
{
    static const struct {
        uint32_t build;
        int8_t layout;          /* their own model's entry for their table */
        uint32_t layout_build;
    } pkgs[] = { { 10110, 3, 10110 }, { 11833, 3, 10110 }, { 23980, 5, 23980 }, { 25735, 5, 23980 } };
    for (size_t i = 0; i < sizeof(pkgs) / sizeof(pkgs[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/UM980_R4.10Build%u.pkg", FL_TEST_UM980_DIR, (unsigned)pkgs[i].build);
        uint32_t len = 0;
        uint8_t *p = load_file(path, &len);
        if (p == NULL) {
            TEST_IGNORE_MESSAGE("firmware/um980 package not found");
        }
        pkg_facts_t f;
        pkg_inspect_mem(p, len, &f);
        TEST_ASSERT_EQUAL_STRING("UM980", f.model);
        TEST_ASSERT_TRUE(f.chain_ok);
        TEST_ASSERT_EQUAL_INT8(pkgs[i].layout, f.layout);
        TEST_ASSERT_EQUAL_UINT32(pkgs[i].layout_build, f.layout_build);

        /* On a UM980 at its own build: every rule passes, the same layout. */
        pkg_check_in_t in = inputs(len);
        in.live_model = "UM980";
        in.receiver_build = pkgs[i].build;
        in.package_build = pkgs[i].build;
        pkg_check_out_t out;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, pkg_check(p, len, &in, &out), out.why);
        TEST_ASSERT_FALSE(out.cross_layout);
        TEST_ASSERT_TRUE(out.lzo_blocks > 0);

        /* Never on a UM982 (rule 8: the receiver's model). */
        in.live_model = "UM982";
        TEST_ASSERT_EQUAL_UINT8(8, pkg_check(p, len, &in, &out));
        TEST_ASSERT_EQUAL_STRING("package model UM980, receiver reports \"UM982\"", out.why);

        /* With the UM982's product tag: rule 9 names the UM980's. */
        in.live_model = "UM980";
        p[PKG_TAG_BYTE_OFF] = 0x40;
        reseal(p, 0);   /* the type-2 block (the first): its sum and header CRC */
        TEST_ASSERT_EQUAL_UINT8(9, pkg_check(p, len, &in, &out));
        TEST_ASSERT_EQUAL_STRING("boot configuration tag 40, not the UM980 tag", out.why);
        free(p);
    }
}

TEST_CASE("package rules: the NTRIP-X's UM980 at 11833: 11833 again, 23980 only as a confirmed cross-layout update",
          "[pkg]")
{
    char path[512];
    uint32_t len = 0;
    snprintf(path, sizeof(path), "%s/UM980_R4.10Build10110.pkg", FL_TEST_UM980_DIR);
    uint8_t *old = load_file(path, &len);
    if (old == NULL) {
        TEST_IGNORE_MESSAGE("firmware/um980 package not found");
    }
    pkg_check_in_t in = inputs(len);
    in.live_model = "UM980";
    in.receiver_build = 11833;
    in.package_build = 10110;
    pkg_check_out_t out;
    TEST_ASSERT_EQUAL_UINT8(12, pkg_check(old, len, &in, &out));   /* lower: never a downgrade */
    free(old);

    snprintf(path, sizeof(path), "%s/UM980_R4.10Build11833.pkg", FL_TEST_UM980_DIR);
    uint8_t *same = load_file(path, &len);
    TEST_ASSERT_NOT_NULL(same);
    in = inputs(len);
    in.live_model = "UM980";
    in.receiver_build = 11833;
    in.package_build = 11833;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, pkg_check(same, len, &in, &out), out.why);   /* the oldest that updates */
    TEST_ASSERT_FALSE(out.cross_layout);
    free(same);

    snprintf(path, sizeof(path), "%s/UM980_R4.10Build23980.pkg", FL_TEST_UM980_DIR);
    uint8_t *next = load_file(path, &len);
    TEST_ASSERT_NOT_NULL(next);
    in = inputs(len);
    in.live_model = "UM980";
    in.receiver_build = 11833;
    in.package_build = 23980;
    TEST_ASSERT_EQUAL_UINT8(12, pkg_check(next, len, &in, &out));
    TEST_ASSERT_TRUE(out.cross_layout);
    in.cross_layout_allowed = true;
    TEST_ASSERT_EQUAL_UINT8(12, pkg_check(next, len, &in, &out));   /* allowed, not yet confirmed */
    in.cross_layout_ack = true;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, pkg_check(next, len, &in, &out), out.why);
    free(next);
}

TEST_CASE("package models: the station flashes the UM982 and the UM980, builds never repeat across them", "[pkg]")
{
    TEST_ASSERT_NOT_NULL(pkg_model_find("UM982"));
    TEST_ASSERT_NOT_NULL(pkg_model_find("UM980"));
    TEST_ASSERT_NULL(pkg_model_find("UM981"));
    TEST_ASSERT_NULL(pkg_model_find("UM982C"));
    TEST_ASSERT_NULL(pkg_model_find(""));
    TEST_ASSERT_NULL(pkg_model_find(NULL));
    TEST_ASSERT_EQUAL_HEX8(0x41, pkg_model_find("UM980")->tag);
    TEST_ASSERT_EQUAL_UINT32(0, pkg_model_find("UM980")->tag_word);
    TEST_ASSERT_EQUAL_UINT32(660000000u, pkg_model_find("UM982")->tag_word);
    for (unsigned i = 0; i < PKG_KNOWN_LAYOUTS; i++) {
        TEST_ASSERT_NOT_NULL(pkg_model_find(pkg_known_layouts[i].model));
        for (unsigned j = 0; j < i; j++) {
            TEST_ASSERT_NOT_EQUAL(pkg_known_layouts[j].build, pkg_known_layouts[i].build);
        }
    }
    TEST_ASSERT_EQUAL_INT(4, pkg_layout_for_build(11833));
    TEST_ASSERT_EQUAL_INT(5, pkg_layout_for_build(23980));
}

