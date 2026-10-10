/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Links between diag's firmware files (diag.c, diag_log.c, diag_journal.c).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach the log rings, set up the console and install the log tap. */
esp_err_t diag_log_init(void);

/** Before a restart: give the console's TX buffer up to `ms` to drain. */
void diag_log_drain_console(uint32_t ms);

/** Mount the journal partition and start the writer task. */
esp_err_t diag_journal_init(void);

/** Before a restart: write what is queued into the sector being written (never erases: bounded). */
void diag_journal_flush(void);

/** UTC seconds now if a trusted clock was set (diag_set_utc), else 0. */
int64_t diag_utc_now(void);

#ifdef __cplusplus
}
#endif
