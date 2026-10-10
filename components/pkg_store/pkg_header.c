/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "pkg_header.h"

#include <string.h>

#define OFF_MAGIC   0
#define OFF_LAYOUT  4
#define OFF_STATE   5
#define OFF_PINNED  6
#define OFF_SEQ     8
#define OFF_SIZE    12
#define OFF_SHA     16
#define OFF_VERSION 48
#define OFF_CRC     124

_Static_assert(PKG_HDR_SECTOR_SIZE % PKG_HDR_RECORD_SIZE == 0, "records must tile the header sector");
_Static_assert(OFF_VERSION + 32 <= OFF_CRC, "record fields overlap the CRC");

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t pkg_hdr_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void pkg_hdr_encode(const pkg_hdr_t *h, uint8_t out[PKG_HDR_RECORD_SIZE])
{
    memset(out, 0, PKG_HDR_RECORD_SIZE);
    put32(&out[OFF_MAGIC], PKG_HDR_MAGIC);
    out[OFF_LAYOUT] = PKG_HDR_LAYOUT;
    out[OFF_STATE] = h->state;
    out[OFF_PINNED] = h->pinned ? 1 : 0;
    put32(&out[OFF_SEQ], h->seq);
    put32(&out[OFF_SIZE], h->size);
    memcpy(&out[OFF_SHA], h->sha256, 32);
    size_t n = strnlen(h->version, 31);
    memcpy(&out[OFF_VERSION], h->version, n);
    put32(&out[OFF_CRC], pkg_hdr_crc32(out, OFF_CRC));
}

bool pkg_hdr_decode(const uint8_t rec[PKG_HDR_RECORD_SIZE], pkg_hdr_t *out)
{
    if (get32(&rec[OFF_MAGIC]) != PKG_HDR_MAGIC || get32(&rec[OFF_CRC]) != pkg_hdr_crc32(rec, OFF_CRC)) {
        return false;
    }
    /* A newer layout may mean anything: not ours to interpret. */
    if (rec[OFF_LAYOUT] != PKG_HDR_LAYOUT || (rec[OFF_STATE] != PKG_HDR_EMPTY && rec[OFF_STATE] != PKG_HDR_VALID) ||
        rec[OFF_PINNED] > 1) {
        return false;
    }
    pkg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.state = rec[OFF_STATE];
    h.pinned = rec[OFF_PINNED] != 0;
    h.seq = get32(&rec[OFF_SEQ]);
    h.size = get32(&rec[OFF_SIZE]);
    memcpy(h.sha256, &rec[OFF_SHA], 32);
    memcpy(h.version, &rec[OFF_VERSION], 31);
    h.version[31] = '\0';
    *out = h;
    return true;
}

bool pkg_hdr_slot_erased(const uint8_t rec[PKG_HDR_RECORD_SIZE])
{
    for (size_t i = 0; i < PKG_HDR_RECORD_SIZE; i++) {
        if (rec[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

void pkg_hdr_scan(const uint8_t sector[PKG_HDR_SECTOR_SIZE], pkg_hdr_scan_t *out)
{
    memset(out, 0, sizeof(*out));
    int last_used = -1;
    for (unsigned i = 0; i < PKG_HDR_SLOTS; i++) {
        const uint8_t *rec = &sector[i * PKG_HDR_RECORD_SIZE];
        if (pkg_hdr_slot_erased(rec)) {
            continue;
        }
        last_used = (int)i;
        pkg_hdr_t h;
        if (pkg_hdr_decode(rec, &h)) {
            out->found = true;
            out->hdr = h;
        }
    }
    /* Records are appended in order; anything written (torn or not) after
     * the erased slots is never overwritten, so the next record goes after
     * the last used slot. */
    out->next_slot = (last_used + 1 < (int)PKG_HDR_SLOTS) ? last_used + 1 : -1;
}
