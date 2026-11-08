/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "fw_station_map.h"

bool fw_station_receiver_problem(station_state_t state)
{
    switch (state) {
    case STATION_STATE_DETECTING:
    case STATION_STATE_CONFIGURING:
    case STATION_STATE_RECOVERING:
    case STATION_STATE_NO_RECEIVER:
    case STATION_STATE_FLASHING:
    case STATION_STATE_FLASH_RECOVERY:
        return true;
    case STATION_STATE_IDLE:
    case STATION_STATE_STREAMING:
        break;
    }
    return false;
}

bool fw_station_receiver_ready(station_state_t state)
{
    return state == STATION_STATE_STREAMING;
}

bool fw_station_hub_fed(uint32_t last_frame_age_ms)
{
    return last_frame_age_ms <= FW_STATION_HUB_FED_MS;
}
