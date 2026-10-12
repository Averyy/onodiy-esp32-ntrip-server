/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * One caster slot's stored configuration (settings namespace "cst<N>", key
 * "cfg"): a version byte (1), the fields in a fixed order, then a layout
 * byte appended later and a flags byte after it (a reader from before
 * either stops ahead of it, so an image rolled back to still reads the
 * record; a record without the flags byte reads as no flags).
 *
 * Flags: bit 0 test_onocoy (a test Onocoy slot; only a build with
 * CONFIG_NTRIP_TEST_ONOCOY_SLOT acts on it). Other bits are written 0 and
 * ignored.
 *
 * Layouts (the meaning of an Onocoy slot's fields):
 *   1  (no layout byte: written before it existed) an Onocoy slot held the
 *      Onocoy credential username in `mountpoint` (NTRIP 1.0 sent it as the
 *      mountpoint, 2.0 posted to `label` with it as the user). `username`
 *      belonged to other NTRIP 2.0 casters only: a slot switched to Onocoy
 *      in place kept the old caster's username, which meant nothing for
 *      Onocoy.
 *   2  an Onocoy slot holds what the Onocoy console gives: mountpoint,
 *      username and password (ntrip_wire_names).
 * An Onocoy slot of layout 1 is read as that older layout whatever its
 * username says: the username is dropped, so the slot keeps streaming as
 * before the update and does not count as set up (ntrip_server_onocoy_ready)
 * until the user enters the console's values. Every record is written as
 * layout 2.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntrip_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTRIP_CFG_BLOB_VERSION 1u
#define NTRIP_CFG_LAYOUT       2u     /* the layout byte every record is written with */
#define NTRIP_CFG_FLAG_TEST_ONOCOY 0x01u   /* flags byte, bit 0 */
#define NTRIP_CFG_BLOB_MAX     320u   /* largest record: 309 bytes */

/** The protocol, trust mode and backoff profile are values the firmware knows. */
bool ntrip_cfg_enums_valid(const ntrip_caster_cfg_t *c);

/** Encode a slot (with the current layout byte). @return bytes written, 0 if cap is too small. */
size_t ntrip_cfg_encode(const ntrip_caster_cfg_t *c, uint8_t *buf, size_t cap);

/**
 * Decode a stored slot (see above for an Onocoy slot of layout 1). Sets
 * password_set; other output-only fields stay 0. @return false for another
 * version, a damaged record or enumerations out of range.
 */
bool ntrip_cfg_decode(const uint8_t *buf, size_t len, ntrip_caster_cfg_t *out);

#ifdef __cplusplus
}
#endif
