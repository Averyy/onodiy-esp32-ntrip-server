/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "boot_gen.h"

#include <string.h>

#include "settings_codec.h"

uint32_t boot_gen_image_id(const uint8_t elf_sha256[32], uint32_t slot_addr)
{
    /* FNV-1a over the hash, then the slot address (little-endian). */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < 32; i++) {
        h ^= elf_sha256[i];
        h *= 16777619u;
    }
    for (unsigned b = 0; b < 4; b++) {
        h ^= (slot_addr >> (8 * b)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

void boot_gen_decide(const boot_gen_record_t *rec, uint32_t image, boot_gen_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!rec->readable) {
        out->accept_unstamped = true;   /* only what earlier boots without settings wrote */
        return;
    }
    if (!rec->found || rec->gen == BOOT_GEN_UNSTAMPED) {
        out->accept_unstamped = true;
        out->current = 1;
        out->store = true;
        return;
    }
    out->accept = rec->gen;
    if (rec->image == image) {
        out->same_image = true;
        out->accept_unstamped = true;
        out->current = rec->gen;
        return;
    }
    out->current = (rec->gen == UINT32_MAX) ? 1u : rec->gen + 1u;
    out->store = true;
}

void boot_gen_store_failed(boot_gen_t *g)
{
    g->store = false;
    g->current = g->accept;
}

bool boot_gen_accepts(const boot_gen_t *g, uint32_t stamp)
{
    if (stamp == BOOT_GEN_UNSTAMPED) {
        return g->accept_unstamped;
    }
    return stamp == g->accept;
}

boot_gen_verdict_t boot_gen_check(const boot_gen_t *g, bool inherited, uint32_t stamp, bool planned_switch)
{
    if (!inherited) {
        return BOOT_GEN_USE;   /* started over in this boot: nothing carried over */
    }
    if (!boot_gen_accepts(g, stamp)) {
        return BOOT_GEN_STALE;
    }
    if (g->same_image && planned_switch) {
        return BOOT_GEN_SWITCH_FAILED;
    }
    return BOOT_GEN_USE;
}

size_t boot_gen_encode(uint32_t gen, uint32_t image, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, BOOT_GEN_VERSION);
    blob_put_u32(&w, gen);
    blob_put_u32(&w, image);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool boot_gen_decode(const uint8_t *buf, size_t len, uint32_t *gen, uint32_t *image)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    uint8_t version = blob_get_u8(&r);
    uint32_t g = blob_get_u32(&r);
    uint32_t id = blob_get_u32(&r);
    if (!blob_reader_ok(&r) || version < BOOT_GEN_VERSION || g == BOOT_GEN_UNSTAMPED) {
        return false;
    }
    *gen = g;
    *image = id;
    return true;
}
