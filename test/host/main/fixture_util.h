/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Real-world fixtures (test/fixtures, see its README) for the host tests:
 * loading the data files and their oracles (pyrtcm and pynmeagps decodes,
 * JSON and JSON lines), a test-side HTTP chunked-body decoder, and the
 * fixture's frames as the framer emits them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "gnss_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Whole file under test/fixtures (rel = "rtcm/x.rtcm3"); fails the test if missing. Free with free(). */
uint8_t *fx_read(const char *rel, size_t *len);

/** oracle/index.json, parsed once and kept for the run. */
const cJSON *fx_index(void);

/** One fixture's index entry by file name (e.g. "centipede_LAX_30ep.rtcm3"); fails the test if missing. */
const cJSON *fx_entry(const char *name);

/** Integer member of a JSON object (fails the test if missing or not a number). */
long fx_int(const cJSON *obj, const char *key);

/** JSON lines file under test/fixtures: one parsed object per line. */
typedef struct {
    cJSON **rec;
    size_t n;
} fx_jsonl_t;

/** Load a JSON lines file; false if it does not exist. */
bool fx_jsonl_load(const char *rel, fx_jsonl_t *out);
void fx_jsonl_free(fx_jsonl_t *j);

/**
 * Decode an HTTP/1.1 chunked body (size line in hex, optional extensions
 * after ';', CRLF, data, CRLF; a 0 chunk ends it). Stops at the end marker,
 * or at the first chunk that is cut or malformed (*complete false then).
 * @return bytes written to out (out must hold len bytes).
 */
size_t fx_dechunk(const uint8_t *in, size_t len, uint8_t *out, size_t *chunks, bool *complete);

/** A fixture's input bytes as the station would receive them (de-chunked when the index says so). */
uint8_t *fx_stream(const char *name, size_t *len);

/** One unit the framer emitted, with its stream offset (frames and lines). */
typedef struct {
    gnss_frame_kind_t kind;
    uint16_t type;
    size_t off;          /**< offset of the first byte in the stream */
    size_t len;
    const uint8_t *data; /**< points into the stream buffer */
} fx_unit_t;

typedef struct {
    fx_unit_t *u;
    size_t n;
    size_t cap;
    gnss_framing_stats_t stats;
} fx_units_t;

/**
 * Run the framer over a buffer fed in pieces of `piece` bytes (0 = all at
 * once) and record every emitted unit. Each unit is located in the buffer
 * (the framer reports none of its own offsets): the search starts at the end
 * of the previous unit and the unit must be found there byte for byte.
 */
void fx_frame(const uint8_t *buf, size_t len, size_t piece, fx_units_t *out);
void fx_units_free(fx_units_t *u);

/** MSM constellation name as the oracle writes it ("GPS", "GLONASS", ...). */
const char *fx_sys_name(unsigned sys);

#ifdef __cplusplus
}
#endif
