/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * UM98x command/config interface (Unicore N4 ASCII command set, public manual).
 *
 * Sends one command at a time over the GNSS link, matches the reply line(s),
 * and builds the station's base-station configuration from a profile:
 * signal group, COM1 baud, base mode (survey-in or fixed), RTCM3 message set
 * and rates on COM1, then one save to the receiver's non-volatile config.
 *
 * Replies arrive as text lines through um98x_ctl_on_line(), which the station
 * registers as the GNSS link's line sink. Only framer-validated lines count;
 * raw buffers are never searched for reply text.
 *
 * Watchdog: the blocking calls (detect, query_version, command,
 * apply_profile) run on the caller's task, the station supervisor, which is
 * on the task watchdog. Every wait blocks in slices of at most 1 s and feeds
 * the caller's task watchdog: a full baud scan takes about 44-50 s, one
 * command up to 15 s (SAVECONFIG 23 s) with its retries, and an apply with a
 * signal-group restart up to about 35 s more.
 *
 * Who may send what:
 *  - With a COMMAND lease (station supervisor only): anything.
 *  - With lease == NULL while streaming: exactly two read-only queries,
 *    VERSIONA (identity) and MODE (base mode and its parameters = status).
 *    There is no position query: the position comes from the 1005 stream.
 *    The configuration dump CONFIG needs a COMMAND lease (several KB of text
 *    between epochs). um98x_ctl_command() rejects everything else without a
 *    lease with ESP_ERR_INVALID_STATE, so this layer never reconfigures the
 *    receiver behind the supervisor's back. Raw writes through
 *    gnss_link_write(NULL, ...) bypass that check; that only the station
 *    configures the receiver is a convention for them (see gnss_link.h).
 *
 * Boot is stream-first: the station opens the link at the stored baud in
 * STREAM mode and publishes at once; it queries the version without a lease
 * and takes a COMMAND lease only if the version or applied profile does not
 * match. An ESP32-only reboot therefore costs no receiver session at all.
 *
 * Identity: VERSIONA, sent as "\r\nVERSIONA\r\n"; completed by a CRC-valid
 * version log (the acknowledgement alone does not complete it), parsed with
 * gnss_versiona_parse() (shared with the flasher). One try is one write plus
 * a 2000 ms window from the end of the write; up to 3 tries, 1000 ms apart.
 * After a known receiver restart: tries every 1000 ms with 1000 ms windows
 * for up to 20 s. Only model "UM982" (exact) gets configured or flashed.
 *
 * Base position: survey-in restarts on every reconfigure, receiver reset or
 * power cut and lands somewhere else each time (metre-level scatter), which
 * corrupts absolute positions for rovers on casters that trust 1005. After
 * the first good survey (or a longer station-side average) the station locks
 * the position (um98x_ctl_get_base_position -> settings gnss/base_pos) and
 * switches the profile to UM98X_BASE_FIXED. Survey-in stays the first-boot
 * default; the UI offers "re-survey". Fixed positions are sent to the
 * receiver as ECEF metres with 4 decimals (um98x_llh_to_ecef), which avoids
 * the height-reference ambiguity of latitude/longitude/height input; they are
 * stored as WGS84 latitude/longitude/ellipsoidal height.
 *
 * Status: PROVISIONAL, to be confirmed on hardware: baud-scan order, model
 * detection, reply/ack parsing, timeouts and the exact command sequence.
 * The profile structure and Onocoy defaults (um98x_profile.h) are ours.
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
 * Send one command and wait for its acknowledgement. One command is in
 * flight at a time; the next is sent 100 ms after the previous reply line.
 * Before the write, queued lines that match no pending request are
 * discarded (the UART driver and the framer are not touched).
 *
 * Ack timeout: 3000 ms (SAVECONFIG 5000 ms) from the end of the write. On a
 * timeout the command is resent up to 3 more times (at most 4 sends), each
 * 1000 ms after the previous timeout; on "PARSING FAILD" it is resent once
 * (2 sends), then the failure is final. A late ack for an earlier send of
 * the same command completes it. Queries with data (VERSIONA, MODE, CONFIG)
 * complete on their data line(s), not on the ack; VERSIONA follows its own
 * timing (see above). Values PROVISIONAL, to be confirmed on hardware.
 *
 * @param lease  COMMAND lease, or NULL for VERSIONA / MODE while streaming.
 * @param reply  optional buffer for the reply line(s); may be NULL.
 * @return ESP_OK on positive ack, ESP_FAIL on negative ack, ESP_ERR_TIMEOUT,
 *         ESP_ERR_INVALID_STATE for anything but VERSIONA or MODE without a
 *         lease.
 */
esp_err_t um98x_ctl_command(gnss_link_lease_t lease, const char *cmd, char *reply, size_t reply_len,
                            uint32_t timeout_ms);

/**
 * Apply a full profile and save it on the receiver. Needs a COMMAND lease.
 *
 * Order (PROVISIONAL, to be confirmed on hardware): "\r\n"; UNLOG COM1;
 * read back CONFIG and MODE; CONFIG SIGNALGROUP only when the read-back
 * differs, then wait for the receiver's restart (it saves itself) and read
 * back again; the COM1 baud change only when it differs; MODE BASE only when
 * the read-back differs or a lock/re-survey asked for it (fixed positions as
 * ECEF, 4 decimals); the antenna model if the profile has one; the RTCM
 * messages in profile order (a rejected 1230 is not fatal); the
 * satellite-view sentence if enabled; one SAVECONFIG. The caller then stores
 * gnss/applied_hash and gnss/applied_fw in one settings batch.
 */
esp_err_t um98x_ctl_apply_profile(gnss_link_lease_t lease, const um98x_base_profile_t *profile);

/**
 * Current base position: the antenna reference point of the latest CRC-valid
 * 1005 (from gnss_obs_get_arp), converted to WGS84 latitude/longitude/height.
 * Sends nothing to the receiver and needs no lease; the stream is the only
 * source of the position. ESP_ERR_NOT_FOUND before the first 1005.
 */
esp_err_t um98x_ctl_get_base_position(um98x_base_position_t *out);

#ifdef __cplusplus
}
#endif
