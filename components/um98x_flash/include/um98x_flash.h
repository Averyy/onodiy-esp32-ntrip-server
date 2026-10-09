/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * UM98x firmware flasher: writes the package staged in pkg_store to the
 * receiver over the GNSS link (XMODEM-1K over UART, public protocol).
 *
 * Runs synchronously in the caller's task (the station supervisor) while the
 * caller holds a RAW lease on the GNSS link. Reports progress through a
 * callback so the web UI can show it. The package's target model must match
 * the connected receiver before a single byte is sent; the payload is
 * re-hashed (pkg_store_verify) right before the transfer.
 *
 * Interrupting a transfer can leave the receiver without valid firmware, so:
 *  - um98x_flash_cancel() is honoured only before the transfer starts;
 *  - the station opens a persisted flash journal and pins the staged package
 *    before ENTERING, and holds a reboot inhibit for the whole run;
 *  - um98x_flash_run() must be safe to call again after a failed run or an
 *    interrupted one (reboot, power cut): the station's recovery path calls
 *    it with recovery = true, without the model check against a live
 *    receiver (the persisted model is used) and without the downtime gate.
 *
 * The XMODEM-1K sender is a pure core behind read/write callbacks with an
 * injected clock, host-tested against a simulated receiver.
 *
 * Status: PROVISIONAL. Loader entry, baud switching, framing details, retry
 * and timeout values, completion check and recovery are still to be designed
 * and must be bench-tested before field use.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"
#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Progress callback, called from the flashing task; keep it short. */
typedef void (*um98x_flash_progress_cb_t)(const um98x_flash_progress_t *p, void *ctx);

/** Parameters for one flash run. */
typedef struct {
    const char *expected_model;   /**< detected model, or the persisted one when the receiver does not answer */
    bool recovery;                /**< receiver left mid-update: skip live detection, go straight to the loader */
    um98x_flash_progress_cb_t progress_cb;  /**< optional */
    void *progress_ctx;
} um98x_flash_request_t;

/** Prepare internal state. */
esp_err_t um98x_flash_init(void);

/**
 * Check the staged package without touching the receiver: present, complete,
 * SHA-256 still matches (re-hashed), and built for `expected_model`.
 *
 * @return ESP_ERR_NOT_FOUND (nothing staged), ESP_ERR_INVALID_CRC (payload
 *         changed since download), ESP_ERR_INVALID_VERSION (wrong model),
 *         ESP_ERR_INVALID_RESPONSE (not a recognisable package).
 */
esp_err_t um98x_flash_check_package(const char *expected_model);

/**
 * Flash the staged package. Blocks until done or failed (minutes). Feeds the
 * task watchdog of the calling task between blocks.
 *
 * @param raw_lease RAW lease on the GNSS link, held by the caller throughout.
 */
esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req);

/** Request cancellation; only effective before the transfer phase. */
void um98x_flash_cancel(void);

/** Latest progress (safe from any task). */
void um98x_flash_get_progress(um98x_flash_progress_t *out);

#ifdef __cplusplus
}
#endif
