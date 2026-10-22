/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Commands that older receiver builds refuse (pure logic, no IDF includes,
 * host-testable).
 *
 * The station sends such a command only to a build known to take it, so a
 * receiver never gets a command its firmware refuses, and the web UI names
 * the build that adds it (a receiver update fixes it). The builds come from
 * hardware: UM980 11833 refuses RTCM1230 and ANT1GROUPA, 23980 takes both
 * (the NTRIP-X, 2026-10-22). A build between the last one seen refusing and
 * the first one seen taking it counts as refusing. Every UM982 build seen
 * (18975, 23575, 25164) takes both, so the UM982 has no entry.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UM98X_CAP_RTCM1230 = 0,   /**< "RTCM1230 COMx <s>": the GLONASS code-phase biases */
    UM98X_CAP_GROUPS,         /**< the ANT1GROUPA log: the signals each group configures */
    UM98X_CAP_COUNT
} um98x_cap_t;

/** The first build of `model` known to take `cap`; 0 when every build does (or the model has no entry). */
uint32_t um98x_cap_min_build(const char *model, um98x_cap_t cap);

/**
 * A receiver `model` on `build` takes `cap`. An unknown build (0) is tried,
 * as before this table: its answer decides.
 */
bool um98x_cap_ok(const char *model, uint32_t build, um98x_cap_t cap);

/** The API name: "rtcm1230", "ant1groupa"; NULL out of range. */
const char *um98x_cap_name(um98x_cap_t cap);

#ifdef __cplusplus
}
#endif
