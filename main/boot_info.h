/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Boot banner: what is running, why we booted, how much memory we have.
 */
#pragma once

#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Human-readable name for an esp_reset_reason_t value (never NULL). */
const char *boot_reset_reason_str(esp_reset_reason_t reason);

/** Log app version/build, IDF version, reset reason, flash/PSRAM/heap sizes. */
void boot_info_log(void);

#ifdef __cplusplus
}
#endif
