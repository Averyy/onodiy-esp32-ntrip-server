/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "pkg_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pkg_header.h"
#include "pkg_store_ram.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif

static const char *TAG = "pkg_store";

/* Custom partition type from partitions.csv (0x40-0xFE is application-defined). */
#define PKG_STORE_PARTITION_TYPE    ((esp_partition_type_t)0x40)
#define PKG_STORE_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x00)
#define PKG_STORE_PARTITION_LABEL   "gnss_pkg"
#define PKG_STORE_HEADER_SIZE       PKG_HDR_SECTOR_SIZE
#define PKG_STORE_ERASE_SIZE        4096u
#define VERIFY_CHUNK                4096u
#if CONFIG_PKG_STORE_RAM
#define RAM_MAX_BYTES               ((size_t)CONFIG_PKG_STORE_RAM_MAX_KB * 1024u)
#define RAM_MARGIN_BYTES            ((size_t)CONFIG_PKG_STORE_RAM_MARGIN_KB * 1024u)
#else
#define RAM_MAX_BYTES               ((size_t)0)   /* never in PSRAM mode */
#define RAM_MARGIN_BYTES            ((size_t)0)
#endif

/* Hashing uses PSA Crypto (Mbed TLS 4 / TF-PSA-Crypto in IDF v6.x); the
 * legacy mbedtls_sha256_* API is private there. */
_Static_assert(PSA_HASH_LENGTH(PSA_ALG_SHA_256) == sizeof(((pkg_store_info_t *)0)->sha256),
               "pkg_store digest size must match SHA-256");
_Static_assert(PKG_STORE_HEADER_SIZE % PKG_STORE_ERASE_SIZE == 0, "payload must start on an erase boundary");

struct pkg_store_writer {
    bool open;
    uint32_t size;               /* expected payload bytes */
    uint32_t written;
    uint32_t erased;             /* payload bytes erased so far (multiple of the erase size) */
    bool check_digest;
    uint8_t expected[32];
    psa_hash_operation_t hash;
};

static const esp_partition_t *s_part;
static bool s_absent;   /* pkg_store_init found no package area: no partition and no PSRAM staging */

/* PSRAM staging (a board without the partition): the payload in one buffer, the header in s_hdr only. */
static bool s_ram_mode;
static uint8_t *s_ram;           /* the payload being written or staged, NULL = none */
static const uint8_t *s_borrowed;/* the buffer the flasher holds (pkg_store_borrow), NULL = none */
static uint8_t *s_orphan;        /* released while borrowed: freed when it comes back */
static SemaphoreHandle_t s_mutex;
static pkg_hdr_t s_hdr;          /* current state (valid when s_have_hdr) */
static bool s_have_hdr;
static int s_next_slot;          /* next free header record slot, -1 = sector full */
static struct pkg_store_writer s_writer;

static void lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static bool staged_valid(void)
{
    return s_have_hdr && s_hdr.state == PKG_HDR_VALID;
}

static void fill_info(pkg_store_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_have_hdr) {
        return;
    }
    out->valid = s_hdr.state == PKG_HDR_VALID;
    out->pinned = s_hdr.pinned;
    out->size = out->valid ? s_hdr.size : 0;
    memcpy(out->sha256, s_hdr.sha256, sizeof(out->sha256));
    strlcpy(out->version, s_hdr.version, sizeof(out->version));
}

/* Read the header sector and adopt its last valid record. Call locked. */
static esp_err_t load_header(void)
{
    uint8_t *sector = malloc(PKG_HDR_SECTOR_SIZE);
    if (sector == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_partition_read(s_part, 0, sector, PKG_HDR_SECTOR_SIZE);
    if (err == ESP_OK) {
        pkg_hdr_scan_t scan;
        pkg_hdr_scan(sector, &scan);
        s_have_hdr = scan.found;
        s_hdr = scan.hdr;
        s_next_slot = scan.next_slot;
        if (s_have_hdr && s_hdr.state == PKG_HDR_VALID && (s_hdr.size == 0 || s_hdr.size > pkg_store_capacity())) {
            ESP_LOGW(TAG, "header names %u bytes, more than the slot holds: treated as empty", (unsigned)s_hdr.size);
            s_hdr.state = PKG_HDR_EMPTY;
            s_hdr.pinned = false;
        }
    }
    free(sector);
    return err;
}

/* Erase the header sector: the slot is empty from here on. Call locked. */
static esp_err_t erase_header(void)
{
    if (s_ram_mode) {
        s_have_hdr = false;
        memset(&s_hdr, 0, sizeof(s_hdr));
        return ESP_OK;
    }
    esp_err_t err = esp_partition_erase_range(s_part, 0, PKG_HDR_SECTOR_SIZE);
    if (err == ESP_OK) {
        s_have_hdr = false;
        memset(&s_hdr, 0, sizeof(s_hdr));
        s_next_slot = 0;
    }
    return err;
}

/*
 * Append a header record (the new state). A full sector is erased and
 * restarted. A record that does not read back intact (failed write) is
 * skipped and the next slot is tried. Call locked.
 */
static esp_err_t append_header(const pkg_hdr_t *h)
{
    pkg_hdr_t rec = *h;
    rec.seq = s_have_hdr ? s_hdr.seq + 1u : 1u;
    if (s_ram_mode) {
        s_hdr = rec;   /* kept until the next start only */
        s_have_hdr = true;
        return ESP_OK;
    }
    uint8_t buf[PKG_HDR_RECORD_SIZE];
    uint8_t back[PKG_HDR_RECORD_SIZE];
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (s_next_slot < 0) {
            err = esp_partition_erase_range(s_part, 0, PKG_HDR_SECTOR_SIZE);
            if (err != ESP_OK) {
                return err;
            }
            s_next_slot = 0;
        }
        size_t off = (size_t)s_next_slot * PKG_HDR_RECORD_SIZE;
        s_next_slot = (s_next_slot + 1 < (int)PKG_HDR_SLOTS) ? s_next_slot + 1 : -1;
        pkg_hdr_encode(&rec, buf);
        err = esp_partition_write(s_part, off, buf, sizeof(buf));
        if (err == ESP_OK) {
            err = esp_partition_read(s_part, off, back, sizeof(back));
        }
        pkg_hdr_t check;
        if (err == ESP_OK && pkg_hdr_decode(back, &check) && check.seq == rec.seq) {
            s_hdr = check;
            s_have_hdr = true;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "header record at %u did not stick (%s), trying the next slot", (unsigned)off,
                 esp_err_to_name(err));
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    return err;
}

/* Let go of the PSRAM payload (the slot is empty): freed now, or when the flasher returns it. Call locked. */
static void ram_drop(void)
{
    if (s_ram == NULL) {
        return;
    }
    if (s_ram == s_borrowed) {
        s_orphan = s_ram;
    } else {
        heap_caps_free(s_ram);
    }
    s_ram = NULL;
}

static esp_err_t append_state(uint8_t state, bool pinned)
{
    if (state == PKG_HDR_EMPTY) {
        ram_drop();
    }
    pkg_hdr_t h = s_hdr;
    h.state = state;
    h.pinned = pinned;
    if (state == PKG_HDR_EMPTY) {
        h.size = 0;
        memset(h.sha256, 0, sizeof(h.sha256));
        memset(h.version, 0, sizeof(h.version));
    }
    return append_header(&h);
}

static void writer_close(void)
{
    if (s_writer.open) {
        psa_hash_abort(&s_writer.hash);
    }
    memset(&s_writer, 0, sizeof(s_writer));
}

static esp_err_t init_impl(bool no_partition)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    lock();
    writer_close();   /* a write cut short by a reboot left the slot empty */
    ram_drop();
    s_have_hdr = false;
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_next_slot = 0;
    s_ram_mode = false;
    s_absent = false;
    s_part = no_partition ? NULL
                          : esp_partition_find_first(PKG_STORE_PARTITION_TYPE, PKG_STORE_PARTITION_SUBTYPE,
                                                     PKG_STORE_PARTITION_LABEL);
    if (s_part == NULL) {
        /* A partition table without one (a board with 8 MB flash): the package is staged in PSRAM. */
#if CONFIG_PKG_STORE_RAM
        s_ram_mode = true;
        unlock();
        ESP_LOGI(TAG, "no partition '%s' on this board: receiver packages are staged in PSRAM (up to %u KB, "
                      "not kept over a restart)",
                 PKG_STORE_PARTITION_LABEL, (unsigned)(pkg_store_capacity() / 1024));
#else
        s_absent = true;
        unlock();
        ESP_LOGI(TAG, "no partition '%s' on this board: receiver packages cannot be staged", PKG_STORE_PARTITION_LABEL);
#endif
        return ESP_OK;
    }
    if (s_part->size <= PKG_STORE_HEADER_SIZE) {
        s_part = NULL;
        unlock();
        ESP_LOGE(TAG, "partition '%s' too small", PKG_STORE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = load_header();
    pkg_store_info_t info;
    fill_info(&info);
    unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "header unreadable: %s", esp_err_to_name(err));
        return err;
    }
    if (info.valid) {
        ESP_LOGI(TAG, "staged: %s, %u bytes%s", info.version, (unsigned)info.size, info.pinned ? ", pinned" : "");
    } else {
        ESP_LOGI(TAG, "partition '%s' at 0x%06x: empty, %u KB capacity", s_part->label, (unsigned)s_part->address,
                 (unsigned)(pkg_store_capacity() / 1024));
    }
    return ESP_OK;
}

esp_err_t pkg_store_init(void)
{
    return init_impl(false);
}

esp_err_t pkg_store_init_ram(void)
{
    return init_impl(true);
}

bool pkg_store_absent(void)
{
    return s_absent;
}

bool pkg_store_partition_present(void)
{
    return esp_partition_find_first(PKG_STORE_PARTITION_TYPE, PKG_STORE_PARTITION_SUBTYPE, PKG_STORE_PARTITION_LABEL) !=
           NULL;
}

bool pkg_store_in_ram(void)
{
    return s_ram_mode;
}

const uint8_t *pkg_store_borrow(uint32_t *len)
{
    if (!s_ram_mode || s_mutex == NULL) {
        return NULL;
    }
    lock();
    const uint8_t *p = NULL;
    if (staged_valid() && s_ram != NULL && s_borrowed == NULL) {
        p = s_ram;
        s_borrowed = s_ram;
        if (len != NULL) {
            *len = s_hdr.size;
        }
    }
    unlock();
    return p;
}

void pkg_store_return(const uint8_t *p)
{
    if (p == NULL || s_mutex == NULL) {
        return;
    }
    lock();
    if (p == s_borrowed) {
        s_borrowed = NULL;
        if (p == s_orphan) {
            heap_caps_free(s_orphan);
            s_orphan = NULL;
        }
    }
    unlock();
}

size_t pkg_store_capacity(void)
{
    if (s_ram_mode) {
        return RAM_MAX_BYTES;
    }
    if (s_part == NULL || s_part->size <= PKG_STORE_HEADER_SIZE) {
        return 0;
    }
    return s_part->size - PKG_STORE_HEADER_SIZE;
}

esp_err_t pkg_store_get_info(pkg_store_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    fill_info(out);
    unlock();
    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* The PSRAM buffer for a package of `size` bytes, when PSRAM keeps its margin beside it. Call locked. */
static esp_err_t ram_alloc(uint32_t size)
{
#if CONFIG_IDF_TARGET_LINUX
    s_ram = malloc(size);
#else
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t margin = RAM_MARGIN_BYTES;
    if (largest < (size_t)size + margin) {
        ESP_LOGW(TAG, "PSRAM: %u KB in one block, a package of %u KB needs %u KB more beside it: not staged",
                 (unsigned)(largest / 1024), (unsigned)(size / 1024), (unsigned)(margin / 1024));
        return ESP_ERR_NO_MEM;
    }
    s_ram = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    return s_ram != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t pkg_store_begin(uint32_t size, const uint8_t expected_sha256[32], pkg_store_writer_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size == 0 || size > pkg_store_capacity()) {
        return ESP_ERR_INVALID_SIZE;
    }
    lock();
    if (s_writer.open || (s_have_hdr && s_hdr.pinned)) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* The slot is empty from the first byte on: a power cut mid-write
     * leaves no header at all, never "valid but truncated". */
    esp_err_t err = erase_header();
    if (err != ESP_OK) {
        unlock();
        return err;
    }
    if (s_ram_mode) {
        ram_drop();
        err = ram_alloc(size);
        if (err != ESP_OK) {
            unlock();
            return err;
        }
    }
    memset(&s_writer, 0, sizeof(s_writer));
    s_writer.hash = psa_hash_operation_init();
    if (psa_hash_setup(&s_writer.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        ram_drop();
        unlock();
        return ESP_FAIL;
    }
    s_writer.open = true;
    s_writer.size = size;
    s_writer.check_digest = expected_sha256 != NULL;
    if (expected_sha256 != NULL) {
        memcpy(s_writer.expected, expected_sha256, sizeof(s_writer.expected));
    }
    *out = &s_writer;
    unlock();
    return ESP_OK;
}

esp_err_t pkg_store_write(pkg_store_writer_t w, const void *data, size_t len)
{
    if (w == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (w != &s_writer || !s_writer.open) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (len > s_writer.size - s_writer.written) {
        unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    uint32_t end = s_writer.written + (uint32_t)len;
    if (s_ram_mode) {
        if (len > 0) {
            memcpy(s_ram + s_writer.written, data, len);
            if (psa_hash_update(&s_writer.hash, data, len) != PSA_SUCCESS) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK) {
            s_writer.written = end;
        }
        unlock();
        return err;
    }
    /* Erase ahead, one sector at a time (never a long block erase). */
    while (err == ESP_OK && s_writer.erased < end) {
        err = esp_partition_erase_range(s_part, PKG_STORE_HEADER_SIZE + s_writer.erased, PKG_STORE_ERASE_SIZE);
        if (err == ESP_OK) {
            s_writer.erased += PKG_STORE_ERASE_SIZE;
        }
    }
    if (err == ESP_OK && len > 0) {
        err = esp_partition_write(s_part, PKG_STORE_HEADER_SIZE + s_writer.written, data, len);
    }
    if (err == ESP_OK && len > 0 && psa_hash_update(&s_writer.hash, data, len) != PSA_SUCCESS) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        s_writer.written = end;
    }
    unlock();
    return err;
}

esp_err_t pkg_store_finish(pkg_store_writer_t w, const char *version, pkg_store_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (w == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    if (w != &s_writer || !s_writer.open) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    uint8_t digest[32];
    size_t digest_len = 0;
    if (s_writer.written != s_writer.size) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (psa_hash_finish(&s_writer.hash, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
               digest_len != sizeof(digest)) {
        err = ESP_FAIL;
    } else if (s_writer.check_digest && memcmp(digest, s_writer.expected, sizeof(digest)) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        pkg_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.state = PKG_HDR_VALID;
        h.size = s_writer.size;
        memcpy(h.sha256, digest, sizeof(h.sha256));
        strlcpy(h.version, version != NULL ? version : "", sizeof(h.version));
        err = append_header(&h);
    }
    /* psa_hash_finish ends the operation whatever it returns; abort is harmless after it. */
    writer_close();
    if (err != ESP_OK) {
        ram_drop();   /* the slot stays empty */
    }
    if (err == ESP_OK && out != NULL) {
        fill_info(out);
    }
    unlock();
    if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGW(TAG, "package digest does not match: not staged");
    }
    return err;
}

void pkg_store_abort(pkg_store_writer_t w)
{
    if (w == NULL || s_mutex == NULL) {
        return;
    }
    lock();
    if (w == &s_writer) {
        writer_close();
        ram_drop();
    }
    unlock();
}

esp_err_t pkg_store_read(uint32_t offset, void *buf, size_t len)
{
    if (buf == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    esp_err_t err = ESP_OK;
    if (!staged_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (offset > s_hdr.size || len > s_hdr.size - offset) {
        err = ESP_ERR_INVALID_SIZE;
    } else if (len > 0 && s_ram_mode) {
        memcpy(buf, s_ram + offset, len);
    } else if (len > 0) {
        err = esp_partition_read(s_part, PKG_STORE_HEADER_SIZE + offset, buf, len);
    }
    unlock();
    return err;
}

esp_err_t pkg_store_verify(void)
{
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *buf = malloc(VERIFY_CHUNK);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_ESP_TASK_WDT_EN
    bool feed = esp_task_wdt_status(NULL) == ESP_OK;
#endif
    lock();
    esp_err_t err = ESP_OK;
    psa_hash_operation_t op = psa_hash_operation_init();
    if (!staged_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        err = ESP_FAIL;
    }
    for (uint32_t off = 0; err == ESP_OK && off < s_hdr.size; off += VERIFY_CHUNK) {
        size_t n = (s_hdr.size - off) < VERIFY_CHUNK ? (s_hdr.size - off) : VERIFY_CHUNK;
        if (s_ram_mode) {
            memcpy(buf, s_ram + off, n);
        } else {
            err = esp_partition_read(s_part, PKG_STORE_HEADER_SIZE + off, buf, n);
        }
        if (err == ESP_OK && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
#if CONFIG_ESP_TASK_WDT_EN
        if (feed && (off / VERIFY_CHUNK) % 64u == 0) {
            esp_task_wdt_reset();
        }
#endif
    }
    if (err == ESP_OK) {
        uint8_t digest[32];
        size_t digest_len = 0;
        if (psa_hash_finish(&op, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != sizeof(digest)) {
            err = ESP_FAIL;
        } else if (memcmp(digest, s_hdr.sha256, sizeof(digest)) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    psa_hash_abort(&op);
    unlock();
    free(buf);
    if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGE(TAG, "staged package no longer matches its digest");
    }
    return err;
}

esp_err_t pkg_store_set_pinned(bool pinned)
{
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    esp_err_t err = ESP_OK;
    if (!staged_valid()) {
        err = ESP_ERR_NOT_FOUND;
    } else if (s_hdr.pinned != pinned) {
        err = append_state(PKG_HDR_VALID, pinned);
    }
    unlock();
    return err;
}

esp_err_t pkg_store_invalidate(void)
{
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = ESP_OK;
    if (s_have_hdr && s_hdr.pinned) {
        err = ESP_ERR_INVALID_STATE;
    } else if (staged_valid()) {
        err = append_state(PKG_HDR_EMPTY, false);
    }
    unlock();
    return err;
}

esp_err_t pkg_store_release(void)
{
    if (s_absent) {
        return ESP_OK;   /* no package area: nothing can be staged */
    }
    if (s_part == NULL && !s_ram_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t err = ESP_OK;
    if (s_have_hdr && (s_hdr.state == PKG_HDR_VALID || s_hdr.pinned)) {
        err = append_state(PKG_HDR_EMPTY, false);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "staged package released");
        }
    }
    unlock();
    return err;
}
