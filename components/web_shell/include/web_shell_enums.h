/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network, reset and log enum strings for product-neutral web cores.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lifecycle.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *web_reboot_reason_name(lifecycle_reboot_reason_t v);

const char *web_probe_name(network_probe_t v);
/** Name of one network_ap_reason_t bit (NULL for an unknown bit). */
const char *web_ap_reason_name(uint32_t bit);
const char *web_ladder_why_name(uint8_t v);
/** Name of one network_ladder_trigger_t bit (NULL for an unknown bit). */
const char *web_ladder_trigger_name(uint32_t bit);
const char *web_reset_class_name(lifecycle_reset_class_t v);
const char *web_reset_reason_name(esp_reset_reason_t v);

/** "none", "error", "warn", "info", "debug", "verbose". */
bool web_log_level_parse(const char *s, esp_log_level_t *out);

#ifdef __cplusplus
}
#endif
