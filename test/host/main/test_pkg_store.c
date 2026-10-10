/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * pkg_store on IDF's emulated flash (linux target, the "gnss_pkg" partition
 * of test/host/partitions.csv). Every test starts on a fresh flash image
 * (all 0xFF, deleted again at the end). pkg_store_init() again stands in for
 * a reboot; writing into the partition directly stands in for power cuts and
 * flash damage.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_private/partition_linux.h"
#include "pkg_header.h"
#include "pkg_store.h"
#include "psa/crypto.h"
#include "unity.h"

#define PKG_LEN 20000u

static uint8_t *s_pkg;

static const esp_partition_t *part(void)
{
    const esp_partition_t *p = esp_partition_find_first((esp_partition_type_t)0x40, (esp_partition_subtype_t)0x00,
                                                        "gnss_pkg");
    TEST_ASSERT_NOT_NULL(p);
    return p;
}

static void up(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    if (s_pkg == NULL) {
        s_pkg = malloc(PKG_LEN);
        TEST_ASSERT_NOT_NULL(s_pkg);
    }
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < PKG_LEN; i++) {
        x = x * 1103515245u + 12345u;
        s_pkg[i] = (uint8_t)(x >> 16);
    }
}

static void down(void)
{
    esp_partition_get_file_mmap_ctrl_input()->remove_dump = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_file_munmap());
}

static void sha(const uint8_t *data, size_t len, uint8_t out[32])
{
    size_t n = 0;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &n));
}

/* Write s_pkg[0..len) in uneven chunks. */
static esp_err_t write_all(pkg_store_writer_t w, size_t len)
{
    size_t off = 0;
    size_t chunk = 1;
    while (off < len) {
        size_t n = (len - off) < chunk ? (len - off) : chunk;
        esp_err_t err = pkg_store_write(w, &s_pkg[off], n);
        if (err != ESP_OK) {
            return err;
        }
        off += n;
        chunk = chunk * 3 + 7;
    }
    return ESP_OK;
}

static void stage(const char *label)
{
    uint8_t digest[32];
    sha(s_pkg, PKG_LEN, digest);
    pkg_store_writer_t w = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(PKG_LEN, digest, &w));
    TEST_ASSERT_EQUAL(ESP_OK, write_all(w, PKG_LEN));
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_finish(w, label, &info));
    TEST_ASSERT_TRUE(info.valid);
}

TEST_CASE("pkg_store: a fresh slot is empty", "[pkg_store]")
{
    up();
    TEST_ASSERT_EQUAL_size_t(part()->size - 4096u, pkg_store_capacity());
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    TEST_ASSERT_FALSE(info.valid);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_verify());
    uint8_t b;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_read(0, &b, 1));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_release());   /* nothing staged: fine */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_set_pinned(true));
    down();
}

TEST_CASE("pkg_store: a download is staged with its size and digest", "[pkg_store]")
{
    up();
    stage("R4.10Build18975");
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_TRUE(info.valid);
    TEST_ASSERT_FALSE(info.pinned);
    TEST_ASSERT_EQUAL_UINT32(PKG_LEN, info.size);
    TEST_ASSERT_EQUAL_STRING("R4.10Build18975", info.version);
    uint8_t digest[32];
    sha(s_pkg, PKG_LEN, digest);
    TEST_ASSERT_EQUAL_MEMORY(digest, info.sha256, 32);

    uint8_t *back = malloc(PKG_LEN);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_read(0, back, PKG_LEN));
    TEST_ASSERT_EQUAL_MEMORY(s_pkg, back, PKG_LEN);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_read(PKG_LEN - 10, back, 10));
    TEST_ASSERT_EQUAL_MEMORY(&s_pkg[PKG_LEN - 10], back, 10);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_read(PKG_LEN - 10, back, 11));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_read(PKG_LEN + 1, back, 0));
    free(back);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_verify());

    /* After a reboot it is still there. */
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_EQUAL_UINT32(PKG_LEN, info.size);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_verify());
    down();
}

TEST_CASE("pkg_store: wrong digest or size leaves the slot empty", "[pkg_store]")
{
    up();
    uint8_t digest[32];
    sha(s_pkg, PKG_LEN, digest);
    digest[5] ^= 1;
    pkg_store_writer_t w = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(PKG_LEN, digest, &w));
    TEST_ASSERT_EQUAL(ESP_OK, write_all(w, PKG_LEN));
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, pkg_store_finish(w, "x", &info));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_write(w, s_pkg, 1));   /* closed */

    /* Fewer bytes than announced. */
    sha(s_pkg, PKG_LEN, digest);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(PKG_LEN, digest, &w));
    TEST_ASSERT_EQUAL(ESP_OK, write_all(w, PKG_LEN - 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_finish(w, "x", &info));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));

    /* More bytes than announced. */
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(100, NULL, &w));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_write(w, s_pkg, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_write(w, s_pkg, 1));
    pkg_store_abort(w);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));

    /* Too large for the slot, or empty. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_begin((uint32_t)pkg_store_capacity() + 1, NULL, &w));
    TEST_ASSERT_NULL(w);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, pkg_store_begin(0, NULL, &w));
    down();
}

TEST_CASE("pkg_store: an upload without a digest is hashed for the user", "[pkg_store]")
{
    up();
    pkg_store_writer_t w = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(PKG_LEN, NULL, &w));
    /* One writer at a time. */
    pkg_store_writer_t w2 = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_begin(PKG_LEN, NULL, &w2));
    TEST_ASSERT_EQUAL(ESP_OK, write_all(w, PKG_LEN));
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_finish(w, "upload", &info));
    uint8_t digest[32];
    sha(s_pkg, PKG_LEN, digest);
    TEST_ASSERT_EQUAL_MEMORY(digest, info.sha256, 32);
    down();
}

TEST_CASE("pkg_store: a pinned package cannot be replaced or invalidated", "[pkg_store]")
{
    up();
    stage("pinned");
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_set_pinned(true));
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_TRUE(info.pinned);
    pkg_store_writer_t w = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_begin(10, NULL, &w));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_invalidate());

    /* The pin survives a reboot. */
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_TRUE(info.pinned);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_begin(10, NULL, &w));

    /* Unpin, then invalidate works. */
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_set_pinned(false));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_invalidate());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    down();
}

TEST_CASE("pkg_store: release unpins and empties the slot, also after a reboot", "[pkg_store]")
{
    up();
    stage("session");
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_set_pinned(true));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_release());
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    TEST_ASSERT_FALSE(info.pinned);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_release());

    /* The slot takes a new package (payload sectors are erased as it is written). */
    stage("next");
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_verify());
    down();
}

TEST_CASE("pkg_store: a power cut during a download leaves the slot empty", "[pkg_store]")
{
    up();
    stage("old");
    pkg_store_writer_t w = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_begin(PKG_LEN, NULL, &w));   /* the old one is gone now */
    TEST_ASSERT_EQUAL(ESP_OK, write_all(w, PKG_LEN / 2));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());                      /* power cut */
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, pkg_store_get_info(&info));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, pkg_store_write(w, s_pkg, 1));   /* the old writer is dead */
    stage("again");
    down();
}

TEST_CASE("pkg_store: a torn header record keeps the previous state", "[pkg_store]")
{
    up();
    stage("torn");
    /* Power cut while the pin record was being written: half a record. */
    uint8_t sector[PKG_HDR_SECTOR_SIZE];
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_read(part(), 0, sector, sizeof(sector)));
    pkg_hdr_scan_t scan;
    pkg_hdr_scan(sector, &scan);
    TEST_ASSERT_EQUAL_INT(1, scan.next_slot);
    uint8_t half[PKG_HDR_RECORD_SIZE / 2];
    memset(half, 0x5A, sizeof(half));
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_write(part(), PKG_HDR_RECORD_SIZE, half, sizeof(half)));

    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_FALSE(info.pinned);
    TEST_ASSERT_EQUAL_STRING("torn", info.version);
    /* The next record goes after the torn slot (the emulator refuses writes over programmed bits). */
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_set_pinned(true));
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_TRUE(info.pinned);
    down();
}

TEST_CASE("pkg_store: many header changes wrap the header sector", "[pkg_store]")
{
    up();
    stage("wrap");
    for (unsigned i = 0; i < 3u * PKG_HDR_SLOTS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, pkg_store_set_pinned((i % 2) == 0));
    }
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_init());
    pkg_store_info_t info;
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_get_info(&info));
    TEST_ASSERT_FALSE(info.pinned);   /* the last change unpinned it */
    TEST_ASSERT_EQUAL_STRING("wrap", info.version);
    TEST_ASSERT_EQUAL(ESP_OK, pkg_store_verify());
    down();
}

TEST_CASE("pkg_store: verify finds a payload changed in flash", "[pkg_store]")
{
    up();
    stage("verify");
    size_t off = 12345;
    while (s_pkg[off] == 0) {
        off++;
    }
    uint8_t zero = 0;   /* NOR flash: clearing bits needs no erase */
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_write(part(), 4096u + off, &zero, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, pkg_store_verify());
    down();
    free(s_pkg);
    s_pkg = NULL;
}
