/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "boot_gen.h"
#include "unity.h"

#define IMG_A 0x0A0A0A0Au
#define IMG_B 0x0B0B0B0Bu

/* The record as settings hold it; `stored` is updated like lifecycle does. */
typedef struct {
    bool readable;
    bool found;
    uint32_t gen;
    uint32_t image;
} store_t;

static boot_gen_t boot_as(store_t *nvs, uint32_t image, bool write_ok)
{
    boot_gen_record_t rec = { nvs->readable, nvs->found, nvs->gen, nvs->image };
    boot_gen_t g;
    boot_gen_decide(&rec, image, &g);
    if (g.store) {
        if (write_ok) {
            nvs->found = true;
            nvs->gen = g.current;
            nvs->image = image;
        } else {
            boot_gen_store_failed(&g);
        }
    }
    return g;
}

TEST_CASE("image generation: the same image keeps it; blocks from its own run are used", "[boot_gen]")
{
    store_t nvs = { .readable = true };
    boot_gen_t g = boot_as(&nvs, IMG_A, true);          /* first boot ever */
    TEST_ASSERT_EQUAL_UINT32(1, g.current);
    TEST_ASSERT_TRUE(nvs.found);
    TEST_ASSERT_FALSE(g.same_image);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
    TEST_ASSERT_FALSE(boot_gen_accepts(&g, 1));         /* nothing was recorded before */

    uint32_t stamp = g.current;                         /* what A's blocks carry */
    for (int i = 0; i < 3; i++) {
        g = boot_as(&nvs, IMG_A, true);
        TEST_ASSERT_TRUE(g.same_image);
        TEST_ASSERT_FALSE(g.store);                     /* no write per boot */
        TEST_ASSERT_EQUAL_UINT32(stamp, g.current);
        TEST_ASSERT_TRUE(boot_gen_accepts(&g, stamp));
        TEST_ASSERT_TRUE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));   /* started over, crashed before stamping */
    }
}

TEST_CASE("image generation: back on an image after another ran, its old blocks are stale", "[boot_gen]")
{
    store_t nvs = { .readable = true };
    boot_gen_t g = boot_as(&nvs, IMG_A, true);
    uint32_t a_stamp = g.current;

    /* B boots (an update): A's blocks are the run right before (used if B's
     * layout matches); B stamps a new generation. */
    g = boot_as(&nvs, IMG_B, true);
    TEST_ASSERT_TRUE(g.store);
    TEST_ASSERT_FALSE(g.same_image);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, a_stamp));
    TEST_ASSERT_FALSE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
    uint32_t b_stamp = g.current;
    TEST_ASSERT_NOT_EQUAL(a_stamp, b_stamp);
    g = boot_as(&nvs, IMG_B, true);                     /* B runs on */
    TEST_ASSERT_EQUAL_UINT32(b_stamp, g.current);

    /* Back on A (rollback): its own blocks from before B are stale; B's
     * (at the same place only if the layouts match) are the latest. */
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_FALSE(boot_gen_accepts(&g, a_stamp));
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, b_stamp));
    TEST_ASSERT_FALSE(g.same_image);
    TEST_ASSERT_EQUAL(BOOT_GEN_STALE, boot_gen_check(&g, true, a_stamp, false));
    TEST_ASSERT_EQUAL(BOOT_GEN_USE, boot_gen_check(&g, true, b_stamp, true));   /* B's planned switch took */
    TEST_ASSERT_EQUAL(BOOT_GEN_USE, boot_gen_check(&g, false, a_stamp, false)); /* started over anyway */
    uint32_t a2 = g.current;
    TEST_ASSERT_NOT_EQUAL(b_stamp, a2);
    TEST_ASSERT_NOT_EQUAL(a_stamp, a2);

    /* A's next boot uses what this run wrote. */
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, a2));
}

TEST_CASE("image generation: a new image that never recorded a boot leaves it; the switch did not take",
          "[boot_gen]")
{
    store_t nvs = { .readable = true };
    boot_gen_t g = boot_as(&nvs, IMG_A, true);
    uint32_t a_stamp = g.current;
    /* A plans an app update; B fails before it records a boot and the
     * bootloader rolls back to A. */
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_TRUE(g.same_image);
    TEST_ASSERT_EQUAL(BOOT_GEN_SWITCH_FAILED, boot_gen_check(&g, true, a_stamp, true));
    TEST_ASSERT_EQUAL(BOOT_GEN_USE, boot_gen_check(&g, true, a_stamp, false));
    TEST_ASSERT_EQUAL(BOOT_GEN_USE, boot_gen_check(&g, false, BOOT_GEN_UNSTAMPED, true));
}

TEST_CASE("image generation: without settings nothing stamped is used; unstamped blocks chain", "[boot_gen]")
{
    store_t nvs = { .readable = true };
    boot_gen_t g = boot_as(&nvs, IMG_A, true);
    uint32_t a_stamp = g.current;

    /* A boot without settings cannot tell whether A's block predates
     * another image's run: not used; this boot's blocks are unstamped. */
    nvs.readable = false;
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_FALSE(g.store);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GEN_UNSTAMPED, g.current);
    TEST_ASSERT_FALSE(boot_gen_accepts(&g, a_stamp));
    TEST_ASSERT_EQUAL(BOOT_GEN_STALE, boot_gen_check(&g, true, a_stamp, false));
    /* ...and the next boot without settings uses them (crash counting). */
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
    TEST_ASSERT_FALSE(g.same_image);

    /* Settings back, record names A: the unstamped blocks are A's. */
    nvs.readable = true;
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
    TEST_ASSERT_EQUAL_UINT32(a_stamp, g.current);

    /* Settings back after another image recorded a boot: unstamped blocks
     * may be from before it, not used. */
    g = boot_as(&nvs, IMG_B, true);
    g = boot_as(&nvs, IMG_A, true);
    TEST_ASSERT_FALSE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
}

TEST_CASE("image generation: a failed record write keeps the previous generation", "[boot_gen]")
{
    store_t nvs = { .readable = true };
    boot_gen_t g = boot_as(&nvs, IMG_A, true);
    uint32_t a_stamp = g.current;

    g = boot_as(&nvs, IMG_B, false);                    /* B cannot store its generation */
    TEST_ASSERT_FALSE(g.store);
    TEST_ASSERT_EQUAL_UINT32(a_stamp, g.current);
    g = boot_as(&nvs, IMG_B, false);                    /* B's next boot: same record, uses B's blocks */
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, a_stamp));

    /* No record at all and the write fails: unstamped, accepted next time. */
    store_t empty = { .readable = true };
    g = boot_as(&empty, IMG_A, false);
    TEST_ASSERT_EQUAL_UINT32(BOOT_GEN_UNSTAMPED, g.current);
    g = boot_as(&empty, IMG_A, false);
    TEST_ASSERT_TRUE(boot_gen_accepts(&g, BOOT_GEN_UNSTAMPED));
}

TEST_CASE("image generation: record codec and image id", "[boot_gen]")
{
    uint8_t buf[BOOT_GEN_BLOB_MAX];
    size_t len = boot_gen_encode(41, IMG_B, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(9, len);
    uint32_t gen = 0;
    uint32_t image = 0;
    TEST_ASSERT_TRUE(boot_gen_decode(buf, len, &gen, &image));
    TEST_ASSERT_EQUAL_UINT32(41, gen);
    TEST_ASSERT_EQUAL_UINT32(IMG_B, image);
    buf[len] = 0x77;                                    /* a newer writer's extra field */
    TEST_ASSERT_TRUE(boot_gen_decode(buf, len + 1u, &gen, &image));
    TEST_ASSERT_FALSE(boot_gen_decode(buf, len - 1u, &gen, &image));
    TEST_ASSERT_EQUAL_size_t(0, boot_gen_encode(1, 1, buf, 8));
    len = boot_gen_encode(BOOT_GEN_UNSTAMPED, IMG_A, buf, sizeof(buf));
    TEST_ASSERT_FALSE(boot_gen_decode(buf, len, &gen, &image));   /* generation 0 is never stored */
    buf[0] = 0;
    TEST_ASSERT_FALSE(boot_gen_decode(buf, len, &gen, &image));

    /* The generation wraps past 0. */
    store_t nvs = { .readable = true, .found = true, .gen = UINT32_MAX, .image = IMG_A };
    boot_gen_t g = boot_as(&nvs, IMG_B, true);
    TEST_ASSERT_EQUAL_UINT32(1, g.current);

    /* Same build in the other slot is another image; another build too. */
    uint8_t sha[32];
    memset(sha, 0x11, sizeof(sha));
    uint32_t a0 = boot_gen_image_id(sha, 0x20000);
    TEST_ASSERT_EQUAL_UINT32(a0, boot_gen_image_id(sha, 0x20000));
    TEST_ASSERT_NOT_EQUAL(a0, boot_gen_image_id(sha, 0x420000));
    sha[31] ^= 1;
    TEST_ASSERT_NOT_EQUAL(a0, boot_gen_image_id(sha, 0x20000));
}
