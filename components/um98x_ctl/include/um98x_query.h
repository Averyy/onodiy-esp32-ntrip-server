/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The user's receiver queries ("Ask the receiver", pure logic, no IDF
 * includes, host-testable).
 *
 * A fixed list of read-only commands whose answers the command layer
 * already reads: the user picks one, the station sends it and shows the
 * receiver's reply lines as text. Nothing on the list changes the
 * receiver: no configuring command, no rate (the logs are one-shot), no
 * save. The configuration dump and the signal-group query need the
 * station's COMMAND lease (several KB of text, or a log the apply session
 * asks under its lease); the others go out without one while the receiver
 * streams, as the station's own queries do.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UM98X_QUERY_VERSION = 0,   /**< VERSIONA: model, firmware build, serial */
    UM98X_QUERY_CONFIG,        /**< CONFIG: the configuration dump (COMMAND lease) */
    UM98X_QUERY_MODE,          /**< MODE: the base mode and its parameters */
    UM98X_QUERY_GROUPS,        /**< ANT1GROUPA: the signals the running signal group configures (COMMAND lease) */
    UM98X_QUERY_BASEPOS,       /**< BASEPOSA: the base's position */
    UM98X_QUERY_SPPNAV,        /**< SPPNAVA: the single-point solution */
    UM98X_QUERY_COUNT,
} um98x_query_t;

/** The command text of query q, as sent (NULL when q is not on the list). */
const char *um98x_query_command(unsigned q);

/** True when query q needs the station's COMMAND lease (the casters pause for it). */
bool um98x_query_leased(unsigned q);

/**
 * The query whose command is cmd (case-insensitive, surrounding spaces
 * ignored), or -1 when cmd is not on the list.
 */
int um98x_query_find(const char *cmd);

#ifdef __cplusplus
}
#endif
