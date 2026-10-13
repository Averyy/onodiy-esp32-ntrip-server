/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM epoch assignment (pure logic, no IDF or RTOS includes, host-testable).
 *
 * Every frame the hub stores gets an epoch sequence number. A new epoch starts
 *  - at the very first frame and the first frame after
 *    rtcm_epoch_force_new() (framer reset, receiver reconfiguration),
 *  - otherwise at the first frame (any type) after an MSM whose
 *    multiple-message bit is 0 ("last MSM of this epoch"),
 *  - otherwise at an MSM whose epoch time differs from one already in the
 *    current epoch on the same time scale: an MSM of its own constellation,
 *    or, for GPS, Galileo, QZSS and NavIC (all on GPS time of week), an MSM
 *    of any of these four (a receiver that never clears the bit, a lost
 *    terminating MSM, or a terminating MSM that is not an MSM here, e.g. an
 *    SBAS 1107 last in its epoch as one real stream sends it: the next
 *    epoch's GPS MSM must not join the open one),
 *  - otherwise at a frame that is no MSM and arrives at least `gap_ms` after
 *    the previous frame while the current epoch holds an MSM not yet
 *    terminated (a lost terminating MSM: the next epoch's station messages;
 *    the next epoch's first MSM meets the time rule) or is unsure (below).
 *    The gap rule applies only between two frames whose arrival times are
 *    exact (`timed`: the UART interrupt's time of their last byte); a frame
 *    with only its read time never starts an epoch by gap, and neither does
 *    the frame after it.
 * The checks run in that order; the first that matches is counted. An MSM
 * after a gap continues its epoch, sure or unsure: an MSM of a later epoch on
 * a time scale the epoch holds met the time rule already, and receivers send
 * their constellations in a fixed order, so one the epoch has not seen yet is
 * its next MSM. Arrival times are the UART interrupt's, so a reader that
 * stalled (flash writes stop every task) no longer makes gaps inside an
 * epoch; the rule still holds for frames whose times are only read times
 * (then the gap rule is off anyway) and for receivers that pause between
 * constellations. An unsure epoch must not turn sure at a late MSM either:
 * the casters waiting for a sure epoch would start on a partial one. Non-MSM
 * frames after an epoch's terminating MSM belong to the next epoch, and a gap
 * after a terminated epoch starts nothing. GLONASS and BeiDou epoch times are
 * compared within their own constellation only (other time scales; one 2012
 * encoder even puts GPS time into its BeiDou MSMs), so no conversion is
 * needed. A constellation split over several MSMs with the same epoch time
 * stays in one epoch.
 *
 * Unsure epochs: an epoch started by the first frame, or by the first frame
 * after rtcm_epoch_force_new(), is "unsure". The bytes before that frame were
 * dropped (a framer reset lands anywhere in an epoch), so the frame may well
 * be the middle or the tail of an epoch, not its start. Every frame of an
 * unsure epoch is reported as such; consumers never start on one. The next
 * epoch started by the MMB, time or gap rule is sure again. While an epoch is
 * unsure, a frame that is no MSM a gap of `gap_ms` after the previous one
 * starts a sure epoch, so a stream without MSMs (nothing ever terminates an
 * epoch there) still gets a sure epoch after a reset; MSMs end an unsure
 * epoch by the MMB and time rules only.
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
    RTCM_EPOCH_START_GAP,     /**< a frame that is no MSM after a gap in an open MSM epoch or an unsure one */
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
    bool unsure;         /**< the current epoch was started by START_FIRST */
    uint32_t epoch;      /**< current epoch sequence number */
    uint32_t last_rx_ms; /**< arrival of the previous frame */
    bool last_timed;     /**< ... an exact one (the gap rule needs two) */
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
 * arriving at `now_ms`, and update the counters. `timed`: `now_ms` is the
 * exact arrival (the UART interrupt's time); false: a read time, which never
 * takes part in the gap rule.
 *
 * @param start optional: why the frame started an epoch (or CONTINUES).
 * @param unsure optional: the frame belongs to an unsure epoch (see the file
 *        comment), so it may not be the start of a whole epoch.
 * @return the frame's epoch sequence number.
 */
uint32_t rtcm_epoch_assign(rtcm_epoch_t *e, const uint8_t *frame, size_t len, uint16_t msg_type,
                           uint32_t now_ms, bool timed, rtcm_epoch_start_t *start, bool *unsure);

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
