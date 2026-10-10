/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "rtcm_epoch.h"

#include <string.h>

/* RTCM3 frame: 3 header bytes, payload, 3 CRC bytes. The MSM epoch time and
 * the multiple-message bit sit in payload bytes 3..6 (bits 24..54). */
#define FRAME_HDR        3u
#define FRAME_CRC        3u
#define MSM_MIN_PAYLOAD  7u

bool rtcm_msm_type(uint16_t msg_type)
{
    if (msg_type < 1071 || msg_type > 1137) {
        return false;
    }
    if (msg_type >= 1101 && msg_type <= 1107) {
        return false;
    }
    unsigned sub = msg_type % 10u;
    return sub >= 1 && sub <= 7;
}

bool rtcm_msm_head(const uint8_t *frame, size_t len, uint16_t msg_type, rtcm_msm_head_t *out)
{
    if (!rtcm_msm_type(msg_type) || frame == NULL || len < FRAME_HDR + MSM_MIN_PAYLOAD + FRAME_CRC) {
        return false;
    }
    const uint8_t *p = frame + FRAME_HDR;
    out->gnss = (uint8_t)((msg_type - 1070u) / 10u);
    out->time = ((uint32_t)p[3] << 22) | ((uint32_t)p[4] << 14) | ((uint32_t)p[5] << 6) | ((uint32_t)p[6] >> 2);
    out->more = ((p[6] >> 1) & 1u) != 0;
    return true;
}

void rtcm_epoch_init(rtcm_epoch_t *e, uint32_t gap_ms, uint32_t first_epoch)
{
    memset(e, 0, sizeof(*e));
    e->gap_ms = gap_ms;
    e->epoch = first_epoch - 1u;   /* the first frame increments it */
}

void rtcm_epoch_force_new(rtcm_epoch_t *e)
{
    e->force_new = true;
}

uint32_t rtcm_epoch_assign(rtcm_epoch_t *e, const uint8_t *frame, size_t len, uint16_t msg_type,
                           uint32_t now_ms, rtcm_epoch_start_t *start)
{
    rtcm_msm_head_t msm;
    bool is_msm = rtcm_msm_head(frame, len, msg_type, &msm);

    rtcm_epoch_start_t why = RTCM_EPOCH_CONTINUES;
    if (!e->started || e->force_new) {
        why = RTCM_EPOCH_START_FIRST;
    } else if (e->terminated) {
        why = RTCM_EPOCH_START_MMB;
    } else if (is_msm && (e->time_mask & (1u << msm.gnss)) && e->msm_time[msm.gnss] != msm.time) {
        why = RTCM_EPOCH_START_TIME;
    } else if (e->msm_open && (uint32_t)(now_ms - e->last_rx_ms) >= e->gap_ms) {
        why = RTCM_EPOCH_START_GAP;
    }

    if (why != RTCM_EPOCH_CONTINUES) {
        e->epoch++;
        e->started = true;
        e->force_new = false;
        e->terminated = false;
        e->msm_open = false;
        e->time_mask = 0;
        e->epochs++;
        if (why == RTCM_EPOCH_START_MMB) {
            e->by_mmb++;
        } else if (why == RTCM_EPOCH_START_TIME) {
            e->by_time++;
        } else if (why == RTCM_EPOCH_START_GAP) {
            e->by_gap++;
        }
    }

    if (is_msm) {
        e->time_mask |= (uint8_t)(1u << msm.gnss);
        e->msm_time[msm.gnss] = msm.time;
        if (msm.more) {
            e->msm_open = true;
        } else {
            e->terminated = true;
            e->msm_open = false;
        }
    }
    e->last_rx_ms = now_ms;
    if (start != NULL) {
        *start = why;
    }
    return e->epoch;
}
