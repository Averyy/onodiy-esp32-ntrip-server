/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Header sector of the package slot (pure logic, no IDF includes,
 * host-testable).
 *
 * Sector 0 of the partition is a small log of fixed-size header records.
 * Every change of the slot's metadata (committed, pinned, unpinned,
 * released) appends one record into the next erased slot of the sector, so
 * no change needs an erase and a power cut can never lose the previous
 * state: a torn record fails its CRC and the one before it stays in force.
 * The last valid record in sector order is the current state. Only a new
 * package (pkg_store_begin) erases the sector, which is the moment the slot
 * becomes empty anyway; a sector that is full (PKG_HDR_SLOTS changes of one
 * package) is erased and restarted with the new record.
 *
 * Record (PKG_HDR_RECORD_SIZE bytes, little-endian):
 *   0   u32 magic PKG_HDR_MAGIC
 *   4   u8  layout version (1)
 *   5   u8  state (PKG_HDR_VALID or PKG_HDR_EMPTY)
 *   6   u8  pinned (0/1)
 *   7   u8  reserved (0)
 *   8   u32 sequence number (increases with every record)
 *   12  u32 payload size
 *   16  u8[32] SHA-256 of the payload
 *   48  char[32] version label, NUL-padded
 *   80  u8[44] reserved (0)
 *   124 u32 CRC-32 (IEEE 802.3) of bytes 0..123
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PKG_HDR_SECTOR_SIZE  4096u
#define PKG_HDR_RECORD_SIZE  128u
#define PKG_HDR_SLOTS        (PKG_HDR_SECTOR_SIZE / PKG_HDR_RECORD_SIZE)
#define PKG_HDR_MAGIC        0x31474B50u   /* "PKG1" */
#define PKG_HDR_LAYOUT       1u

typedef enum {
    PKG_HDR_EMPTY = 1,   /**< released or invalidated: nothing staged */
    PKG_HDR_VALID = 2,   /**< a complete, verified package */
} pkg_hdr_state_t;

typedef struct {
    uint8_t state;           /**< pkg_hdr_state_t */
    bool pinned;
    uint32_t seq;
    uint32_t size;
    uint8_t sha256[32];
    char version[32];
} pkg_hdr_t;

/** Result of reading the header sector. */
typedef struct {
    bool found;              /**< a valid record exists (hdr holds the last one) */
    pkg_hdr_t hdr;
    int next_slot;           /**< first erased slot after the last used one, -1 if the sector is full */
} pkg_hdr_scan_t;

/** Serialise a record (seq included). */
void pkg_hdr_encode(const pkg_hdr_t *h, uint8_t out[PKG_HDR_RECORD_SIZE]);

/** Parse one record; false if it is erased, torn or not ours. */
bool pkg_hdr_decode(const uint8_t rec[PKG_HDR_RECORD_SIZE], pkg_hdr_t *out);

/** True if all bytes of the record slot are 0xFF (never written since the erase). */
bool pkg_hdr_slot_erased(const uint8_t rec[PKG_HDR_RECORD_SIZE]);

/** Scan a whole header sector. */
void pkg_hdr_scan(const uint8_t sector[PKG_HDR_SECTOR_SIZE], pkg_hdr_scan_t *out);

/** CRC-32 (IEEE 802.3, reflected, init and final XOR 0xFFFFFFFF). */
uint32_t pkg_hdr_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
