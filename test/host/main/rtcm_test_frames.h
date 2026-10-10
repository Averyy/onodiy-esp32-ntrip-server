/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Synthetic RTCM3 frames for the hub tests. Payload: message number (12
 * bits), station ID 0, then for MSMs the epoch time (30 bits) and the
 * multiple-message bit at bits 24..54; bytes 7..10 carry a test serial and
 * the rest is a pattern derived from it, so a delivered frame can be checked
 * byte for byte. The three CRC bytes are a pattern too (the hub never checks
 * CRCs; the framer did that).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gnss_framing.h"

#define TF_MIN_PAYLOAD 11u

static inline uint8_t tf_fill(uint32_t serial, size_t i)
{
    return (uint8_t)(serial * 31u + i * 7u);
}

/** Build a frame with `payload_len` (>= TF_MIN_PAYLOAD) payload bytes. @return frame length. */
static inline size_t tf_build(uint8_t *f, uint16_t type, size_t payload_len, uint32_t tow, bool more,
                              uint32_t serial)
{
    if (payload_len < TF_MIN_PAYLOAD) {
        payload_len = TF_MIN_PAYLOAD;
    }
    if (payload_len > GNSS_RTCM3_MAX_PAYLOAD) {
        payload_len = GNSS_RTCM3_MAX_PAYLOAD;
    }
    uint8_t *p = f + GNSS_RTCM3_HEADER_LEN;
    f[0] = GNSS_RTCM3_PREAMBLE;
    f[1] = (uint8_t)((payload_len >> 8) & 0x03u);
    f[2] = (uint8_t)(payload_len & 0xFFu);
    p[0] = (uint8_t)(type >> 4);
    p[1] = (uint8_t)((type & 0x0Fu) << 4);
    p[2] = 0;
    p[3] = (uint8_t)(tow >> 22);
    p[4] = (uint8_t)(tow >> 14);
    p[5] = (uint8_t)(tow >> 6);
    p[6] = (uint8_t)(((tow & 0x3Fu) << 2) | (more ? 0x02u : 0x00u));
    p[7] = (uint8_t)(serial >> 24);
    p[8] = (uint8_t)(serial >> 16);
    p[9] = (uint8_t)(serial >> 8);
    p[10] = (uint8_t)serial;
    for (size_t i = TF_MIN_PAYLOAD; i < payload_len; i++) {
        p[i] = tf_fill(serial, i);
    }
    for (size_t i = 0; i < GNSS_RTCM3_CRC_LEN; i++) {
        p[payload_len + i] = tf_fill(serial, 2000u + i);
    }
    return payload_len + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN;
}

/**
 * Check one frame at the start of `buf` (`avail` bytes) built by tf_build.
 * @return its length, or 0 if it is not an intact test frame.
 */
static inline size_t tf_check(const uint8_t *buf, size_t avail, uint32_t *serial, uint16_t *type)
{
    if (avail < GNSS_RTCM3_HEADER_LEN + TF_MIN_PAYLOAD + GNSS_RTCM3_CRC_LEN || buf[0] != GNSS_RTCM3_PREAMBLE ||
        (buf[1] & 0xFCu) != 0) {
        return 0;
    }
    size_t n = ((size_t)(buf[1] & 0x03u) << 8) | buf[2];
    size_t len = n + GNSS_RTCM3_HEADER_LEN + GNSS_RTCM3_CRC_LEN;
    if (n < TF_MIN_PAYLOAD || len > avail) {
        return 0;
    }
    const uint8_t *p = buf + GNSS_RTCM3_HEADER_LEN;
    uint32_t s = ((uint32_t)p[7] << 24) | ((uint32_t)p[8] << 16) | ((uint32_t)p[9] << 8) | p[10];
    for (size_t i = TF_MIN_PAYLOAD; i < n; i++) {
        if (p[i] != tf_fill(s, i)) {
            return 0;
        }
    }
    for (size_t i = 0; i < GNSS_RTCM3_CRC_LEN; i++) {
        if (p[n + i] != tf_fill(s, 2000u + i)) {
            return 0;
        }
    }
    *serial = s;
    *type = (uint16_t)(((uint16_t)p[0] << 4) | (p[1] >> 4));
    return len;
}
