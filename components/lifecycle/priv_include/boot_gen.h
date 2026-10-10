/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Image generation of the RTC blocks (pure logic, no IDF includes,
 * host-testable).
 *
 * RTC memory survives software, panic and watchdog resets, and each app
 * image puts its RTC blocks where its own link layout does. When image A
 * ran, then image B, and then the station comes back to A (the user's
 * rollback, a trial revert, a bootloader rollback), A finds its own blocks
 * from before B ran, intact and with valid checksums. So every RTC block
 * (lifecycle's, the ledger's, the network's) also carries a stamp: the image
 * generation of the run that wrote it.
 *
 * The generation is kept in settings (life/gen: the generation and the id
 * of the image that last recorded a boot) and changes only when another
 * image records a boot. The image id is a hash of the app's ELF SHA-256 and
 * the flash address of the slot it runs from, so the same build installed
 * into the other slot counts as another image too. Nothing is written per
 * boot: one small write per image change.
 *
 * At boot (lifecycle_init, right after the settings), boot_gen_decide()
 * compares the record with the running image:
 *   - the record names this image: the generation stays, and blocks stamped
 *     with it are this image's own (unstamped ones too, see below);
 *   - it names another image: the generation advances and is stored. Blocks
 *     stamped with the old generation were written by the run right before
 *     this boot (the other image's: used when this image's layout matches,
 *     which magic, version, size and checksum decide); any other stamp means
 *     the block predates that run and is not used, as after power loss;
 *   - no record yet: generation 1 is stored; only unstamped blocks are used;
 *   - no settings this boot (or the record cannot be read): nothing can be
 *     checked. Blocks stamped with a generation are not used (a boot without
 *     settings most often follows another image, e.g. an older app after a
 *     newer one changed the NVS format), and this boot's blocks carry no
 *     generation (BOOT_GEN_UNSTAMPED). A later boot without settings uses
 *     those, so crash counting still works across such boots (one boot
 *     later than usual), and so does the next boot with settings when the
 *     record names this image.
 * A block started over at power-on is unstamped until its boot stamps it; if
 * that boot fails before (a crash before the settings were read), the next
 * boot uses it when the record names this image or there is none.
 *
 * Limits. A run that cannot store the record (no settings, a failed write)
 * is invisible: its blocks carry the generation before it (or none). So is
 * a new image that the bootloader rolls back before it got as far as
 * recording its boot; the previous image then reads its own blocks back,
 * which are its latest state indeed, except that the reboot it planned (an
 * app update) never led to a recorded boot of the other image:
 * boot_gen_check() reports that as BOOT_GEN_SWITCH_FAILED. For that it
 * needs to know who wrote the block, so lifecycle's block also carries the
 * id of the image whose run wrote it: a block that an invisible run of
 * another image wrote (same layout, accepted stamp) holds that image's own
 * planned reboot, which did take.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stamp of a block written without a known generation. Generations start at 1. */
#define BOOT_GEN_UNSTAMPED 0u

/** life/gen v1 (settings_codec.h, little-endian): u8 version, u32 generation, u32 image id; newer writers may append. */
#define BOOT_GEN_VERSION  1u
#define BOOT_GEN_BLOB_MAX 64u   /**< read buffer; a longer record is not readable here */

/** What settings hold (filled by the caller). */
typedef struct {
    bool readable;     /**< the settings could be read this boot */
    bool found;        /**< a record is stored */
    uint32_t gen;      /**< stored generation (found) */
    uint32_t image;    /**< id of the image that stored it (found) */
} boot_gen_record_t;

/** This boot's view (boot_gen_decide). */
typedef struct {
    uint32_t accept;        /**< generation an inherited block must carry, BOOT_GEN_UNSTAMPED = none */
    bool accept_unstamped;  /**< inherited unstamped blocks are used too */
    uint32_t current;       /**< stamp of every block this boot writes */
    bool store;             /**< store {current, image} as the new record */
    bool same_image;        /**< the record names this image: no other image recorded a boot since */
    uint32_t image;         /**< id of the running image */
} boot_gen_t;

/** Image id: hash of the ELF SHA-256 and the flash address of the running slot. */
uint32_t boot_gen_image_id(const uint8_t elf_sha256[32], uint32_t slot_addr);

/** Decide this boot's generation from the stored record and the running image. */
void boot_gen_decide(const boot_gen_record_t *rec, uint32_t image, boot_gen_t *out);

/**
 * Storing the new record failed: this boot keeps the previous generation
 * (BOOT_GEN_UNSTAMPED when there was none), so the next boot, which reads
 * the same record, still uses what this one writes.
 */
void boot_gen_store_failed(boot_gen_t *g);

/** True if an inherited block with this stamp comes from the run right before this boot. */
bool boot_gen_accepts(const boot_gen_t *g, uint32_t stamp);

typedef enum {
    BOOT_GEN_USE = 0,           /**< use the inherited block (or there is none to check) */
    BOOT_GEN_STALE,             /**< it predates another image's run: treat it as lost */
    BOOT_GEN_SWITCH_FAILED,     /**< use it, but the image switch the previous boot planned did not take */
} boot_gen_verdict_t;

/**
 * Verdict on lifecycle's own block. `inherited`: the block was valid and
 * carried over (not started over at power-on or as corrupt); `stamp`: its
 * stamp; `writer`: the id of the image whose run wrote it (0 if never
 * stamped); `planned_switch`: the reboot it records was meant to start
 * another image (lifecycle_reboot_switches_image()). SWITCH_FAILED only for
 * a switch this image planned itself.
 */
boot_gen_verdict_t boot_gen_check(const boot_gen_t *g, bool inherited, uint32_t stamp, uint32_t writer,
                                  bool planned_switch);

/** Encode the record. @return bytes written, 0 if cap is too small. */
size_t boot_gen_encode(uint32_t gen, uint32_t image, uint8_t *buf, size_t cap);

/** Decode a record (version >= 1, generation not 0). @return false if damaged. */
bool boot_gen_decode(const uint8_t *buf, size_t len, uint32_t *gen, uint32_t *image);

#ifdef __cplusplus
}
#endif
