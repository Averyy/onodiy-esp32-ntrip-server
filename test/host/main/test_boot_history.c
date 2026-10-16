/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "boot_history.h"
#include "settings_codec.h"
#include "unity.h"

static void fill(boot_history_entry_t *e, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        memset(&e[i], 0, sizeof(e[i]));
        e[i].cls = (uint8_t)(i % 5);
        e[i].raw = (uint8_t)(i + 1);
        e[i].reboot_reason = (uint16_t)(i * 3);
        e[i].boot_no = 1000u + (uint32_t)i;
        e[i].prev_uptime_s = (i % 3 == 0) ? BOOT_HISTORY_UPTIME_UNKNOWN : 86400u * 40u + (uint32_t)i;
        /* longest version esp_app_desc_t can carry */
        snprintf(e[i].version, sizeof(e[i].version), "v1.0.0-rc.1-12-g47fc47b-d+%05u", (unsigned)(i % 100000u));
        e[i].start_unix = (i % 4 == 1) ? 0 : 1760000000u - (uint32_t)i * 3600u;
    }
}

/* Entries with a release-length version ("v0.3.0"): the start times fit beside them. */
static void fill_short(boot_history_entry_t *e, size_t n)
{
    fill(e, n);
    for (size_t i = 0; i < n; i++) {
        snprintf(e[i].version, sizeof(e[i].version), "v0.3.%u", (unsigned)(i % 10u));
    }
}

/* The v1 entries alone, as a build without start times writes them. */
static size_t encode_plain(const boot_history_entry_t *e, size_t n, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, BOOT_HISTORY_VERSION);
    blob_put_u8(&w, (uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        blob_put_u8(&w, e[i].cls);
        blob_put_u8(&w, e[i].raw);
        blob_put_u16(&w, e[i].reboot_reason);
        blob_put_u32(&w, e[i].boot_no);
        blob_put_u32(&w, e[i].prev_uptime_s);
        blob_put_str(&w, e[i].version);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

TEST_CASE("boot history round-trips, newest first", "[boot_history]")
{
    boot_history_entry_t in[3];
    boot_history_entry_t out[BOOT_HISTORY_LEN];
    fill(in, 3);
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = boot_history_encode(in, 3, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    size_t count = 0;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT8(in[i].cls, out[i].cls);
        TEST_ASSERT_EQUAL_UINT8(in[i].raw, out[i].raw);
        TEST_ASSERT_EQUAL_UINT16(in[i].reboot_reason, out[i].reboot_reason);
        TEST_ASSERT_EQUAL_UINT32(in[i].boot_no, out[i].boot_no);
        TEST_ASSERT_EQUAL_UINT32(in[i].prev_uptime_s, out[i].prev_uptime_s);
        TEST_ASSERT_EQUAL_STRING(in[i].version, out[i].version);
        TEST_ASSERT_EQUAL_UINT32(in[i].start_unix, out[i].start_unix);
    }
}

TEST_CASE("boot history: start times follow the entries, also past the slots a boot leaves", "[boot_history]")
{
    boot_history_entry_t e[BOOT_HISTORY_LEN];
    fill_short(e, BOOT_HISTORY_LEN);
    uint8_t plain[BOOT_HISTORY_BLOB_MAX];
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = boot_history_encode(e, BOOT_HISTORY_LEN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(encode_plain(e, BOOT_HISTORY_LEN, plain, sizeof(plain)) + 4 * BOOT_HISTORY_LEN, len);

    /* What lifecycle does at boot: the oldest entry is not read, the others keep their times one slot down. */
    boot_history_entry_t next[BOOT_HISTORY_LEN];
    size_t count = 0;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, &next[1], BOOT_HISTORY_LEN - 1, &count));
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_LEN - 1, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(e[i].boot_no, next[i + 1].boot_no);
        TEST_ASSERT_EQUAL_UINT32(e[i].start_unix, next[i + 1].start_unix);
    }
    TEST_ASSERT_EQUAL_UINT32(0, next[2].start_unix);   /* not dated stays not dated */
    TEST_ASSERT_NOT_EQUAL(0, next[1].start_unix);
}

TEST_CASE("boot history: an older build's blob reads undated, ours starts with its bytes", "[boot_history]")
{
    boot_history_entry_t e[3];
    boot_history_entry_t out[BOOT_HISTORY_LEN];
    fill_short(e, 3);
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t count = 0;

    /* Its blob (no times): every entry, none dated. */
    size_t len = encode_plain(e, 3, buf, sizeof(buf));
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(e[i].boot_no, out[i].boot_no);
        TEST_ASSERT_EQUAL_UINT32(0, out[i].start_unix);
    }

    /* Ours, as it reads it: the times come after its last entry, so its entries are the same bytes. */
    uint8_t plain[BOOT_HISTORY_BLOB_MAX];
    size_t plain_len = encode_plain(e, 3, plain, sizeof(plain));
    len = boot_history_encode(e, 3, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(plain_len + 12, len);
    TEST_ASSERT_EQUAL_MEMORY(plain, buf, plain_len);

    /* Times cut short: the entries stay, no time is trusted. */
    TEST_ASSERT_TRUE(boot_history_decode(buf, len - 1, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(3, count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, out[i].start_unix);
    }
}

TEST_CASE("boot history: a full history fits and a new boot drops the oldest", "[boot_history]")
{
    boot_history_entry_t e[BOOT_HISTORY_LEN];
    fill(e, BOOT_HISTORY_LEN);
    TEST_ASSERT_EQUAL_size_t(31, strlen(e[0].version));
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = boot_history_encode(e, BOOT_HISTORY_LEN, buf, sizeof(buf));
    /* The longest versions leave no room for the start times: they are left out, every v1 reader still holds it. */
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_BLOB_MAX, len);

    /* What lifecycle does at boot: old entries go behind the new one. */
    boot_history_entry_t next[BOOT_HISTORY_LEN];
    size_t count = 0;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, &next[1], BOOT_HISTORY_LEN - 1, &count));
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_LEN - 1, count);
    TEST_ASSERT_EQUAL_UINT32(e[0].boot_no, next[1].boot_no);
    TEST_ASSERT_EQUAL_UINT32(e[0].prev_uptime_s, next[1].prev_uptime_s);
    TEST_ASSERT_EQUAL_UINT32(e[BOOT_HISTORY_LEN - 2].boot_no, next[BOOT_HISTORY_LEN - 1].boot_no);
    TEST_ASSERT_EQUAL_UINT32(e[BOOT_HISTORY_LEN - 2].prev_uptime_s, next[BOOT_HISTORY_LEN - 1].prev_uptime_s);
    TEST_ASSERT_EQUAL_UINT32(0, next[1].start_unix);

    /* Encode never writes more than BOOT_HISTORY_LEN entries. */
    boot_history_entry_t many[BOOT_HISTORY_LEN + 4];
    fill(many, BOOT_HISTORY_LEN + 4);
    len = boot_history_encode(many, BOOT_HISTORY_LEN + 4, buf, sizeof(buf));
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, e, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_LEN, count);
}

TEST_CASE("boot history leaves a newer layout alone", "[boot_history]")
{
    boot_history_entry_t e[2];
    boot_history_entry_t out[BOOT_HISTORY_LEN];
    fill(e, 2);
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = boot_history_encode(e, 2, buf, sizeof(buf));
    size_t count = 99;

    buf[0] = BOOT_HISTORY_VERSION + 1;               /* written by a newer app */
    TEST_ASSERT_FALSE(boot_history_decode(buf, len, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_FALSE(boot_history_decode(buf, 0, out, BOOT_HISTORY_LEN, &count));

    /* A damaged tail of our own layout only loses the damaged entry (and the start times after it). */
    buf[0] = BOOT_HISTORY_VERSION;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len - 2 * 4 - 1, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_UINT32(e[0].boot_no, out[0].boot_no);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].start_unix);

    TEST_ASSERT_EQUAL_size_t(0, boot_history_encode(e, 2, buf, 10));   /* too small */
}
