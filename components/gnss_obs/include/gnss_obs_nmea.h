/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * What the receiver's position (GGA) and error statistics (GST) sentences
 * say, field by field (NMEA 0183; the talkers gnss_obs reads, GP, GN, GL,
 * GA, GB/BD, GQ/QZ, GI; pure types, no IDF). Filled by
 * gnss_obs from checksum-valid lines (the framer verified the checksum);
 * read with gnss_obs_get_solution().
 *
 * Every field has its own validity: false when the field is empty, cannot
 * be read, or is outside what it can hold. An absent field is never
 * reported as zero, and a zero that was sent is reported as zero. Values are
 * as the receiver sent them, converted to fixed point and rounded half away
 * from zero (the UTC time is cut to the millisecond instead, so the same
 * epoch gives the same value in both sentences): nothing is derived from
 * another field, HDOP is never turned
 * into metres, and a standard deviation is the receiver's own estimate, not
 * a certified accuracy.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One GGA sentence (fields numbered after the address field). */
typedef struct {
    char talker[3];              /**< "GN", "GP", ... as sent */
    uint8_t quality;             /**< field 6 as sent (NMEA 0183: 0 no fix, 1 autonomous, 2 differential,
                                      4 RTK fixed, 5 RTK float, 6 estimated, ...) */
    uint8_t satellites;          /**< field 7: satellites used */
    bool utc_valid;
    uint32_t utc_ms;             /**< field 1: UTC time of the solution, ms since 00:00 (fraction cut to ms) */
    bool position_valid;         /**< fields 2-5; never with quality 0 (a no-fix sentence's position is not one) */
    int64_t lat_ndeg;            /**< latitude, 1e-9 degree, north positive */
    int64_t lon_ndeg;            /**< longitude, 1e-9 degree, east positive */
    bool hdop_valid;
    uint16_t hdop_c;             /**< field 8: horizontal dilution of precision, hundredths (no unit) */
    bool altitude_valid;         /**< fields 9-10 (unit M); never with quality 0 */
    int32_t altitude_01mm;       /**< antenna altitude above mean sea level, 0.1 mm */
    bool geoid_valid;            /**< fields 11-12 (unit M); never with quality 0 */
    int32_t geoid_01mm;          /**< geoidal separation (geoid above the WGS84 ellipsoid), 0.1 mm */
    bool diff_age_valid;         /**< field 13: the receiver's age of differential data; absent is not zero */
    uint32_t diff_age_ms;
    bool diff_station_valid;
    uint16_t diff_station;       /**< field 14: differential reference station ID, 0..4095 */
} gnss_obs_gga_t;

/** One GST sentence (the receiver's estimate of its position error). */
typedef struct {
    char talker[3];
    bool utc_valid;
    uint32_t utc_ms;             /**< field 1: UTC time of the solution it describes, ms since 00:00 */
    bool rms_valid;
    uint32_t rms_01mm;           /**< field 2: RMS of the standard deviation of the range inputs, 0.1 mm */
    bool major_valid;
    uint32_t major_01mm;         /**< field 3: standard deviation of the error ellipse's semi-major axis, 0.1 mm */
    bool minor_valid;
    uint32_t minor_01mm;         /**< field 4: the same for its semi-minor axis */
    bool orient_valid;
    uint16_t orient_cdeg;        /**< field 5: orientation of the semi-major axis, 0.01 degree from true north */
    bool lat_sd_valid;
    uint32_t lat_sd_01mm;        /**< field 6: standard deviation of the latitude error, 0.1 mm */
    bool lon_sd_valid;
    uint32_t lon_sd_01mm;        /**< field 7: the same for longitude */
    bool alt_sd_valid;
    uint32_t alt_sd_01mm;        /**< field 8: the same for altitude */
} gnss_obs_gst_t;

/**
 * The receiver's solution as its latest position sentence (GGA) and its
 * latest error-statistics sentence (GST) give it, each field with its own
 * validity (gnss_obs_nmea.h). The two are kept apart: each is the latest of
 * its kind, whatever its talker (GN after GP replaces it), and they need not
 * describe the same epoch; same_epoch says when they do. A product picks its
 * own freshness limits from the ages.
 *
 * Times: *_arrival_us is the esp_timer time at which the sentence reached
 * gnss_obs (fed from the link's RX task right after it was framed; the
 * UART interrupt time is not kept for lines), monotonic; the receiver's own
 * epoch is the sentence's UTC field. The GGA's age of differential data
 * (diff_age) is the receiver's; it is not the time since a correction
 * arrived anywhere else.
 */
typedef struct {
    bool gga_valid;               /**< a GGA arrived since boot or the last gnss_obs_reset() */
    int64_t gga_arrival_us;
    uint32_t gga_age_ms;          /**< since it arrived, UINT32_MAX = none */
    gnss_obs_gga_t gga;
    bool gst_valid;               /**< a GST arrived since boot or the last gnss_obs_reset() */
    int64_t gst_arrival_us;
    uint32_t gst_age_ms;
    gnss_obs_gst_t gst;
    bool same_epoch;              /**< both carry a UTC time and it is the same */
} gnss_obs_solution_t;

#ifdef __cplusplus
}
#endif
