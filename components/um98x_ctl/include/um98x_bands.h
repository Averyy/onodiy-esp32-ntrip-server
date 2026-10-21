/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The signals a signal group configures, as the receiver reports them (pure
 * logic, no IDF includes, host-testable).
 *
 * The one-shot query ANT1GROUPA (main antenna input; ANT2GROUPA for the
 * second) is sent without a rate: with one ("ANT1GROUPA 1") it would start a
 * periodic log. It answers without any sky, right after the group is set.
 * The body of its log is
 *
 *   <group>,<n>,<sys>,<mask>,<sys>,<mask>...   e.g. 3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35
 *
 * with n constellation entries; <mask> is hexadecimal with "0x" (plain
 * digits read as decimal). Constellations GP (GPS), GL (GLONASS), GA
 * (Galileo), GB (BeiDou) and GQ (QZSS) are decoded; others are skipped.
 * Bit order of the masks (UM982 build 25164, our bench 2026-10-14; every
 * mask seen there fits the manual's signal-group table with it):
 *
 *   GPS      bit 0 L1C/A, 1 L1C, 2 L2C, 3 L2P, 4 L5
 *   GLONASS  bit 0 G1, 1 G2, 2 G3
 *   Galileo  bit 0 E1, 1 E5a, 2 E5b, 3 E6
 *   BeiDou   bit 0 B1I, 1 B2I, 2 B3I, 3 B1C, 4 B2a, 5 B2b
 *   QZSS     bit 0 L1C/A, 1 L1C, 2 L2C, 3 (never seen set), 4 L5, 5 L6
 *
 * Each signal falls in one frequency band bucket of gnss_rtcm3.h (L1, L2,
 * L5, L6), so the bands one constellation gets from a group can be counted
 * before any satellite is tracked: Galileo E5b, BeiDou B2I and B2b and
 * GLONASS G3 count as L2, Galileo E5a and BeiDou B2a as L5, BeiDou B3I as
 * L6.
 *
 * What a group should configure on the main antenna input (the manual's
 * table, confirmed on build 25164): 4 (factory) GPS L1C/A L2C L2P L5,
 * GLONASS G1 G2, Galileo E1 E5a E5b, BeiDou B1I B2I B3I, QZSS L1C/A L2C L5;
 * 3 the same with Galileo E6 (four Galileo bands), BeiDou B1I B3I B1C B2b
 * and QZSS L6; 7 the same as 4 with every BeiDou signal (B1C, B2a and B2b
 * added: four BeiDou bands); 5 no L5 at all (GPS L1C/A L2C L2P, Galileo E1
 * E5b, BeiDou B1I B2I B3I, QZSS L1C/A L2C). UM980 group 2 (UNTESTED: the
 * manual's table, read with the bit order above; no UM982 group is 2): GPS
 * L1C/A L1C L2C L2P L5, GLONASS G1 G2 G3, Galileo E1 E5a E5b E6, BeiDou
 * B1I B3I B1C B2a B2b, QZSS L1C/A L1C L2C L5. A report matches a group when it
 * names that group and lists at least those signals (a newer firmware may
 * add some). Other group numbers have no expectation: only the number is
 * compared.
 *
 * Status: the log's form is confirmed on build 25164: the station read it
 * in a real apply on 2026-10-14 (state OK, group 3, matches). The signal
 * tables per group are the bench's (no sky needed).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Constellation index of um98x_bands_t.mask (and bit of .present). */
typedef enum {
    UM98X_BSYS_GPS = 0,
    UM98X_BSYS_GLONASS,
    UM98X_BSYS_GALILEO,
    UM98X_BSYS_BEIDOU,
    UM98X_BSYS_QZSS,
} um98x_bsys_t;

_Static_assert(UM98X_BSYS_QZSS + 1 == UM98X_BANDS_SYS, "one mask per decoded constellation");

/**
 * Parse one log body (the text after the header, without the checksum).
 * @return true with out->state UM98X_BANDS_OK; false (out->state
 *         UM98X_BANDS_UNREAD, nothing else set) when it does not have the
 *         form above.
 */
bool um98x_bands_parse(const char *body, size_t len, um98x_bands_t *out);

/** API name of a constellation ("gps", "glonass", "galileo", "beidou", "qzss"), NULL out of range. */
const char *um98x_bands_sys_name(unsigned sys);

/** Name of one signal bit ("E6", "B2a", ...), NULL for a bit with no known signal. */
const char *um98x_bands_signal_name(unsigned sys, unsigned bit);

/** Frequency bands (L1, L2, L5, L6 buckets) the signals of `mask` cover. */
uint8_t um98x_bands_count(unsigned sys, uint32_t mask);

/**
 * The signals main-antenna group `master` should configure (see above).
 * @return false (out zeroed) for a group with no expectation.
 */
bool um98x_bands_expected(uint8_t master, uint32_t out[UM98X_BANDS_SYS]);

/**
 * Does a report match the profile's main-antenna group? Only an OK report
 * is judged. missing (optional) gets the expected signals the report lacks.
 * @return true when the report names `master` and lists every expected
 *         signal; false otherwise (also for a report that is not OK).
 */
bool um98x_bands_match(uint8_t master, const um98x_bands_t *b, uint32_t missing[UM98X_BANDS_SYS]);

#ifdef __cplusplus
}
#endif
