/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Forward one complete, already validated RTCM3 frame to the receiver
 * through the link while it streams (lease-free). Any task but the link's
 * RX task. *tx tells what reached the wire (gnss_link.h, "Writing").
 */
esp_err_t example_send_correction(const uint8_t *frame, size_t len, gnss_link_tx_t *tx);

#ifdef __cplusplus
}
#endif
