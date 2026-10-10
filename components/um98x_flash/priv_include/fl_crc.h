/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Checksums of the receiver package and the XMODEM-1K transfer (pure logic,
 * written from the public definitions).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-16/XMODEM: poly 0x1021, init 0, no reflection, no final XOR ("123456789" -> 0x31C3). */
uint16_t fl_crc16_xmodem(const uint8_t *data, size_t len);

/** Adler-32 continued from `adler` (start with 1). */
uint32_t fl_adler32(uint32_t adler, const uint8_t *data, size_t len);

/** Sum of little-endian 32-bit words, mod 2^32 (len a multiple of 4; a tail is ignored). */
uint32_t fl_sum32_le(const uint8_t *data, size_t len);

/** 8-bit sum of the bytes, mod 256 (XMODEM checksum mode). */
uint8_t fl_sum8(const uint8_t *data, size_t len);

static inline uint32_t fl_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t fl_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t fl_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t fl_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

#ifdef __cplusplus
}
#endif
