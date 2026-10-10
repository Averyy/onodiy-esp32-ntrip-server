/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "flash_journal.h"
#include "unity.h"

static void sample(um98x_flash_journal_t *j)
{
    memset(j, 0, sizeof(*j));
    j->version = FLASH_JOURNAL_VERSION;
    j->state = UM98X_JOURNAL_TRANSFERRING;
    j->phase = UM98X_JOURNAL_TRANSFERRING;
    for (size_t i = 0; i < sizeof(j->sha256); i++) {
        j->sha256[i] = (uint8_t)(0xA0 + i);
    }
    j->size = 3027628;
    strcpy(j->expected_fw, "R4.10Build18975");
    j->expected_build = 18975;
    strcpy(j->pre_model, "UM982");
    strcpy(j->pre_fw, "R4.10Build13495");
    j->pre_build = 13495;
    j->app_baud = 115200;
    j->boot_baud = 460800;
    j->transfers = 2;
    j->entry_attempts = 1;
    j->last_acked_block = 1984;
    j->failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    j->changed_unix = 1791590400;
}

TEST_CASE("flash journal round-trips every field", "[flash_journal]")
{
    um98x_flash_journal_t in;
    um98x_flash_journal_t out;
    sample(&in);
    uint8_t buf[FLASH_JOURNAL_V1_MAX];
    size_t len = flash_journal_encode(&in, buf, sizeof(buf));
    TEST_ASSERT_NOT_EQUAL(0, len);
    TEST_ASSERT_TRUE(len <= FLASH_JOURNAL_V1_MAX);
    memset(&out, 0x5A, sizeof(out));
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_OK, flash_journal_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT8(FLASH_JOURNAL_VERSION, out.version);
    TEST_ASSERT_EQUAL_UINT8(in.state, out.state);
    TEST_ASSERT_EQUAL_UINT8(in.phase, out.phase);
    TEST_ASSERT_EQUAL_MEMORY(in.sha256, out.sha256, sizeof(in.sha256));
    TEST_ASSERT_EQUAL_UINT32(in.size, out.size);
    TEST_ASSERT_EQUAL_STRING(in.expected_fw, out.expected_fw);
    TEST_ASSERT_EQUAL_UINT32(in.expected_build, out.expected_build);
    TEST_ASSERT_EQUAL_STRING(in.pre_model, out.pre_model);
    TEST_ASSERT_EQUAL_STRING(in.pre_fw, out.pre_fw);
    TEST_ASSERT_EQUAL_UINT32(in.pre_build, out.pre_build);
    TEST_ASSERT_EQUAL_UINT32(in.app_baud, out.app_baud);
    TEST_ASSERT_EQUAL_UINT32(in.boot_baud, out.boot_baud);
    TEST_ASSERT_EQUAL_UINT8(in.transfers, out.transfers);
    TEST_ASSERT_EQUAL_UINT8(in.entry_attempts, out.entry_attempts);
    TEST_ASSERT_EQUAL_UINT16(in.last_acked_block, out.last_acked_block);
    TEST_ASSERT_EQUAL_UINT8(in.failure_class, out.failure_class);
    TEST_ASSERT_EQUAL_INT64(in.changed_unix, out.changed_unix);

    /* Longest strings still fit the v1 maximum. */
    memset(in.expected_fw, 'x', sizeof(in.expected_fw) - 1);
    memset(in.pre_fw, 'y', sizeof(in.pre_fw) - 1);
    memset(in.pre_model, 'z', sizeof(in.pre_model) - 1);
    TEST_ASSERT_EQUAL_size_t(FLASH_JOURNAL_V1_MAX, flash_journal_encode(&in, buf, sizeof(buf)));
}

TEST_CASE("flash journal from a newer schema is reported, not guessed", "[flash_journal]")
{
    um98x_flash_journal_t j;
    um98x_flash_journal_t out;
    sample(&j);
    uint8_t buf[FLASH_JOURNAL_READ_MAX];
    size_t len = flash_journal_encode(&j, buf, sizeof(buf));
    buf[0] = FLASH_JOURNAL_VERSION + 1;
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_UNKNOWN_VERSION, flash_journal_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.state);               /* cleared */
    buf[0] = 0;
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(buf, len, &out));
}

TEST_CASE("flash journal: truncated or out-of-range records are damaged", "[flash_journal]")
{
    um98x_flash_journal_t j;
    um98x_flash_journal_t out;
    sample(&j);
    uint8_t buf[FLASH_JOURNAL_READ_MAX];
    size_t len = flash_journal_encode(&j, buf, sizeof(buf));
    for (size_t cut = 0; cut < len; cut++) {
        TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(buf, cut, &out));
    }
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(NULL, 0, &out));

    buf[1] = 0;                                          /* state */
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(buf, len, &out));
    buf[1] = UM98X_JOURNAL_NEEDS_USER + 1;
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(buf, len, &out));
    buf[1] = UM98X_JOURNAL_NEEDS_USER;
    buf[2] = UM98X_JOURNAL_FAILED;                       /* phase must be a main state */
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_DAMAGED, flash_journal_decode(buf, len, &out));
    buf[2] = UM98X_JOURNAL_EOT_SENT;
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_OK, flash_journal_decode(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT8(UM98X_JOURNAL_NEEDS_USER, out.state);
    TEST_ASSERT_EQUAL_UINT8(UM98X_JOURNAL_EOT_SENT, out.phase);

    /* A main state with a different phase is inconsistent; encode refuses it too. */
    j.state = UM98X_JOURNAL_VERIFYING;
    j.phase = UM98X_JOURNAL_TRANSFERRING;
    TEST_ASSERT_EQUAL_size_t(0, flash_journal_encode(&j, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, flash_journal_encode(&j, buf, 10));
}

TEST_CASE("flash journal ignores fields a newer writer appended", "[flash_journal]")
{
    um98x_flash_journal_t j;
    um98x_flash_journal_t out;
    sample(&j);
    uint8_t buf[FLASH_JOURNAL_READ_MAX];
    size_t len = flash_journal_encode(&j, buf, sizeof(buf));
    memset(&buf[len], 0xEE, 16);
    TEST_ASSERT_EQUAL(FLASH_JOURNAL_OK, flash_journal_decode(buf, len + 16, &out));
    TEST_ASSERT_EQUAL_INT64(j.changed_unix, out.changed_unix);
}

TEST_CASE("flash journal: the boot race only after both chips reset in the loader phase", "[flash_journal]")
{
    um98x_flash_journal_t j;
    sample(&j);
    static const struct {
        uint8_t state;
        uint8_t phase;
        bool race;
    } rows[] = {
        { UM98X_JOURNAL_PREPARED, UM98X_JOURNAL_PREPARED, false },
        { UM98X_JOURNAL_BAUD_CHANGING, UM98X_JOURNAL_BAUD_CHANGING, false },
        { UM98X_JOURNAL_RESET_SENT, UM98X_JOURNAL_RESET_SENT, false },
        { UM98X_JOURNAL_MENU2_SENT, UM98X_JOURNAL_MENU2_SENT, true },
        { UM98X_JOURNAL_TRANSFERRING, UM98X_JOURNAL_TRANSFERRING, true },
        { UM98X_JOURNAL_EOT_SENT, UM98X_JOURNAL_EOT_SENT, true },
        { UM98X_JOURNAL_VERIFYING, UM98X_JOURNAL_VERIFYING, true },
        { UM98X_JOURNAL_FAILED, UM98X_JOURNAL_TRANSFERRING, true },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_RESET_SENT, false },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_MENU2_SENT, true },
        { UM98X_JOURNAL_NEEDS_USER, UM98X_JOURNAL_VERIFYING, true },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        j.state = rows[i].state;
        j.phase = rows[i].phase;
        j.transfers = rows[i].phase >= UM98X_JOURNAL_TRANSFERRING ? 1 : 0;
        TEST_ASSERT_EQUAL_MESSAGE(rows[i].race, flash_journal_needs_race(&j, true), "power-on / brownout");
        /* An ESP32-only reset leaves the receiver where it was: never a race. */
        TEST_ASSERT_FALSE(flash_journal_needs_race(&j, false));
    }
    TEST_ASSERT_FALSE(flash_journal_needs_race(NULL, true));

    /* A retry after the point of no return records the entry states again:
     * the flash is presumed modified, so a power cut there races too. */
    j.state = UM98X_JOURNAL_RESET_SENT;
    j.phase = UM98X_JOURNAL_RESET_SENT;
    j.transfers = 1;
    TEST_ASSERT_TRUE(flash_journal_needs_race(&j, true));
    TEST_ASSERT_FALSE(flash_journal_needs_race(&j, false));
}
