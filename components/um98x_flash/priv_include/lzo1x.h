/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * LZO1X decompressor (pure, bounds-checked; written from the public
 * description of the LZO1X stream format). Used only to check the receiver
 * package's compressed blocks before a flash: every input and output access
 * is checked, so a damaged or hostile stream fails instead of reading or
 * writing out of bounds.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LZO1X_OK = 0,
    LZO1X_INPUT_OVERRUN,     /**< the stream ends inside an instruction */
    LZO1X_OUTPUT_OVERRUN,    /**< more output than the buffer holds */
    LZO1X_LOOKBEHIND,        /**< a match reaches before the start of the output */
    LZO1X_INPUT_LEFT,        /**< bytes after the end-of-stream marker */
    LZO1X_NO_END,            /**< the input ended without the end-of-stream marker */
} lzo1x_result_t;

/**
 * Decompress `in` (exactly one LZO1X stream, ending with its end marker)
 * into `out` (capacity out_cap). *out_len gets the bytes produced.
 */
lzo1x_result_t lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
