/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station's side of the updater: health for the app trial (network, RTCM
 * reaching the hub, casters streaming, receiver trouble), the receiver's
 * state and the receiver update jobs, all from the station's own services.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the station as fw_update's product, then fw_update_init(). */
esp_err_t fw_update_station_init(void);

#ifdef __cplusplus
}
#endif
