/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Package slot header records: codec and the append-only log in sector 0.
 */
#include <string.h>

#include "pkg_header.h"
#include "unity.h"

static pkg_hdr_t hdr(uint8_t state, bool pinned, uint32_t seq, uint32_t size, const char *version)
{
    pkg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.state = state;
    h.pinned = pinned;
    h.seq = seq;
    h.size = size;
    for (int i = 0; i < 32; i++) {
        h.sha256[i] = (uint8_t)(i * 7 + 1);
    }
    strlcpy(h.version, version, sizeof(h.version));
    return h;
}

static void put(uint8_t *sector, unsigned slot, const pkg_hdr_t *h)
{
    pkg_hdr_encode(h, &sector[slot * PKG_HDR_RECORD_SIZE]);
}

TEST_CASE("package header CRC-32 is the IEEE one", "[pkg_header]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, pkg_hdr_crc32((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, pkg_hdr_crc32(NULL, 0));
}

TEST_CASE("package header record round-trips; damage is detected", "[pkg_header]")
{
    pkg_hdr_t h = hdr(PKG_HDR_VALID, true, 42, 3027628, "R4.10Build18975");
    uint8_t rec[PKG_HDR_RECORD_SIZE];
    pkg_hdr_encode(&h, rec);
    pkg_hdr_t d;
    TEST_ASSERT_TRUE(pkg_hdr_decode(rec, &d));
    TEST_ASSERT_EQUAL_UINT8(PKG_HDR_VALID, d.state);
    TEST_ASSERT_TRUE(d.pinned);
    TEST_ASSERT_EQUAL_UINT32(42, d.seq);
    TEST_ASSERT_EQUAL_UINT32(3027628, d.size);
    TEST_ASSERT_EQUAL_MEMORY(h.sha256, d.sha256, 32);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", d.version);

    /* Any single bit flip fails the CRC (or the magic). */
    for (size_t bit = 0; bit < PKG_HDR_RECORD_SIZE * 8; bit += 13) {
        uint8_t bad[PKG_HDR_RECORD_SIZE];
        memcpy(bad, rec, sizeof(bad));
        bad[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        TEST_ASSERT_FALSE(pkg_hdr_decode(bad, &d));
    }
    /* Erased flash is not a record. */
    uint8_t erased[PKG_HDR_RECORD_SIZE];
    memset(erased, 0xFF, sizeof(erased));
    TEST_ASSERT_FALSE(pkg_hdr_decode(erased, &d));
    TEST_ASSERT_TRUE(pkg_hdr_slot_erased(erased));
    TEST_ASSERT_FALSE(pkg_hdr_slot_erased(rec));

    /* A long label is cut to 31 characters, always terminated. */
    h = hdr(PKG_HDR_VALID, false, 1, 1, "0123456789012345678901234567890123456789");
    pkg_hdr_encode(&h, rec);
    TEST_ASSERT_TRUE(pkg_hdr_decode(rec, &d));
    TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890", d.version);
}

TEST_CASE("package header of an unknown layout or state is not trusted", "[pkg_header]")
{
    pkg_hdr_t h = hdr(PKG_HDR_VALID, false, 1, 1, "x");
    uint8_t rec[PKG_HDR_RECORD_SIZE];
    pkg_hdr_encode(&h, rec);
    pkg_hdr_t d;
    /* Rewrite fields and fix the CRC: still refused. */
    uint8_t bad[PKG_HDR_RECORD_SIZE];
    memcpy(bad, rec, sizeof(bad));
    bad[4] = 2;   /* layout */
    uint32_t crc = pkg_hdr_crc32(bad, 124);
    memcpy(&bad[124], &crc, 4);   /* host is little-endian like the record */
    TEST_ASSERT_FALSE(pkg_hdr_decode(bad, &d));
    memcpy(bad, rec, sizeof(bad));
    bad[5] = 7;   /* state */
    crc = pkg_hdr_crc32(bad, 124);
    memcpy(&bad[124], &crc, 4);
    TEST_ASSERT_FALSE(pkg_hdr_decode(bad, &d));
}

TEST_CASE("package header log: the last valid record wins", "[pkg_header]")
{
    static uint8_t sector[PKG_HDR_SECTOR_SIZE];
    memset(sector, 0xFF, sizeof(sector));
    pkg_hdr_scan_t scan;
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_FALSE(scan.found);
    TEST_ASSERT_EQUAL_INT(0, scan.next_slot);

    pkg_hdr_t a = hdr(PKG_HDR_VALID, false, 1, 100, "a");
    pkg_hdr_t b = hdr(PKG_HDR_VALID, true, 2, 100, "a");
    put(sector, 0, &a);
    put(sector, 1, &b);
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_TRUE(scan.hdr.pinned);
    TEST_ASSERT_EQUAL_UINT32(2, scan.hdr.seq);
    TEST_ASSERT_EQUAL_INT(2, scan.next_slot);

    /* A torn write (power cut) in slot 2: the pin stays, the next record goes to slot 3. */
    memset(&sector[2 * PKG_HDR_RECORD_SIZE], 0x00, 40);
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_TRUE(scan.hdr.pinned);
    TEST_ASSERT_EQUAL_INT(3, scan.next_slot);

    pkg_hdr_t c = hdr(PKG_HDR_EMPTY, false, 3, 0, "");
    put(sector, 3, &c);
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_EQUAL_UINT8(PKG_HDR_EMPTY, scan.hdr.state);
    TEST_ASSERT_FALSE(scan.hdr.pinned);
    TEST_ASSERT_EQUAL_INT(4, scan.next_slot);
}

TEST_CASE("package header log: a full sector reports no free slot", "[pkg_header]")
{
    static uint8_t sector[PKG_HDR_SECTOR_SIZE];
    memset(sector, 0xFF, sizeof(sector));
    for (unsigned i = 0; i < PKG_HDR_SLOTS; i++) {
        pkg_hdr_t h = hdr(PKG_HDR_VALID, (i % 2) != 0, i + 1, 64, "x");
        put(sector, i, &h);
    }
    pkg_hdr_scan_t scan;
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_TRUE(scan.found);
    TEST_ASSERT_EQUAL_UINT32(PKG_HDR_SLOTS, scan.hdr.seq);
    TEST_ASSERT_EQUAL_INT(-1, scan.next_slot);
    /* Garbage only (no erased slot, nothing valid). */
    memset(sector, 0x00, sizeof(sector));
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_FALSE(scan.found);
    TEST_ASSERT_EQUAL_INT(-1, scan.next_slot);
}
