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

#if EXAMPLE_UPDATER
#include "um98x_flash.h"

/**
 * A receiver update with a package already staged (pkg_store) that the user
 * confirmed (size, SHA-256, the firmware it should produce): the receiver
 * identified, the package checked against it, pinned, sent under one RAW
 * lease. *res gets the outcome; with UM98X_FLASH_END_DONE or _ABORTED its
 * receiver field is the application's own version log after the session,
 * for the product's profile check before anything else is sent.
 */
esp_err_t example_update_receiver(const um98x_flash_expect_t *confirmed, um98x_flash_result_t *res);
#endif

#ifdef __cplusplus
}
#endif
