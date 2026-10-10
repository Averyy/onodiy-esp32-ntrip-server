/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_crc.h"

/* CRC of each nibble value, reflected polynomial 0xEDB88320. */
static const uint32_t k_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

/* CRC of each nibble value in the top four bits, polynomial 0x1021. */
static const uint16_t k_crc16_nibble[16] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu,
};

uint32_t diag_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ k_crc32_nibble[crc & 0x0Fu];
        crc = (crc >> 4) ^ k_crc32_nibble[crc & 0x0Fu];
    }
    return crc;
}

uint32_t diag_crc32(const void *data, size_t len)
{
    return diag_crc32_final(diag_crc32_update(DIAG_CRC32_INIT, data, len));
}

uint16_t diag_crc16_update(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 4) ^ k_crc16_nibble[((crc >> 12) ^ (p[i] >> 4)) & 0x0Fu]);
        crc = (uint16_t)((crc << 4) ^ k_crc16_nibble[((crc >> 12) ^ (p[i] & 0x0Fu)) & 0x0Fu]);
    }
    return crc;
}
