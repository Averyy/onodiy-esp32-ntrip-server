/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * UM98x command/config interface (Unicore N4 ASCII command set, public manual).
 *
 * Sends one command at a time over the GNSS link, matches the reply line(s),
 * and builds the station's base-station configuration from a profile:
 * signal group, base mode (survey-in or fixed), RTCM3 message set and rates on
 * COM1, then save to the receiver's non-volatile config.
 *
 * Replies arrive as text lines through um98x_ctl_on_line(), which the station
 * registers as the GNSS link's line sink.
 *
 * Who may send what:
 *  - With a COMMAND lease (station supervisor only): anything.
 *  - With lease == NULL while streaming: read-only queries only (version,
 *    position, status). um98x_ctl_command() rejects other commands without
 *    a lease with ESP_ERR_INVALID_STATE, so this layer never reconfigures
 *    the receiver behind the supervisor's back. Raw writes through
 *    gnss_link_write(NULL, ...) bypass that check; that only the station
 *    configures the receiver is a convention for them (see gnss_link.h).
 *
 * Boot is stream-first: the station opens the link at the stored baud in
 * STREAM mode and publishes at once; it queries the version without a lease
 * and takes a COMMAND lease only if the version or applied profile does not
 * match. An ESP32-only reboot therefore costs no receiver session at all.
 *
 * Base position: survey-in restarts on every reconfigure, receiver reset or
 * power cut and lands somewhere else each time (metre-level scatter), which
 * corrupts absolute positions for rovers on casters that trust 1005. After
 * the first good survey (or a longer station-side average) the station locks
 * the position (um98x_ctl_get_base_position -> settings gnss/base_pos) and
 * switches the profile to UM98X_BASE_FIXED. Survey-in stays the first-boot
 * default; the UI offers "re-survey".
 *
 * Status: PROVISIONAL. Baud-scan order, model detection, reply/ack parsing,
 * the read-only command list and the exact command sequence/timing are still
 * to be confirmed on hardware. The profile structure and Onocoy defaults
 * (um98x_profile.h) are ours and stable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"
#include "gnss_types.h"
#include "um98x_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare internal state (reply queue, command mutex). */
esp_err_t um98x_ctl_init(void);

/** Line sink for gnss_link (called from the GNSS RX task; copies and returns). */
void um98x_ctl_on_line(const char *line, size_t len, void *ctx);

/**
 * Find the receiver: try baud rates, identify the model, read version info.
 * Needs a COMMAND lease. On success the link is left at the working baud.
 */
esp_err_t um98x_ctl_detect(gnss_link_lease_t lease, um98x_info_t *out);

/** Read-only version query while streaming (no lease); used by the stream-first boot. */
esp_err_t um98x_ctl_query_version(um98x_info_t *out);

/** Last receiver info (from detect or a later query). */
esp_err_t um98x_ctl_get_info(um98x_info_t *out);

/**
 * Send one command and wait for its acknowledgement.
 *
 * @param lease  COMMAND lease, or NULL for a read-only query while streaming.
 * @param reply  optional buffer for the reply line(s); may be NULL.
 * @return ESP_OK on positive ack, ESP_FAIL on negative ack, ESP_ERR_TIMEOUT,
 *         ESP_ERR_INVALID_STATE for a configuring command without a lease.
 */
esp_err_t um98x_ctl_command(gnss_link_lease_t lease, const char *cmd, char *reply, size_t reply_len,
                            uint32_t timeout_ms);

/** Apply a full profile and save it on the receiver. Needs a COMMAND lease. */
esp_err_t um98x_ctl_apply_profile(gnss_link_lease_t lease, const um98x_base_profile_t *profile);

/**
 * Current base position as the receiver reports it (survey-in result or the
 * fixed position in use). Read-only query, no lease.
 */
esp_err_t um98x_ctl_get_base_position(um98x_base_position_t *out);

#ifdef __cplusplus
}
#endif
