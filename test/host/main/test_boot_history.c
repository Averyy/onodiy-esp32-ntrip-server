/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "boot_history.h"
#include "unity.h"

static void fill(boot_history_entry_t *e, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        memset(&e[i], 0, sizeof(e[i]));
        e[i].cls = (uint8_t)(i % 5);
        e[i].raw = (uint8_t)(i + 1);
        e[i].reboot_reason = (uint16_t)(i * 3);
        e[i].boot_no = 1000u + (uint32_t)i;
        /* longest version esp_app_desc_t can carry */
        snprintf(e[i].version, sizeof(e[i].version), "v1.0.0-rc.1-12-g47fc47b-d+%05u", (unsigned)(i % 100000u));
    }
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
        TEST_ASSERT_EQUAL_STRING(in[i].version, out[i].version);
    }
}

TEST_CASE("boot history: a full history fits and a new boot drops the oldest", "[boot_history]")
{
    boot_history_entry_t e[BOOT_HISTORY_LEN];
    fill(e, BOOT_HISTORY_LEN);
    TEST_ASSERT_EQUAL_size_t(31, strlen(e[0].version));
    uint8_t buf[BOOT_HISTORY_BLOB_MAX];
    size_t len = boot_history_encode(e, BOOT_HISTORY_LEN, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_BLOB_MAX, len);

    /* What lifecycle does at boot: old entries go behind the new one. */
    boot_history_entry_t next[BOOT_HISTORY_LEN];
    size_t count = 0;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len, &next[1], BOOT_HISTORY_LEN - 1, &count));
    TEST_ASSERT_EQUAL_size_t(BOOT_HISTORY_LEN - 1, count);
    TEST_ASSERT_EQUAL_UINT32(e[0].boot_no, next[1].boot_no);
    TEST_ASSERT_EQUAL_UINT32(e[BOOT_HISTORY_LEN - 2].boot_no, next[BOOT_HISTORY_LEN - 1].boot_no);

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

    /* A damaged tail of our own layout only loses the damaged entry. */
    buf[0] = BOOT_HISTORY_VERSION;
    TEST_ASSERT_TRUE(boot_history_decode(buf, len - 1, out, BOOT_HISTORY_LEN, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_UINT32(e[0].boot_no, out[0].boot_no);

    TEST_ASSERT_EQUAL_size_t(0, boot_history_encode(e, 2, buf, 10));   /* too small */
}
