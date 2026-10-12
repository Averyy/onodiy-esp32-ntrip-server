/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTCM3 frame helpers (pure logic, no RTOS, host-testable).
 *
 * Decodes only public RTCM 10403.x structures:
 *  - CRC-24Q: polynomial 0x1864CFB, initial value 0, no reflection, no final
 *    XOR, over the preamble, the two header bytes and the payload; the
 *    three trailer bytes hold it most significant byte first, so a whole
 *    valid frame checks to 0;
 *  - the message number (first 12 payload bits) and the field after it,
 *    which is the reference station ID in station, antenna and MSM messages
 *    (ephemerides carry a satellite number there instead);
 *  - the MSM header: constellation, epoch time, multiple-message bit,
 *    satellite, signal and cell masks. The hub uses it for epoch
 *    boundaries, the observation summary for the band self-check;
 *  - the antenna reference point of 1005 and 1006.
 *
 * Every function takes a whole frame as the framer emits it (preamble,
 * header, payload, CRC) and checks the frame structure (preamble, length
 * field matching `len`) before reading any field. Only gnss_rtcm3_frame_ok()
 * also checks the reserved bits and the CRC; the others trust the caller.
 *
 * MSM types are 1071..1137 with a last digit of 1..7: GPS 107x, GLONASS
 * 108x, Galileo 109x, QZSS 111x, BeiDou 112x, NavIC 113x. The SBAS numbers
 * 1101..1107 are deliberately not treated as MSM: the receiver may reuse
 * some of them for its own non-standard messages, which must never take
 * part in epoch logic.
 *
 * Bands: every MSM signal ID maps to one frequency band bucket, used to
 * count "bands on one constellation":
 *   L1    GPS and QZSS L1 (incl. L1C), GLONASS G1, Galileo E1, BeiDou B1I and B1C
 *   L2    GPS and QZSS L2, GLONASS G2, Galileo E5b, BeiDou B2I and B2b
 *   L5    GPS and QZSS L5, Galileo E5a, BeiDou B2a, NavIC L5 SPS (signal 22)
 *   L6    Galileo E6, QZSS L6, BeiDou B3I
 *   E5AB  Galileo E5 AltBOC (counted as a fifth band)
 * The bucket mapping is PROVISIONAL, to be confirmed on hardware against
 * the caster's own band view. SBAS has no bands and is never listed: the
 * SBAS message numbers 1101..1107 are reused by the receiver for its own
 * messages, so the station drops them before the hub and the observation
 * view. This table (k_band_sigs in gnss_rtcm3.c) is the single source of the
 * band rule; the bench tools' table is checked against it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_RTCM3_MSM_HEADER_BITS 169u   /**< MSM header up to the cell mask */
#define GNSS_RTCM3_MSM_MAX_CELLS   64u    /**< satellites x signals in one MSM */

/** Constellations, in RTCM message-number order. */
typedef enum {
    GNSS_SYS_GPS = 0,
    GNSS_SYS_GLONASS,
    GNSS_SYS_GALILEO,
    GNSS_SYS_SBAS,
    GNSS_SYS_QZSS,
    GNSS_SYS_BEIDOU,
    GNSS_SYS_NAVIC,
    GNSS_SYS_COUNT,
} gnss_sys_t;

/** Frequency band buckets (see the mapping above). */
typedef enum {
    GNSS_BAND_L1 = 0,
    GNSS_BAND_L2,
    GNSS_BAND_L5,
    GNSS_BAND_L6,
    GNSS_BAND_E5AB,
    GNSS_BAND_COUNT,
    GNSS_BAND_NONE = 0xFF,   /**< signal ID not mapped */
} gnss_band_t;

/** Decoded MSM header. */
typedef struct {
    uint16_t msg_type;
    gnss_sys_t sys;
    uint8_t msm;               /**< MSM number 1..7 (last digit of the message number) */
    uint16_t station_id;       /**< reference station ID */
    /**
     * Epoch time, 30 bits as sent: ms of week (GPS, Galileo, QZSS, NavIC),
     * ms of BeiDou week (BDT), or day of week << 27 | ms of day (GLONASS).
     * Compare only within one constellation.
     */
    uint32_t epoch_time;
    bool multiple_message;     /**< 1: more MSMs follow for this epoch; 0: last (or only) one */
    uint8_t iods;              /**< issue of data station */
    uint64_t sat_mask;         /**< bit n-1 set = satellite n (1..64) present */
    uint32_t sig_mask;         /**< bit n-1 set = signal ID n (1..32) present */
    uint64_t cell_mask;        /**< bit k = cell k in sent order (satellite-major) */
    uint8_t num_sats;
    uint8_t num_sigs;
    uint8_t num_cells;         /**< cells marked present in cell_mask */
} gnss_rtcm3_msm_t;

/** Antenna reference point of a 1005 or 1006. */
typedef struct {
    uint16_t msg_type;         /**< 1005 or 1006 */
    uint16_t station_id;
    uint8_t itrf_year;         /**< ITRF realization year field */
    bool gps;                  /**< system indicators */
    bool glonass;
    bool galileo;
    bool ref_station;          /**< reference-station indicator (1 = non-physical station) */
    int64_t ecef_01mm[3];      /**< X, Y, Z in 0.1 mm */
    uint16_t height_01mm;      /**< antenna height above the marker in 0.1 mm (1006 only, else 0) */
} gnss_rtcm3_arp_t;

/** CRC-24Q of `len` bytes (24-bit result). */
uint32_t gnss_rtcm3_crc24q(const uint8_t *data, size_t len);

/**
 * Full check of one frame: preamble, zero reserved bits, payload of at
 * least 2 bytes, `len` equal to payload + 6, and the CRC.
 */
bool gnss_rtcm3_frame_ok(const uint8_t *frame, size_t len);

/** Message number of a frame; 0 if the frame structure is wrong or the payload is shorter than 2 bytes. */
uint16_t gnss_rtcm3_msg_type(const uint8_t *frame, size_t len);

/**
 * The 12 bits after the message number (reference station ID in station,
 * antenna and MSM messages); 0 if the payload is shorter than 3 bytes.
 */
uint16_t gnss_rtcm3_station_id(const uint8_t *frame, size_t len);

/**
 * Whether `msg_type` is an MSM (1101..1107 excluded, see above).
 * @param sys  constellation (may be NULL)
 * @param msm  MSM number 1..7 (may be NULL)
 */
bool gnss_rtcm3_msm_type(uint16_t msg_type, gnss_sys_t *sys, uint8_t *msm);

/**
 * Decode the MSM header of one frame. Fails (out cleared) when the frame is
 * not an MSM, is too short for its header and cell mask, or announces more
 * than 64 cells.
 */
bool gnss_rtcm3_msm_decode(const uint8_t *frame, size_t len, gnss_rtcm3_msm_t *out);

/**
 * Satellites of a decoded MSM header that have at least one present cell
 * (bit n-1 = satellite n); 0 for a header whose masks do not fit together.
 */
uint64_t gnss_rtcm3_msm_sats_with_cells(const gnss_rtcm3_msm_t *h);

/**
 * Signals of a decoded MSM header that have at least one present cell
 * (bit n-1 = signal ID n). An encoder may announce a signal in the signal
 * mask and send no cell for it (seen from a native receiver: BeiDou signal
 * 14 on every message); such a signal carries no observation. 0 for a
 * header whose masks do not fit together.
 */
uint32_t gnss_rtcm3_msm_sigs_with_cells(const gnss_rtcm3_msm_t *h);

/** Decode a 1005 or 1006. Fails (out cleared) for other types or a short payload. */
bool gnss_rtcm3_arp_decode(const uint8_t *frame, size_t len, gnss_rtcm3_arp_t *out);

/** Band bucket of one MSM signal ID (1..32); GNSS_BAND_NONE if not mapped. */
gnss_band_t gnss_rtcm3_msm_band(gnss_sys_t sys, unsigned sig_id);

/** Band buckets present in an MSM signal mask (bit n-1 = signal ID n), as a bit per gnss_band_t. */
uint8_t gnss_rtcm3_msm_band_mask(gnss_sys_t sys, uint32_t sig_mask);

/* ---- MSM epoch time on the GPS time scale ----------------------------------------- */

#define GNSS_RTCM3_WEEK_MS          604800000LL   /**< one week */
#define GNSS_RTCM3_DAY_MS           86400000LL    /**< one day */
#define GNSS_RTCM3_GPS_EPOCH_UNIX_MS 315964800000LL /**< 1980-01-06 00:00 UTC, GPS week 0 */
/** GPS time minus UTC (s) since 2017-01-01, no change announced: the same constant as the availability ledger's. */
#define GNSS_RTCM3_GPS_UTC_LEAP_S   18
#define GNSS_RTCM3_BDS_OFFSET_MS    14000         /**< BeiDou time = GPS time - 14 s */
#define GNSS_RTCM3_GLO_UTC_OFFSET_MS (3LL * 3600000LL) /**< GLONASS time = UTC + 3 h */

/**
 * The MSM epoch time field of constellation `sys` as an absolute GPS time
 * (ms since the GPS epoch, on the GPS scale), taking the week (or day) that
 * puts it nearest to `gps_ms_now`, a GPS time known to within a few days
 * (from a set clock: gps = unix_ms + GNSS_RTCM3_GPS_UTC_LEAP_S * 1000 -
 * GNSS_RTCM3_GPS_EPOCH_UNIX_MS).
 *  - GPS, Galileo, QZSS, NavIC: ms of the week, aligned with GPS time;
 *  - BeiDou: ms of the BeiDou week; BeiDou time + 14 s = GPS time;
 *  - GLONASS: day of week (bits 29..27, 0 = Sunday, 7 = unknown) and ms of
 *    the day in GLONASS time (UTC + 3 h), converted with the constant
 *    GPS-UTC offset above; an unknown day takes the nearest day.
 * @return false for SBAS or another value without MSM, a time-of-week or
 *         time-of-day field out of range, or a non-positive gps_ms_now.
 */
bool gnss_rtcm3_epoch_gps_ms(gnss_sys_t sys, uint32_t epoch_field, int64_t gps_ms_now, int64_t *out_gps_ms);

#ifdef __cplusplus
}
#endif
