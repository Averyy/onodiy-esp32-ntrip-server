/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "pkg_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fl_crc.h"
#include "lzo1x.h"

/* Block types in package order, with what their headers must say. */
typedef struct {
    uint8_t type;
    uint32_t load;
    uint32_t attr;
} blk_rule_t;

static const blk_rule_t k_order[PKG_BLOCKS] = {
    { 2, 0x00000000u, 0x00u },
    { 0, 0x00440000u, 0x03u },
    { 3, 0x00200000u, 0xE0u },
    { 4, 0x00000000u, 0x00u },
    { 6, 0x10000000u, 0xE0u },
};

const pkg_model_t pkg_models[PKG_MODELS] = {
    { "UM982", 0x40u, 660000000u },
    { "UM980", 0x41u, 0u },
};

const pkg_model_t *pkg_model_find(const char *name)
{
    for (unsigned i = 0; name != NULL && i < PKG_MODELS; i++) {
        if (strcmp(pkg_models[i].model, name) == 0) {
            return &pkg_models[i];
        }
    }
    return NULL;
}

#define LAYOUT_13495                                                                                                   \
    { 0x00000000u, 0x0001C000u, 0x0001A000u, 0x00030000u, 0x00680000u, 0x0001E000u, 0x00240000u, 0x0002F000u,      \
      0x00018000u },                                                                                                   \
    { 0x00020000u, 0x0001C000u, 0x0001B000u, 0x00138000u, 0x00680000u, 0x0001F000u, 0x00460000u, 0x0002F000u,      \
      0x00018000u }
#define LAYOUT_18975                                                                                                   \
    { 0x00000000u, 0x0001C000u, 0x0001A000u, 0x00030000u, 0x00680000u, 0x0001E000u, 0x00240000u, 0x0002F000u,      \
      0x00018000u },                                                                                                   \
    { 0x00020000u, 0x0001C000u, 0x0001B000u, 0x00138000u, 0x006C0000u, 0x0001F000u, 0x00460000u, 0x0002F000u,      \
      0x00018000u }

/* Known-good partition tables. The 18975 table is read from that package, and
 * the 23575 package carries the same one; the 13495 table differs only in the
 * type-4 backup address (PROVISIONAL for the UM982, derived; UM980 builds 10110
 * and 11833 carry exactly it in their own files, and 23980 and 25735 the 18975
 * one). Two builds may share a table: rule 12 compares the tables. */
const pkg_layout_t pkg_known_layouts[PKG_KNOWN_LAYOUTS] = {
    { "UM982", 13495u, LAYOUT_13495 },
    { "UM982", 18975u, LAYOUT_18975 },
    { "UM982", 23575u, LAYOUT_18975 },
    { "UM980", 10110u, LAYOUT_13495 },
    { "UM980", 11833u, LAYOUT_13495 },
    { "UM980", 23980u, LAYOUT_18975 },
    { "UM980", 25735u, LAYOUT_18975 },
};


/* Two known tables with the same addresses (builds that share a layout). */
static bool same_layout(int a, int b)
{
    return a >= 0 && b >= 0 &&
           memcmp(pkg_known_layouts[a].a, pkg_known_layouts[b].a, sizeof(pkg_known_layouts[a].a)) == 0 &&
           memcmp(pkg_known_layouts[a].b, pkg_known_layouts[b].b, sizeof(pkg_known_layouts[a].b)) == 0;
}

#define K0_W0      0xB0B10003u
#define K0_W1      0x00000130u
#define LZ_W0      0xB1B2F000u
#define TRAILER    16u

/* lzop container */
#define LZOP_VERSION      0x1040u
#define LZOP_NEEDED       0x0940u
#define LZOP_METHOD       3u
#define LZOP_LEVEL        9u
#define F_ADLER32_D       0x00000001u
#define F_ADLER32_C       0x00000002u
#define F_H_EXTRA_FIELD   0x00000040u
#define F_CRC32_D         0x00000100u
#define F_CRC32_C         0x00000200u
#define F_MULTIPART       0x00000400u
#define F_H_FILTER        0x00000800u
#define F_H_CRC32         0x00001000u

static const uint8_t k_lzop_magic[9] = { 0x89, 0x4C, 0x5A, 0x4F, 0x00, 0x0D, 0x0A, 0x1A, 0x0A };

static uint32_t block_magic(uint8_t type)
{
    return 0xA0A1A2A3u + (uint32_t)type * 0x01010101u;
}

int pkg_layout_for_build(uint32_t build)
{
    for (unsigned i = 0; i < PKG_KNOWN_LAYOUTS; i++) {
        if (pkg_known_layouts[i].build == build) {
            return (int)i;
        }
    }
    return -1;
}

/* Index of the known table equal to the 72 table bytes, -1 none; among equal ones, the first of `model` (NULL: any). */
static int layout_of_table(const uint8_t *t, const char *model)
{
    int any = -1;
    for (unsigned i = 0; i < PKG_KNOWN_LAYOUTS; i++) {
        bool same = true;
        for (unsigned k = 0; k < PKG_TYPES && same; k++) {
            same = fl_le32(t + 8u * k) == pkg_known_layouts[i].a[k] && fl_le32(t + 8u * k + 4u) == pkg_known_layouts[i].b[k];
        }
        if (same && (model == NULL || strcmp(pkg_known_layouts[i].model, model) == 0)) {
            return (int)i;
        }
        if (same && any < 0) {
            any = (int)i;
        }
    }
    return any;
}

/* ---- facts ------------------------------------------------------------------ */

void pkg_inspect(pkg_read_t read, void *ctx, uint32_t len, pkg_facts_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = len;
    out->layout = -1;
    uint8_t head[0xB0];
    if (len < sizeof(head) || !read(ctx, 0, head, sizeof(head))) {
        return;
    }
    if (fl_le32(head) == block_magic(2)) {
        const uint8_t *m = head + PKG_MODEL_OFF;
        size_t n = 0;
        while (n < PKG_MODEL_LEN && m[n] != 0) {
            n++;
        }
        if (n < PKG_MODEL_LEN) {
            bool text = true;
            for (size_t i = 0; i < n; i++) {
                text = text && m[i] >= 0x20 && m[i] <= 0x7E;
            }
            if (text) {
                memcpy(out->model, m, n);
                out->model[n] = '\0';
            }
        }
        int l = layout_of_table(head + PKG_TABLE_OFF, out->model[0] != '\0' ? out->model : NULL);
        out->layout = (int8_t)l;
        out->layout_build = l >= 0 ? pkg_known_layouts[l].build : 0;
    }
    /* Walk the chain for the date stamps. */
    uint32_t off = 0;
    unsigned i = 0;
    while (off < len && i < PKG_BLOCKS) {
        uint8_t h[PKG_HDR_LEN];
        if ((uint64_t)off + PKG_HDR_LEN > len || !read(ctx, off, h, sizeof(h))) {
            return;
        }
        if (fl_le32(h) != block_magic(k_order[i].type)) {
            return;
        }
        uint32_t size = fl_le32(h + 0x0C);
        out->stamps[i] = fl_le32(h + 0x10);
        uint64_t next = (uint64_t)off + PKG_HDR_LEN + size;
        if (next > len) {
            return;
        }
        off = (uint32_t)next;
        i++;
    }
    out->chain_ok = (i == PKG_BLOCKS && off == len);
}

static bool mem_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    memcpy(buf, (const uint8_t *)ctx + off, len);
    return true;
}

void pkg_inspect_mem(const uint8_t *pkg, uint32_t len, pkg_facts_t *out)
{
    pkg_inspect(mem_read, (void *)pkg, len, out);
}

/* ---- rules ------------------------------------------------------------------- */

typedef struct {
    uint32_t off[PKG_BLOCKS];    /* header offsets */
    uint32_t size[PKG_BLOCKS];   /* payload sizes */
} chain_t;

static uint8_t fail(pkg_check_out_t *out, uint8_t rule, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static uint8_t fail(pkg_check_out_t *out, uint8_t rule, const char *fmt, ...)
{
    out->failed_rule = rule;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->why, sizeof(out->why), fmt, ap);
    va_end(ap);
    return rule;
}

static uint8_t rule1(const uint8_t *p, uint32_t len, chain_t *c, pkg_check_out_t *out)
{
    uint32_t off = 0;
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        if ((uint64_t)off + PKG_HDR_LEN > len) {
            return fail(out, 1, "file ends inside block %u's header", i + 1u);
        }
        uint32_t magic = fl_le32(p + off);
        if (magic != block_magic(k_order[i].type)) {
            return fail(out, 1, "block %u is not of type %u (magic %08x)", i + 1u, (unsigned)k_order[i].type,
                        (unsigned)magic);
        }
        uint32_t size = fl_le32(p + off + 0x0C);
        if (size % 4u != 0) {
            return fail(out, 1, "block %u payload size %u is not a multiple of 4", i + 1u, (unsigned)size);
        }
        if (k_order[i].type == 2 && size != PKG_BOOTCFG_SIZE) {
            return fail(out, 1, "boot configuration block is %u bytes, not %u", (unsigned)size,
                        (unsigned)PKG_BOOTCFG_SIZE);
        }
        uint64_t next = (uint64_t)off + PKG_HDR_LEN + size;
        if (next > len) {
            return fail(out, 1, "block %u runs past the end of the file", i + 1u);
        }
        c->off[i] = off;
        c->size[i] = size;
        off = (uint32_t)next;
    }
    if (off != len) {
        return fail(out, 1, "%u bytes after the last block", (unsigned)(len - off));
    }
    return 0;
}

static uint8_t rule2(const uint8_t *p, const chain_t *c, pkg_check_out_t *out)
{
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        const uint8_t *h = p + c->off[i];
        if (fl_crc16_xmodem(h, 0x1C) != fl_le16(h + 0x1C)) {
            return fail(out, 2, "block %u header CRC wrong", i + 1u);
        }
        if (h[0x1E] != 0) {
            return fail(out, 2, "block %u header byte 0x1E is %02x", i + 1u, (unsigned)h[0x1E]);
        }
    }
    return 0;
}

static uint8_t rule3(const uint8_t *p, const chain_t *c, pkg_check_out_t *out)
{
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        const uint8_t *h = p + c->off[i];
        uint8_t k = k_order[i].type;
        uint32_t primary = fl_le32(p + PKG_TABLE_OFF + 8u * k);
        if (fl_le32(h + 0x04) != primary) {
            return fail(out, 3, "type %u flash address %08x, its table says %08x", (unsigned)k,
                        (unsigned)fl_le32(h + 0x04), (unsigned)primary);
        }
        if (fl_le32(h + 0x08) != k_order[i].load) {
            return fail(out, 3, "type %u load address %08x", (unsigned)k, (unsigned)fl_le32(h + 0x08));
        }
        if (fl_le32(h + 0x14) != k_order[i].attr) {
            return fail(out, 3, "type %u attributes %08x", (unsigned)k, (unsigned)fl_le32(h + 0x14));
        }
    }
    return 0;
}

static uint8_t rule4(const uint8_t *p, const chain_t *c, pkg_check_out_t *out)
{
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        const uint8_t *h = p + c->off[i];
        if (fl_sum32_le(h + PKG_HDR_LEN, c->size[i]) != fl_le32(h + 0x18)) {
            return fail(out, 4, "type %u payload sum wrong", (unsigned)k_order[i].type);
        }
    }
    return 0;
}

static uint8_t rule5(const uint8_t *p, const chain_t *c, pkg_check_out_t *out)
{
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        uint8_t k = k_order[i].type;
        if (k != 0 && k != 3 && k != 6) {
            continue;
        }
        uint32_t size = c->size[i];
        if (size < TRAILER) {
            return fail(out, 5, "type %u payload too short for its trailer", (unsigned)k);
        }
        const uint8_t *pl = p + c->off[i] + PKG_HDR_LEN;
        const uint8_t *t = pl + size - TRAILER;
        uint32_t w0 = fl_le32(t), w1 = fl_le32(t + 4), w2 = fl_le32(t + 8), w3 = fl_le32(t + 12);
        if (k == 0) {
            if (w0 != K0_W0 || w1 != K0_W1 || w2 != size) {
                return fail(out, 5, "type 0 trailer words wrong");
            }
        } else {
            uint32_t l = size - TRAILER;
            if (w0 != LZ_W0 || w1 != l || w2 != fl_sum32_le(pl, l)) {
                return fail(out, 5, "type %u trailer words wrong", (unsigned)k);
            }
        }
        if (w3 != (uint32_t)(w0 + w1 + w2)) {
            return fail(out, 5, "type %u trailer check word wrong", (unsigned)k);
        }
    }
    return 0;
}

/* One lzop container occupying exactly pl[0 : l]. */
static uint8_t lzop(const uint8_t *pl, uint32_t l, unsigned type, const pkg_check_in_t *in, pkg_check_out_t *out)
{
    const uint32_t fixed = 9u + 2u + 2u + 2u + 1u + 1u + 4u + 4u + 4u + 4u + 1u;
    if (l < fixed) {
        return fail(out, 6, "type %u: no room for the container header", type);
    }
    if (memcmp(pl, k_lzop_magic, sizeof(k_lzop_magic)) != 0) {
        return fail(out, 6, "type %u: container magic wrong", type);
    }
    uint32_t pos = 9;
    uint16_t version = fl_be16(pl + pos);
    uint16_t needed = fl_be16(pl + pos + 4);
    uint8_t method = pl[pos + 6];
    uint8_t level = pl[pos + 7];
    uint32_t flags = fl_be32(pl + pos + 8);
    pos += 2u + 2u + 2u + 1u + 1u + 4u + 4u + 4u + 4u;   /* versions, method, level, flags, mode, mtime (two words) */
    if (version != LZOP_VERSION || needed != LZOP_NEEDED || method != LZOP_METHOD || level != LZOP_LEVEL) {
        return fail(out, 6, "type %u: container version %04x/%04x method %u level %u", type, (unsigned)version,
                    (unsigned)needed, (unsigned)method, (unsigned)level);
    }
    uint32_t sums = flags & (F_ADLER32_D | F_ADLER32_C | F_CRC32_D | F_CRC32_C);
    if (sums != F_ADLER32_D || (flags & (F_H_FILTER | F_MULTIPART | F_H_EXTRA_FIELD | F_H_CRC32)) != 0) {
        return fail(out, 6, "type %u: container flags %08x", type, (unsigned)flags);
    }
    uint8_t name_len = pl[pos++];
    if ((uint64_t)pos + name_len + 4u > l) {
        return fail(out, 6, "type %u: container header runs past its end", type);
    }
    pos += name_len;
    if (fl_adler32(1, pl + 9, pos - 9u) != fl_be32(pl + pos)) {
        return fail(out, 6, "type %u: container header checksum wrong", type);
    }
    pos += 4;
    for (;;) {
        /* Block headers start 4-byte aligned to the payload; the gap is zero. */
        while (pos % 4u != 0) {
            if (pos >= l || pl[pos] != 0) {
                return fail(out, 6, "type %u: alignment padding wrong at %u", type, (unsigned)pos);
            }
            pos++;
        }
        if ((uint64_t)pos + 4u > l) {
            return fail(out, 6, "type %u: no terminator", type);
        }
        uint32_t dlen = fl_be32(pl + pos);
        pos += 4;
        if (dlen == 0) {
            if (pos != l) {
                return fail(out, 6, "type %u: terminator ends %u bytes before the trailer", type, (unsigned)(l - pos));
            }
            return 0;
        }
        if ((uint64_t)pos + 8u > l) {
            return fail(out, 6, "type %u: block header runs past the end", type);
        }
        uint32_t slen = fl_be32(pl + pos);
        uint32_t adler = fl_be32(pl + pos + 4);
        pos += 8;
        if (slen == 0 || slen > dlen || (uint64_t)pos + slen > l) {
            return fail(out, 6, "type %u: block %u sizes %u/%u wrong", type, (unsigned)(out->lzo_blocks + 1u),
                        (unsigned)slen, (unsigned)dlen);
        }
        if (dlen > in->work_cap || in->work == NULL) {
            return fail(out, 6, "type %u: block of %u bytes larger than the check buffer", type, (unsigned)dlen);
        }
        const uint8_t *data;
        if (slen == dlen) {
            data = pl + pos;   /* stored */
        } else {
            size_t got = 0;
            lzo1x_result_t r = lzo1x_decompress(pl + pos, slen, in->work, dlen, &got);
            if (r != LZO1X_OK || got != dlen) {
                return fail(out, 6, "type %u: block %u does not decompress (%d, %u of %u bytes)", type,
                            (unsigned)(out->lzo_blocks + 1u), (int)r, (unsigned)got, (unsigned)dlen);
            }
            data = in->work;
        }
        if (fl_adler32(1, data, dlen) != adler) {
            return fail(out, 6, "type %u: block %u checksum wrong", type, (unsigned)(out->lzo_blocks + 1u));
        }
        pos += slen;
        out->lzo_blocks++;
        if (in->tick != NULL) {
            in->tick(in->tick_ctx);
        }
    }
}

static uint8_t rule6(const uint8_t *p, const chain_t *c, const pkg_check_in_t *in, pkg_check_out_t *out)
{
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        uint8_t k = k_order[i].type;
        if (k != 3 && k != 6) {
            continue;
        }
        uint8_t r = lzop(p + c->off[i] + PKG_HDR_LEN, c->size[i] - TRAILER, k, in, out);
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

static uint8_t rule7(const uint8_t *p, pkg_check_out_t *out)
{
    if (fl_le32(p + PKG_BOOT_BAUD_OFF) != PKG_BOOT_BAUD) {
        return fail(out, 7, "loader baud %u, not %u", (unsigned)fl_le32(p + PKG_BOOT_BAUD_OFF), (unsigned)PKG_BOOT_BAUD);
    }
    if (layout_of_table(p + PKG_TABLE_OFF, NULL) < 0) {
        return fail(out, 7, "partition table is not one of the known ones");
    }
    return 0;
}

static uint8_t rule8(const uint8_t *p, const pkg_check_in_t *in, pkg_check_out_t *out)
{
    const uint8_t *m = p + PKG_MODEL_OFF;
    const uint8_t *nul = memchr(m, 0, PKG_MODEL_LEN);
    if (nul == NULL) {
        return fail(out, 8, "model field has no terminator");
    }
    char model[PKG_MODEL_LEN];
    memcpy(model, m, (size_t)(nul - m) + 1u);
    if (pkg_model_find(model) == NULL) {
        return fail(out, 8, "package model \"%.15s\" is not one the station flashes", model);
    }
    if (in->live_model == NULL || strcmp(in->live_model, model) != 0) {
        return fail(out, 8, "package model %s, receiver reports \"%s\"", model,
                    in->live_model != NULL ? in->live_model : "(nothing)");
    }
    return 0;
}

static uint8_t rule9(const uint8_t *p, const chain_t *c, pkg_check_out_t *out)
{
    char model[PKG_MODEL_LEN];
    memcpy(model, p + PKG_MODEL_OFF, PKG_MODEL_LEN);
    model[PKG_MODEL_LEN - 1] = '\0';   /* rule 8 found the terminator */
    const pkg_model_t *mk = pkg_model_find(model);
    if (mk == NULL) {
        return fail(out, 9, "no product tags known for model \"%.15s\"", model);
    }
    for (unsigned i = 0; i < PKG_BLOCKS; i++) {
        uint8_t k = k_order[i].type;
        if (k == 0) {
            continue;
        }
        if (p[c->off[i] + 0x1F] != mk->tag) {
            return fail(out, 9, "type %u header tag %02x, not the %s tag", (unsigned)k, (unsigned)p[c->off[i] + 0x1F],
                        mk->model);
        }
    }
    if (p[PKG_TAG_BYTE_OFF] != mk->tag) {
        return fail(out, 9, "boot configuration tag %02x, not the %s tag", (unsigned)p[PKG_TAG_BYTE_OFF], mk->model);
    }
    if (fl_le32(p + PKG_TAG_WORD_OFF) != mk->tag_word) {
        return fail(out, 9, "boot configuration word %u, not the %s value", (unsigned)fl_le32(p + PKG_TAG_WORD_OFF),
                    mk->model);
    }
    return 0;
}

uint8_t pkg_check(const uint8_t *pkg, uint32_t len, const pkg_check_in_t *in, pkg_check_out_t *out)
{
    memset(out, 0, sizeof(*out));
    pkg_inspect_mem(pkg, len, &out->facts);
    chain_t c;
    memset(&c, 0, sizeof(c));
    uint8_t r = rule1(pkg, len, &c, out);
    if (r == 0) {
        r = rule2(pkg, &c, out);
    }
    if (r == 0) {
        r = rule3(pkg, &c, out);
    }
    if (r == 0) {
        r = rule4(pkg, &c, out);
    }
    if (r == 0) {
        r = rule5(pkg, &c, out);
    }
    if (r == 0) {
        r = rule6(pkg, &c, in, out);
    }
    if (r == 0) {
        r = rule7(pkg, out);
    }
    if (r == 0) {
        r = rule8(pkg, in, out);
    }
    if (r == 0) {
        r = rule9(pkg, &c, out);
    }
    if (r == 0 && len % 1024u == 0) {
        r = fail(out, 10, "length %u is a multiple of 1024", (unsigned)len);
    }
    if (r == 0) {
        if (in->expected_sha256 == NULL || in->actual_sha256 == NULL) {
            r = fail(out, 11, "no SHA-256 to compare");
        } else if (len != in->expected_size) {
            r = fail(out, 11, "size %u, expected %u", (unsigned)len, (unsigned)in->expected_size);
        } else if (memcmp(in->actual_sha256, in->expected_sha256, 32) != 0) {
            r = fail(out, 11, "SHA-256 differs from the confirmed one");
        }
    }
    /* Rule 12 (and the cross-layout fact, also shown in recovery). */
    int cur = pkg_layout_for_build(in->receiver_build);
    out->cross_layout = !same_layout(out->facts.layout, cur);
    if (r == 0 && !in->recovery) {
        if (in->package_build == 0) {
            r = fail(out, 12, "package build unknown");
        } else if (in->receiver_build == 0) {
            r = fail(out, 12, "receiver build unknown: a downgrade cannot be ruled out");
        } else if (in->package_build < in->receiver_build) {
            r = fail(out, 12, "build %u is lower than the receiver's %u", (unsigned)in->package_build,
                     (unsigned)in->receiver_build);
        } else if (out->cross_layout && !in->cross_layout_allowed) {
            r = fail(out, 12, "partition layout differs from the receiver's (not tested on hardware yet)");
        } else if (out->cross_layout && !in->cross_layout_ack) {
            r = fail(out, 12, "partition layout differs from the receiver's: needs its own confirmation");
        }
    }
    return r;
}
