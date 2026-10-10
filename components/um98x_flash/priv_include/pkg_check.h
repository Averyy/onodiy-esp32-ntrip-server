/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver package pre-flight rules (pure logic, host-tested with the real
 * package and mutated copies of it). They run on the RAM copy that will be
 * sent, right before the receiver's loader is entered. Every rule is
 * mandatory; the first one that fails refuses the session.
 *
 * Package layout (all little-endian): a chain of blocks that tiles the file
 * exactly; each block is a 32-byte header and `size` payload bytes.
 * Header: +0x00 magic (type k: 0xA0A1A2A3 + k * 0x01010101), +0x04 flash
 * address, +0x08 load address, +0x0C payload size, +0x10 date stamp, +0x14
 * attributes, +0x18 sum of the payload's 32-bit words, +0x1C CRC-16/XMODEM
 * of header bytes 0x00..0x1B, +0x1E zero, +0x1F product tag.
 *
 *  1 structure: types exactly 2, 0, 3, 4, 6 in that order, sizes chaining
 *    to the end of the file, every payload a multiple of 4, the type-2
 *    payload 0x9C bytes
 *  2 header CRC and the zero byte at +0x1E
 *  3 flash address = the primary address for the type in the package's own
 *    partition table; load address and attributes as expected per type
 *  4 payload word sums
 *  5 trailers of the type-0, type-3 and type-6 payloads (four words)
 *  6 the compressed type-3 and type-6 payloads: an lzop container (header
 *    fields, header Adler-32, big-endian block fields, block headers 4-byte
 *    aligned to the payload start, zero terminator ending exactly at the
 *    trailer), every block decompressed with LZO1X to its stated length and
 *    Adler-32
 *  7 the loader baud in the type-2 block is 460800 and the partition table
 *    is one of the known-good tables compiled in
 *  8 the model string (NUL within 16 bytes) is exactly "UM982" and equals
 *    the receiver's model from this session's version query (the journalled
 *    model in recovery)
 *  9 UM982 product tags: +0x1F = 0x40 on the type-2, 3, 4, 6 headers, file
 *    byte 0x7F = 0x40, file word 0xAC = 660000000
 * 10 the length is not a multiple of 1024
 * 11 size and SHA-256 of the RAM copy equal the release manifest entry or
 *    what the user confirmed for an upload
 * 12 the package build is not lower than the receiver's; a package whose
 *    partition table differs from the receiver's current build's (unknown
 *    current build counts as different) is a cross-layout update: refused
 *    unless the build allows it (bench-tested) and the user confirmed it.
 *    Not re-run in recovery.
 *
 * PROVISIONAL, to be confirmed on hardware: the format knowledge comes from
 * the package files themselves, and the table of the older build is
 * derived from the one difference recorded for it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PKG_HDR_LEN          32u
#define PKG_BLOCKS           5u
#define PKG_TYPES            9u
#define PKG_TABLE_OFF        0x30u
#define PKG_TABLE_LEN        (PKG_TYPES * 8u)
#define PKG_MODEL_OFF        0x9Cu
#define PKG_MODEL_LEN        16u
#define PKG_BOOT_BAUD_OFF    0x20u
#define PKG_BOOT_BAUD        460800u
#define PKG_TAG_BYTE_OFF     0x7Fu
#define PKG_TAG_WORD_OFF     0xACu
#define PKG_TAG_WORD         660000000u
#define PKG_UM982_TAG        0x40u
#define PKG_BOOTCFG_SIZE     0x9Cu
#define PKG_MODEL            "UM982"
#define PKG_LZO_BLOCK_MAX    (256u * 1024u)   /**< largest decompressed block accepted */
#define PKG_KNOWN_LAYOUTS    2u

/** A known-good partition table: file bytes 0x30..0x77 of the package. */
typedef struct {
    uint32_t build;                      /**< receiver build that uses it */
    uint32_t a[PKG_TYPES];               /**< primary flash address per type */
    uint32_t b[PKG_TYPES];               /**< backup flash address per type */
} pkg_layout_t;

extern const pkg_layout_t pkg_known_layouts[PKG_KNOWN_LAYOUTS];

/** Index of the known table a receiver build uses, -1 when the build is unknown. */
int pkg_layout_for_build(uint32_t build);

/** Facts readable without the receiver (upload page, logs). */
typedef struct {
    uint32_t size;
    bool chain_ok;                       /**< the block chain could be walked (stamps are meaningful) */
    char model[PKG_MODEL_LEN];           /**< "" if unreadable */
    int8_t layout;                       /**< index of the known table it carries, -1 unknown */
    uint32_t layout_build;               /**< 0 unknown */
    uint32_t stamps[PKG_BLOCKS];         /**< date stamps of the five blocks, 0 if unreadable */
} pkg_facts_t;

/** Reader for inspecting a package that is not in RAM (offset, length; false on a read error). */
typedef bool (*pkg_read_t)(void *ctx, uint32_t off, void *buf, size_t len);

/** Facts through a reader (headers only: a few reads). */
void pkg_inspect(pkg_read_t read, void *ctx, uint32_t len, pkg_facts_t *out);

/** Facts of a package in RAM. */
void pkg_inspect_mem(const uint8_t *pkg, uint32_t len, pkg_facts_t *out);

typedef struct {
    const char *live_model;              /**< rule 8: this session's (or the journalled) receiver model */
    uint32_t receiver_build;             /**< rule 12: the receiver's current build (0 = unknown) */
    uint32_t package_build;              /**< rule 12: from the manifest or the user */
    uint32_t expected_size;              /**< rule 11 */
    const uint8_t *expected_sha256;      /**< rule 11 (32 bytes) */
    const uint8_t *actual_sha256;        /**< rule 11: computed over the RAM copy by the caller */
    bool cross_layout_ack;               /**< rule 12: the user's extra confirmation */
    bool cross_layout_allowed;           /**< rule 12: this build may flash a cross-layout package at all */
    bool recovery;                       /**< recovery gate: rule 12 is not run */
    uint8_t *work;                       /**< rule 6: decompression buffer */
    size_t work_cap;                     /**<   at least the largest block (PKG_LZO_BLOCK_MAX covers every known one) */
    void (*tick)(void *ctx);             /**< called between compressed blocks (watchdog) */
    void *tick_ctx;
} pkg_check_in_t;

typedef struct {
    uint8_t failed_rule;                 /**< 0 = every rule passed */
    char why[80];                        /**< what failed */
    bool cross_layout;                   /**< the package's table differs from the receiver's current build's */
    uint32_t lzo_blocks;                 /**< compressed blocks checked */
    pkg_facts_t facts;
} pkg_check_out_t;

/** Run rules 1-12 on the RAM copy. @return out->failed_rule. */
uint8_t pkg_check(const uint8_t *pkg, uint32_t len, const pkg_check_in_t *in, pkg_check_out_t *out);

#ifdef __cplusplus
}
#endif
