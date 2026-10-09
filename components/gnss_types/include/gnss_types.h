/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Shared receiver value types (types only, no functions, no dependencies).
 *
 * These are passed between layers: the command layer fills them, the station
 * supervisor stores and reports them, and the web UI, health monitor and
 * update manager read them. Keeping them here lets those readers include
 * station.h without pulling in the GNSS link or the command layer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_MAX_RTCM_MSGS 16

/** What the receiver reports about itself. */
typedef struct {
    bool valid;
    char model[16];          /**< e.g. "UM982" */
    char firmware[48];       /**< firmware version string as reported */
    uint32_t build;          /**< numeric build, 0 if unknown */
    char serial[32];
} um98x_info_t;

typedef enum {
    UM98X_BASE_SURVEY_IN = 0,  /**< average own position for survey_in_s, then fix it */
    UM98X_BASE_FIXED,          /**< use the fixed coordinates in the profile */
} um98x_base_mode_t;

/** One RTCM3 output: message number and period in seconds. */
typedef struct {
    uint16_t msg;
    uint16_t period_s;
} um98x_rtcm_out_t;

/**
 * Base-station profile stored in settings and applied by the station.
 *
 * Persisted with um98x_profile_encode() (explicit little-endian layout with
 * its own version byte), never as a raw struct.
 */
typedef struct {
    uint8_t signal_group_master;     /**< e.g. 3 */
    uint8_t signal_group_slave;      /**< e.g. 6 (0 = slave antenna off) */
    um98x_base_mode_t base_mode;
    uint16_t survey_in_s;
    double fixed_lat_deg;
    double fixed_lon_deg;
    double fixed_height_m;           /**< ellipsoidal height */
    uint8_t rtcm_count;
    um98x_rtcm_out_t rtcm[UM98X_MAX_RTCM_MSGS];
    bool sat_view_output;            /**< also emit satellite-in-view sentences for the web UI */
} um98x_base_profile_t;

/** Where a base position came from. */
typedef enum {
    UM98X_POS_NONE = 0,
    UM98X_POS_SURVEY,        /**< receiver's own survey-in result */
    UM98X_POS_AVERAGED,      /**< long average computed by the station */
    UM98X_POS_USER,          /**< entered by the user */
} um98x_pos_source_t;

/** Antenna reference position (WGS84 ellipsoidal). */
typedef struct {
    um98x_pos_source_t source;
    double lat_deg;
    double lon_deg;
    double height_m;
    float sigma_m;           /**< 1-sigma horizontal estimate, 0 if unknown */
    int64_t locked_unix;     /**< when it was locked, 0 = not locked */
} um98x_base_position_t;

typedef enum {
    UM98X_FLASH_IDLE = 0,
    UM98X_FLASH_CHECKING,      /**< re-hashing/validating the staged package */
    UM98X_FLASH_ENTERING,      /**< getting the receiver into its loader */
    UM98X_FLASH_TRANSFERRING,  /**< package transfer in progress */
    UM98X_FLASH_FINISHING,     /**< completion check, receiver restart */
    UM98X_FLASH_DONE,
    UM98X_FLASH_FAILED,
} um98x_flash_phase_t;

/** Flash progress snapshot. */
typedef struct {
    um98x_flash_phase_t phase;
    uint32_t bytes_sent;
    uint32_t bytes_total;
    uint32_t retries;          /**< retransmissions so far */
    int32_t last_err;          /**< esp_err_t of the last failure, 0 = none */
} um98x_flash_progress_t;

#ifdef __cplusplus
}
#endif
