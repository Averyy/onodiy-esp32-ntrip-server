/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Published RTCM3 example frames shared by the host tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 1005, reference station 2003, ITRF year 0, GPS indicator 1, ECEF
 * X = 1114104.5999 m, Y = -4850729.7108 m, Z = 3975521.4643 m. CRC 0x360B98.
 * Payload byte 0xD3 at frame offset 5.
 */
extern const uint8_t k_v1005[25];

/*
 * 1074 GPS MSM4, station 0, epoch 270524000 ms, multiple-message bit 1,
 * IODS 0, satellites 10 14 16 25 26 29 31 32, signals 2 and 10, 16 cells,
 * all present. CRC 0x71CE42. Payload bytes 0xD3 at frame offsets 113 and 136.
 */
extern const uint8_t k_v1074[144];

/** Wrap `payload` (n >= 2) in a valid frame; returns the frame length (n + 6). */
size_t rtcm_make_frame(uint8_t *out, const uint8_t *payload, size_t n);
