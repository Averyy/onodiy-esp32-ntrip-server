/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation decoders (pure logic, no IDF or RTOS includes, no state,
 * host-testable): carrier-to-noise values from MSM signal cells, and
 * satellite-in-view sentences.
 *
 * MSM signal cells (public RTCM 10403 layout, MSM4 to MSM7). After the
 * header (169 bits) and the cell mask (Nsat x Nsig bits) come the satellite
 * data and then the signal data. Both are sent field by field: the first
 * field for every satellite (or cell), then the next field for every
 * satellite, and so on. Bits per satellite / per cell and the cell fields in
 * sent order:
 *   MSM4  18 / 48  fine pseudorange 15, fine phase range 22, lock time 4,
 *                  half-cycle 1, CNR 6 (1 dB-Hz)
 *   MSM5  36 / 63  the same, then fine phase-range rate 15
 *   MSM6  18 / 65  fine pseudorange 20, fine phase range 24, lock time 10,
 *                  half-cycle 1, CNR 10 (1/16 dB-Hz)
 *   MSM7  36 / 80  the same, then fine phase-range rate 15
 * so the CNR of cell k starts at bit 169 + Nsat*Nsig + Nsat*S + Ncell*B +
 * k*W, with S the satellite bits, B the cell bits in front of the CNR (42
 * or 55) and W its width (6 or 10). A CNR of 0 means "not available". Cells
 * are numbered in sent order over the present cells only: satellite by
 * satellite (ascending satellite number), within one satellite signal by
 * signal (ascending signal ID). MSM1 to MSM3 carry no CNR.
 *
 * Satellite-in-view sentences (NMEA 0183 "$--GSV", checksum already
 * verified by the framer): total sentences, sentence number, satellites in
 * view, then up to four blocks of satellite number, elevation (deg),
 * azimuth (deg, true) and SNR (dB-Hz), each of the last three possibly
 * empty, and an optional signal ID (NMEA 4.10 and later) as the last field.
 * Talkers: GP (GPS, plus the SBAS, GLONASS, QZSS, BeiDou and Galileo number
 * ranges some receivers report under it), GN (by number range), GL
 * (GLONASS), GA (Galileo), GB or BD (BeiDou), GQ or QZ (QZSS), GI (NavIC).
 * Satellite numbers are converted to the numbering of the MSM satellite
 * mask (1..64): GPS, Galileo, BeiDou and NavIC by PRN, GLONASS by slot,
 * QZSS as PRN - 192, SBAS as PRN - 119. Blocks whose number cannot be
 * mapped are skipped. Out-of-range elevation, azimuth or SNR values count as
 * empty.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_rtcm3.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBS_GSV_MAX_SATS 4u   /**< satellite blocks in one sentence */

/** CNR of one MSM summed per band (cells whose CNR is available). */
typedef struct {
    uint32_t sum_q4[GNSS_BAND_COUNT];   /**< sum of the cells' CNR, 1/16 dB-Hz */
    uint16_t cells[GNSS_BAND_COUNT];    /**< cells counted in sum_q4 */
} obs_cnr_t;

/** One satellite of a satellite-in-view sentence. */
typedef struct {
    gnss_sys_t sys;
    uint8_t sat;              /**< MSM satellite number 1..64 (see above) */
    bool has_pos;             /**< elevation and azimuth both present */
    int8_t elevation_deg;     /**< -90..90 (valid with has_pos) */
    uint16_t azimuth_deg;     /**< 0..359 (valid with has_pos) */
    uint8_t cnr_dbhz;         /**< 0 = not tracked (empty SNR) */
} obs_gsv_sat_t;

/** One satellite-in-view sentence. */
typedef struct {
    uint8_t total;            /**< sentences in this set */
    uint8_t number;           /**< this sentence's number, 1..total */
    uint8_t in_view;          /**< satellites in view (whole set) */
    uint8_t signal_id;        /**< NMEA signal ID, 0 = none sent */
    uint8_t count;            /**< satellites decoded into sat[] */
    obs_gsv_sat_t sat[OBS_GSV_MAX_SATS];
} obs_gsv_t;

/**
 * Per-band CNR of one MSM frame whose header `h` was decoded from it
 * (gnss_rtcm3_msm_decode). Signals without a band bucket are left out.
 *
 * @param cell_q4  optional: receives every present cell's CNR in sent order
 *                 (h->num_cells entries, 1/16 dB-Hz, 0 = not available)
 * @return false (out cleared) for MSM1..MSM3, a frame shorter than its
 *         announced cells, or a header that does not belong to the frame
 */
bool obs_msm_cnr(const uint8_t *frame, size_t len, const gnss_rtcm3_msm_t *h, obs_cnr_t *out,
                 uint16_t *cell_q4);

/**
 * Parse one text line (without its terminator) as a satellite-in-view
 * sentence. false (out cleared) for any other line or a malformed one.
 */
bool obs_gsv_parse(const char *line, size_t len, obs_gsv_t *out);

#ifdef __cplusplus
}
#endif
