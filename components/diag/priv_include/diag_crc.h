/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Checksums for diag's stored formats (pure logic, no IDF includes,
 * host-testable).
 *
 *  - CRC-32/ISO-HDLC (the common zlib/Ethernet CRC): reflected polynomial
 *    0xEDB88320, initial value and final XOR 0xFFFFFFFF. Check value for the
 *    nine bytes "123456789": 0xCBF43926. Used for the log ring header and
 *    the journal's sector headers and records.
 *  - CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no
 *    reflection, no final XOR. Check value 0x29B1. Used for each log ring
 *    record (two bytes per line instead of four).
 *
 * Both work a nibble at a time from a 16-entry table: small, and fast
 * enough for a log line or a 32-byte record.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_CRC32_INIT 0xFFFFFFFFu
#define DIAG_CRC16_INIT 0xFFFFu

/** Feed bytes into a running CRC-32 (start with DIAG_CRC32_INIT). */
uint32_t diag_crc32_update(uint32_t crc, const void *data, size_t len);

/** Finish a running CRC-32. */
static inline uint32_t diag_crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

/** CRC-32 of one buffer. */
uint32_t diag_crc32(const void *data, size_t len);

/** Feed bytes into a running CRC-16 (start with DIAG_CRC16_INIT; no final step). */
uint16_t diag_crc16_update(uint16_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
