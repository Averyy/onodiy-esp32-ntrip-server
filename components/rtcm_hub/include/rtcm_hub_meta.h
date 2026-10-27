/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

/** Arrival range of frames actually copied by one hub read. Stamps are
 * monotonic milliseconds, not GNSS observation epochs. timed_frames counts
 * reconstructable stamps; exact_frames is the subset measured by the UART
 * interrupt (still rounded to milliseconds). Zero timed_frames means unknown.
 */
typedef struct {
    uint64_t oldest_rx_ms;
    uint64_t newest_rx_ms;
    uint32_t frames;
    uint32_t timed_frames;
    uint32_t exact_frames;
} rtcm_hub_arrivals_t;

typedef struct {
    rtcm_hub_arrivals_t live;         /**< Frames copied from the ring. */
    rtcm_hub_arrivals_t observations; /**< Subset of live: MSMs and legacy observations. */
    rtcm_hub_arrivals_t sticky;       /**< Cached station metadata replayed before live data. */
} rtcm_hub_read_meta_t;
