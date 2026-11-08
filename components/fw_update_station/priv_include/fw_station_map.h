/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station's states as the updater reads them (pure logic, host-tested).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "station.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RTCM older than this no longer counts as reaching the hub (ms). */
#define FW_STATION_HUB_FED_MS 5000u

/** A state that means "no RTCM because of the receiver": excuses the hub and the casters in an app trial. */
bool fw_station_receiver_problem(station_state_t state);

/** A receiver update may start in this state. */
bool fw_station_receiver_ready(station_state_t state);

/** The hub counts as fed with its latest frame this old. */
bool fw_station_hub_fed(uint32_t last_frame_age_ms);

#ifdef __cplusplus
}
#endif
