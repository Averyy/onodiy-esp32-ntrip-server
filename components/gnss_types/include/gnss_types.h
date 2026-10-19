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

/** What the receiver reports about itself (the body fields of its version log). */
typedef struct {
    bool valid;
    char model[16];          /**< e.g. "UM982" */
    char firmware[48];       /**< firmware version string as reported, e.g. "R4.10Build18975" */
    uint32_t build;          /**< integer after "Build" in the firmware string, 0 if absent */
    char serial[32];         /**< serial number (the part after the first '-' of the PN-SN field) */
    char pn[24];             /**< part number (the part before it) */
    char efuse_id[24];       /**< efuse / board ID */
    char compile_date[12];   /**< "YYYY/MM/DD" */
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

/** Longest antenna descriptor or antenna serial number (characters, without the terminator). */
#define UM98X_ANTENNA_STR_MAX 31

/**
 * Base-station profile stored in settings and applied by the station.
 *
 * Persisted with um98x_profile_encode() (explicit little-endian layout with
 * its own version byte), never as a raw struct.
 *
 * Antenna: the descriptor the receiver publishes in its antenna message
 * (RTCM 1033), which casters show and rovers that apply antenna
 * corrections read. Empty antenna_model (the default) = leave the
 * receiver's own value alone, which is the honest value for an antenna
 * without a published calibration. Otherwise the IGS antenna name with its
 * radome (inner spaces kept: the name is padded to 16 characters before
 * the 4-character radome code). Printable ASCII without '"', at most
 * UM98X_ANTENNA_STR_MAX characters; trailing spaces are dropped when the
 * profile is stored. The serial number and setup ID only matter with a
 * model. UM982 build 25164 stores the name with runs of spaces collapsed
 * to one, so names are compared that way (um98x_antenna_str_same).
 */
typedef struct {
    uint8_t signal_group_master;     /**< e.g. 3 */
    uint8_t signal_group_slave;      /**< e.g. 6 (0 = slave antenna off) */
    um98x_base_mode_t base_mode;
    uint16_t survey_in_s;            /**< 1..3600 */
    double fixed_lat_deg;
    double fixed_lon_deg;
    double fixed_height_m;           /**< ellipsoidal height */
    uint16_t station_id;             /**< 0..4095, sent with every base-mode command (reference station ID in 1005 and MSM) */
    uint32_t com1_baud;              /**< receiver COM1 baud rate the profile sets (one the receiver supports) */
    uint8_t rtcm_count;
    um98x_rtcm_out_t rtcm[UM98X_MAX_RTCM_MSGS];
    bool sat_view_output;            /**< also emit satellite-in-view sentences permanently (saved in the receiver): the sky map */
    char antenna_model[UM98X_ANTENNA_STR_MAX + 1];   /**< IGS antenna name with radome; "" = leave the receiver's value */
    char antenna_serial[UM98X_ANTENNA_STR_MAX + 1];  /**< antenna serial number, may be empty */
    uint8_t antenna_setup_id;        /**< antenna setup ID 0..255 */
} um98x_base_profile_t;

/** A receiver on/off setting as read back from its configuration dump. */
typedef enum {
    UM98X_SETTING_UNKNOWN = 0,   /**< not in the dump, or a value that was not understood */
    UM98X_SETTING_ON,            /**< "ENABLE" */
    UM98X_SETTING_OFF,           /**< "DISABLE" */
} um98x_setting_t;

/**
 * Receiver settings read back from its configuration dump during an apply
 * session (for the UI; only the antenna descriptor and B1C/B2a decide
 * anything). PROVISIONAL, to be confirmed on hardware: the dump's item
 * names and value forms.
 */
typedef struct {
    uint8_t b1cb2a;                  /**< um98x_setting_t: BeiDou B1C/B2a observations in RTCM */
    uint8_t mmp;                     /**< um98x_setting_t: multipath mitigation (shown only, never sent) */
    bool mask_known;                 /**< an elevation mask was read */
    int16_t mask_ddeg;               /**< elevation mask in 0.1 degree (shown only, never sent) */
    bool antenna_known;              /**< the base antenna descriptor was read */
    char antenna_model[UM98X_ANTENNA_STR_MAX + 1];
    char antenna_serial[UM98X_ANTENNA_STR_MAX + 1];
    uint8_t antenna_setup_id;
} um98x_rx_settings_t;

/** Constellations a signal-group report decodes (um98x_bands.h: GPS, GLONASS, Galileo, BeiDou, QZSS). */
#define UM98X_BANDS_SYS 5

/** What the receiver said about the signals its running signal group configures. */
typedef enum {
    UM98X_BANDS_UNKNOWN = 0,   /**< not asked this boot, or no answer */
    UM98X_BANDS_OK,            /**< reported: group, present and mask are valid */
    UM98X_BANDS_REFUSED,       /**< the receiver refused the query (a firmware without it): unknown, never a fault */
    UM98X_BANDS_UNREAD,        /**< answered in a form this firmware does not read: unknown */
} um98x_bands_state_t;

/**
 * The signals the running signal group configures on the main antenna
 * input, as the receiver reported them after a configuration (no sky
 * needed). Bit order and the expectation per group: um98x_bands.h.
 */
typedef struct {
    uint8_t state;                   /**< um98x_bands_state_t */
    uint8_t group;                   /**< the group the receiver named (main antenna input) */
    uint8_t present;                 /**< bit per constellation: listed in the report */
    uint32_t mask[UM98X_BANDS_SYS];  /**< configured signals per constellation */
} um98x_bands_t;

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
    UM98X_FLASH_CHECKING,      /**< validating the package copy in RAM */
    UM98X_FLASH_ENTERING,      /**< getting the receiver into its loader */
    UM98X_FLASH_TRANSFERRING,  /**< package transfer in progress */
    UM98X_FLASH_COMMITTING,    /**< transfer ended: waiting for the receiver to write and report (send nothing) */
    UM98X_FLASH_FINISHING,     /**< leaving the loader, confirming the application, re-applying the profile */
    UM98X_FLASH_DONE,
    UM98X_FLASH_FAILED,
    UM98X_FLASH_NEEDS_USER,    /**< stopped; the receiver waits for the user's choice (journal stays open) */
} um98x_flash_phase_t;

/**
 * Why the last flash attempt failed (journal and progress). PROVISIONAL, to
 * be confirmed on hardware: the receiver's exact failure texts are not
 * captured yet.
 */
typedef enum {
    UM98X_FLASH_FAIL_NONE = 0,
    UM98X_FLASH_FAIL_TRANSPORT,        /**< timeout, retry limit, cancel or text during the transfer, handshake: retryable */
    UM98X_FLASH_FAIL_PACKAGE,          /**< the receiver rejected the package content: not retried with the same package */
    UM98X_FLASH_FAIL_UNCONFIRMED,      /**< loader prompt after the transfer without the success text */
    UM98X_FLASH_FAIL_NO_ANSWER,        /**< nothing classifiable after the transfer */
    UM98X_FLASH_FAIL_APP_NOT_STARTING, /**< the loader answers, the application does not start */
    UM98X_FLASH_FAIL_ENTRY,            /**< could not reach the loader (before the first data block) */
    UM98X_FLASH_FAIL_JOURNAL,          /**< journal unreadable or written by a newer schema */
    UM98X_FLASH_FAIL_RECEIVER,         /**< the receiver reported an error writing or loading (not a package content error): not retried automatically */
} um98x_flash_failure_t;

/** Flash progress snapshot. */
typedef struct {
    um98x_flash_phase_t phase;
    uint32_t bytes_sent;       /**< acknowledged bytes */
    uint32_t bytes_total;
    uint32_t retries;          /**< retransmissions so far */
    uint8_t transfers;         /**< transfers started under the open journal (counted across reboots; at most 3 without a new confirmation) */
    uint8_t failure_class;     /**< um98x_flash_failure_t of the last failure */
    int32_t last_err;          /**< esp_err_t of the last failure, 0 = none */
    uint32_t frames_acked;     /**< acknowledged frames of this transfer */
    uint32_t frames_total;     /**< frames of the package */
    uint32_t frames_sent;      /**< frames written, resends included */
    uint8_t entry_attempts;    /**< loader entry attempts in this session */
    uint8_t journal_state;     /**< um98x_journal_state_t written last (0 = none) */
} um98x_flash_progress_t;

/**
 * Receiver-flash journal states. Each state is persisted before the action
 * that starts it, so after a reset the receiver may be on either side of
 * that action. The main states are in session order (compare with < / >=);
 * FAILED and NEEDS_USER are side states, and `phase` keeps the last main
 * state while one of them is set.
 */
typedef enum {
    UM98X_JOURNAL_PREPARED = 1,    /**< journal opened, before the link is quiesced */
    UM98X_JOURNAL_BAUD_CHANGING,   /**< before the receiver's COM1 is moved to the loader baud */
    UM98X_JOURNAL_RESET_SENT,      /**< before the reset command (covers the stop key and the prompt) */
    UM98X_JOURNAL_MENU2_SENT,      /**< before the "download to flash" menu item */
    UM98X_JOURNAL_TRANSFERRING,    /**< before the first data block: the point of no return */
    UM98X_JOURNAL_EOT_SENT,        /**< before the first end-of-transfer byte */
    UM98X_JOURNAL_VERIFYING,       /**< before leaving the loader */
    UM98X_JOURNAL_FAILED,          /**< failure after the point of no return recorded, retry pending */
    UM98X_JOURNAL_NEEDS_USER,      /**< terminal until the user acts */
} um98x_journal_state_t;

/**
 * The open receiver-flash journal (settings gnss/flash_job). Persisted as a
 * versioned blob by the flasher's codec, never as a raw struct. A journal
 * exists only while a flash session is open.
 */
typedef struct {
    uint8_t version;               /**< blob layout version it was read from (1) */
    uint8_t state;                 /**< um98x_journal_state_t */
    uint8_t phase;                 /**< last main state (kept while FAILED / NEEDS_USER) */
    uint8_t sha256[32];            /**< SHA-256 of the pinned package */
    uint32_t size;                 /**< package size in bytes */
    char expected_fw[48];          /**< firmware string the update should produce */
    uint32_t expected_build;
    char pre_model[16];            /**< model from this session's live version query: the recovery model gate */
    char pre_fw[48];               /**< firmware running before the flash */
    uint32_t pre_build;
    uint32_t app_baud;             /**< receiver application baud detected at session start */
    uint32_t boot_baud;            /**< loader baud of the current entry attempt */
    uint8_t transfers;             /**< transfers started: at most 3 in total (the user's first plus up to 2 automatic retries); more need a user confirmation each */
    uint8_t entry_attempts;        /**< loader entry attempts in this session (at most 3, fallback-baud attempts included) */
    uint16_t last_acked_block;     /**< updated every 64 blocks */
    uint8_t failure_class;         /**< um98x_flash_failure_t of the last failure */
    int64_t changed_unix;          /**< last state change, 0 without a trusted clock */
} um98x_flash_journal_t;

#ifdef __cplusplus
}
#endif
