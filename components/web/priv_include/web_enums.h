/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * API strings of the component enums (the API contract's appendix), and the
 * reverse lookups the request parsers need. Pure logic over the component
 * headers' types (no RTOS or driver calls), host-testable.
 *
 * A value outside a table is written as "unknown"; the UI shows enum strings
 * it does not know verbatim.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "fw_update.h"
#include "gnss_obs.h"
#include "gnss_types.h"
#include "health.h"
#include "ledger.h"
#include "lifecycle.h"
#include "network.h"
#include "ntrip_server.h"
#include "station.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *web_station_state_name(station_state_t v);
/** Name of one station_alert_t bit (NULL for an unknown bit). */
const char *web_station_alert_name(uint32_t bit);
const char *web_health_level_name(health_level_t v);
const char *web_health_step_name(health_step_t v);
/** Name of one health_alert_t bit (NULL for an unknown bit). */
const char *web_health_alert_name(uint32_t bit);
const char *web_ntrip_state_name(ntrip_state_t v);
const char *web_ntrip_fail_name(ntrip_fail_class_t v);
const char *web_ntrip_trust_name(ntrip_trust_t v);
const char *web_ntrip_backoff_name(ntrip_backoff_profile_t v);
const char *web_base_mode_name(um98x_base_mode_t v);
const char *web_pos_source_name(um98x_pos_source_t v);
const char *web_flash_phase_name(um98x_flash_phase_t v);
/** um98x_journal_state_t (1-based C values). */
const char *web_journal_state_name(uint8_t v);
const char *web_flash_failure_name(uint8_t v);
/** station_flash_outcome_t name, and the progress `result` code it ends a job with. */
const char *web_flash_outcome_name(uint8_t v);
const char *web_flash_outcome_result(uint8_t v);
const char *web_fw_state_name(fw_update_state_t v);
const char *web_fw_trial_name(fw_update_trial_t v);
const char *web_gnss_block_name(fw_update_gnss_block_t v);
const char *web_probe_name(network_probe_t v);
/** Name of one network_ap_reason_t bit (NULL for an unknown bit). */
const char *web_ap_reason_name(uint32_t bit);
const char *web_ladder_why_name(uint8_t v);
/** Name of one network_ladder_trigger_t bit (NULL for an unknown bit). */
const char *web_ladder_trigger_name(uint32_t bit);
const char *web_link_mode_name(int v);
const char *web_reset_class_name(lifecycle_reset_class_t v);
const char *web_reboot_reason_name(lifecycle_reboot_reason_t v);
const char *web_reset_reason_name(esp_reset_reason_t v);
const char *web_clock_name(ledger_clock_t v);
const char *web_sys_name(gnss_sys_t v);
const char *web_band_name(gnss_band_t v);
/** diag_event_t (1-based C values). */
const char *web_diag_event_name(uint16_t kind);
const char *web_update_slot_name(lifecycle_update_t v);
/** health_recovery_mode_t ("full", "no_reboot", "alerts_only"). */
const char *web_recovery_mode_name(health_recovery_mode_t v);
/** um98x_setting_t of a receiver read-back ("unknown", "on", "off"). */
const char *web_setting_name(uint8_t v);
/** station_live_sky_t, station_live_why_t, station_live_end_t. */
const char *web_live_sky_name(uint8_t v);
const char *web_live_why_name(uint8_t v);
const char *web_live_end_name(uint8_t v);

/* ---- reverse lookups (request parsing) ---------------------------------------- */

bool web_base_mode_parse(const char *s, um98x_base_mode_t *out);
bool web_ntrip_trust_parse(const char *s, ntrip_trust_t *out);
bool web_ntrip_backoff_parse(const char *s, ntrip_backoff_profile_t *out);
/** "none", "error", "warn", "info", "debug", "verbose". */
bool web_log_level_parse(const char *s, esp_log_level_t *out);
bool web_recovery_mode_parse(const char *s, health_recovery_mode_t *out);

#ifdef __cplusplus
}
#endif
