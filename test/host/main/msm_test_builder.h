/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Builds complete MSM4..MSM7 frames for the observation tests, following the
 * public RTCM 10403 layout: the 169-bit header, the cell mask, then the
 * satellite data and the signal data, each sent field by field (every
 * satellite's value of one field, then the next field). Bits per satellite
 * and cell: MSM4 18/48, MSM5 36/63, MSM6 18/65, MSM7 36/80.
 *
 * Every field other than the CNR is filled with a non-zero pattern, so a
 * decoder that reads the CNR at a wrong offset gets visibly wrong values.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gnss_framing.h"
#include "gnss_rtcm3.h"
#include "rtcm_vectors.h"

typedef struct {
    uint16_t type;             /**< e.g. 1097 (Galileo MSM7) */
    uint16_t station;
    uint32_t epoch;            /**< 30-bit epoch time as sent */
    bool more;                 /**< multiple-message bit */
    const uint8_t *sats;       /**< satellite numbers, ascending */
    unsigned nsats;
    const uint8_t *sigs;       /**< signal IDs, ascending */
    unsigned nsigs;
    uint64_t absent;           /**< grid cells (bit sat*nsigs + sig) left out of the cell mask */
    const uint16_t *cnr;       /**< raw CNR field per present cell, in sent order (NULL = all 0) */
} msm_desc_t;

/* Write n <= 64 bits of v at bit `pos` (bit 0 = MSB of p[0]). */
static inline void msm_put(uint8_t *p, size_t pos, unsigned n, uint64_t v)
{
    for (unsigned i = 0; i < n; i++) {
        size_t b = pos + i;
        uint8_t mask = (uint8_t)(0x80u >> (b & 7u));
        if ((v >> (n - 1 - i)) & 1u) {
            p[b >> 3] |= mask;
        } else {
            p[b >> 3] &= (uint8_t)~mask;
        }
    }
}

/* Write one field for `count` items: a pattern unless `values` is given. */
static inline size_t msm_fields(uint8_t *p, size_t pos, unsigned width, unsigned count, const uint16_t *values)
{
    for (unsigned i = 0; i < count; i++) {
        uint64_t v = values ? values[i] : ((0x5A5A5A5Au ^ (i * 0x1111u)) | 1u);
        msm_put(p, pos, width, v & ((width == 64) ? ~0ull : ((1ull << width) - 1u)));
        pos += width;
    }
    return pos;
}

/** Build the frame into `frame` (room for GNSS_RTCM3_MAX_FRAME); returns its length, 0 if it does not fit. */
static inline size_t msm_build(uint8_t *frame, const msm_desc_t *sp)
{
    static uint8_t payload[GNSS_RTCM3_MAX_PAYLOAD];
    memset(payload, 0, sizeof(payload));
    unsigned msm = sp->type % 10u;
    bool ext = (msm == 5 || msm == 7);      /* extended satellite info and rough rate */
    bool hi = (msm == 6 || msm == 7);       /* high-resolution cell fields */

    msm_put(payload, 0, 12, sp->type);
    msm_put(payload, 12, 12, sp->station);
    msm_put(payload, 24, 30, sp->epoch);
    msm_put(payload, 54, 1, sp->more ? 1u : 0u);
    msm_put(payload, 55, 3, 3);              /* IODS; reserved, clock and smoothing fields stay 0 */
    for (unsigned i = 0; i < sp->nsats; i++) {
        msm_put(payload, 73u + sp->sats[i] - 1u, 1, 1);
    }
    for (unsigned j = 0; j < sp->nsigs; j++) {
        msm_put(payload, 137u + sp->sigs[j] - 1u, 1, 1);
    }
    size_t pos = GNSS_RTCM3_MSM_HEADER_BITS;
    unsigned cells = 0;
    for (unsigned x = 0; x < sp->nsats * sp->nsigs; x++) {
        bool present = !(sp->absent & ((uint64_t)1 << x));
        msm_put(payload, pos++, 1, present ? 1u : 0u);
        cells += present ? 1u : 0u;
    }

    /* Satellite data. */
    pos = msm_fields(payload, pos, 8, sp->nsats, NULL);            /* rough range, whole ms */
    if (ext) {
        pos = msm_fields(payload, pos, 4, sp->nsats, NULL);        /* extended satellite info */
    }
    pos = msm_fields(payload, pos, 10, sp->nsats, NULL);           /* rough range, 1/1024 ms */
    if (ext) {
        pos = msm_fields(payload, pos, 14, sp->nsats, NULL);       /* rough phase-range rate */
    }

    /* Signal data. */
    static const uint16_t k_zero[64] = { 0 };
    pos = msm_fields(payload, pos, hi ? 20u : 15u, cells, NULL);   /* fine pseudorange */
    pos = msm_fields(payload, pos, hi ? 24u : 22u, cells, NULL);   /* fine phase range */
    pos = msm_fields(payload, pos, hi ? 10u : 4u, cells, NULL);    /* lock time indicator */
    pos = msm_fields(payload, pos, 1, cells, NULL);                /* half-cycle ambiguity */
    pos = msm_fields(payload, pos, hi ? 10u : 6u, cells, sp->cnr ? sp->cnr : k_zero);   /* CNR */
    if (ext) {
        pos = msm_fields(payload, pos, 15, cells, NULL);           /* fine phase-range rate */
    }

    size_t n = (pos + 7u) / 8u;
    if (n > GNSS_RTCM3_MAX_PAYLOAD) {
        return 0;
    }
    return rtcm_make_frame(frame, payload, n);
}
