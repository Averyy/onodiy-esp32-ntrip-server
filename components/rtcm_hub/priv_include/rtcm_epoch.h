/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM epoch assignment (pure logic, no IDF or RTOS includes, host-testable).
 *
 * Every frame the hub stores gets an epoch sequence number. A new epoch starts
 *  - at the first frame (any type) after an MSM whose multiple-message bit is
 *    0 ("last MSM of this epoch"),
 *  - otherwise at an MSM whose constellation already has an MSM with a
 *    different epoch time in the current epoch (a receiver that never clears
 *    the bit),
 *  - otherwise at any frame that arrives at least `gap_ms` after the previous
 *    frame while the current epoch holds an MSM not yet terminated (a lost
 *    terminating MSM),
 *  - and at the very first frame and the first frame after
 *    rtcm_epoch_force_new() (framer reset, receiver reconfiguration).
 * The checks run in that order; the first that matches is counted. Non-MSM
 * frames after an epoch's terminating MSM belong to the next epoch, and a gap
 * after a terminated epoch starts nothing. Epoch times are compared within one
 * constellation only (GLONASS and BeiDou run on other time scales), so no
 * conversion is needed. A constellation split over several MSMs with the same
 * epoch time stays in one epoch.
 *
 * MSM detection follows the public RTCM numbering: 1071..1137 with a last
 * digit of 1..7, except 1101..1107 (SBAS numbers the receiver may reuse for
 * its own messages; the station drops them before the hub anyway). A frame too
 * short to carry the epoch time and the multiple-message bit is not an MSM
 * here.
 *
 * Times are wrapping u32 milliseconds compared only as (uint32_t)(now - then).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Constellation slots: message number tens digit 107x..113x (110x unused). */
#define RTCM_EPOCH_GNSS_SLOTS 7u

/** Why a frame started an epoch. */
typedef enum {
    RTCM_EPOCH_CONTINUES = 0, /**< same epoch as the previous frame */
    RTCM_EPOCH_START_FIRST,   /**< first frame, or first after a forced start */
    RTCM_EPOCH_START_MMB,     /**< the previous epoch's last MSM said so */
    RTCM_EPOCH_START_TIME,    /**< epoch-time change within one constellation */
    RTCM_EPOCH_START_GAP,     /**< gap while an MSM epoch was open */
} rtcm_epoch_start_t;

/** Decoded MSM header fields the epoch rules need. */
typedef struct {
    uint8_t gnss;       /**< constellation slot, < RTCM_EPOCH_GNSS_SLOTS */
    uint32_t time;      /**< 30-bit epoch time as sent (system-specific scale) */
    bool more;          /**< multiple-message bit: more MSMs follow for this epoch */
} rtcm_msm_head_t;

/** Epoch tracker state. Treat as opaque; public so the hub can embed it. */
typedef struct {
    uint32_t gap_ms;
    bool started;        /**< an epoch exists */
    bool force_new;      /**< the next frame starts an epoch */
    bool terminated;     /**< the current epoch's last MSM had the bit cleared */
    bool msm_open;       /**< the current epoch holds an MSM not yet terminated */
    uint32_t epoch;      /**< current epoch sequence number */
    uint32_t last_rx_ms; /**< arrival of the previous frame */
    uint8_t time_mask;   /**< constellations with an MSM in the current epoch */
    uint32_t msm_time[RTCM_EPOCH_GNSS_SLOTS];
    uint64_t epochs;     /**< epochs started (every reason) */
    uint64_t by_mmb;
    uint64_t by_time;
    uint64_t by_gap;
} rtcm_epoch_t;

/**
 * Start with no epoch; the first frame starts epoch `first_epoch` (1 in the
 * hub; tests start near UINT32_MAX to cover the wrap).
 */
void rtcm_epoch_init(rtcm_epoch_t *e, uint32_t gap_ms, uint32_t first_epoch);

/**
 * Assign the epoch for one frame (whole RTCM3 frame incl. header and CRC)
 * arriving at `now_ms`, and update the counters.
 *
 * @param start optional: why the frame started an epoch (or CONTINUES).
 * @return the frame's epoch sequence number.
 */
uint32_t rtcm_epoch_assign(rtcm_epoch_t *e, const uint8_t *frame, size_t len, uint16_t msg_type,
                           uint32_t now_ms, rtcm_epoch_start_t *start);

/** Make the next frame start a new epoch (counted in `epochs` only). */
void rtcm_epoch_force_new(rtcm_epoch_t *e);

/** True if `msg_type` is an MSM for the epoch rules (see the file comment). */
bool rtcm_msm_type(uint16_t msg_type);

/** Decode the MSM fields of a whole frame. @return false if it is not an MSM here. */
bool rtcm_msm_head(const uint8_t *frame, size_t len, uint16_t msg_type, rtcm_msm_head_t *out);

/** Wrap-safe "a is a later epoch than b". */
static inline bool rtcm_epoch_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

#ifdef __cplusplus
}
#endif
