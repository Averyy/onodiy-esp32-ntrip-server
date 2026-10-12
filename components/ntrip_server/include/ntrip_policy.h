/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Caster types shared by the public API and the pure protocol core (types
 * only, no IDF includes), so there is one definition: the cross-slot rules
 * (ntrip_server_set_config names the rule a save breaks), the RTK2go
 * mountpoint rules a save is checked against, and why a slot stopped.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Which cross-slot rule saving a slot config breaks. */
typedef enum {
    NTRIP_POLICY_OK = 0,
    NTRIP_POLICY_SECOND_ONOCOY,   /**< another enabled slot already streams to Onocoy */
    NTRIP_POLICY_SECOND_RTK2GO,   /**< another enabled slot already streams to RTK2go */
    NTRIP_POLICY_NEED_DUP_ACK,    /**< Onocoy plus an open caster (RTK2go, Centipede) needs the acknowledgement */
    NTRIP_POLICY_COUNT,
} ntrip_policy_t;

/**
 * RTK2go mountpoint rules for a new reservation, in the order they are
 * checked. RTK2go's reservation form refuses these; a slot stored before the
 * rules were checked keeps its name (they apply only when a slot is saved
 * with a new RTK2go host or a changed mountpoint).
 */
typedef enum {
    NTRIP_RTK2GO_OK = 0,
    NTRIP_RTK2GO_BAD_CHARS,          /**< anything but [A-Za-z0-9_-] (so no '.') */
    NTRIP_RTK2GO_FIRST_NOT_LETTER,   /**< the first character is not a letter */
    NTRIP_RTK2GO_FIRST_A,            /**< an uppercase 'A' first (the form names that letter only) */
    NTRIP_RTK2GO_TOO_SHORT,          /**< fewer than 4 characters */
    NTRIP_RTK2GO_TOO_LONG,           /**< more than 20 characters */
} ntrip_rtk2go_mount_t;

/**
 * Why a slot stopped and makes no attempt until the user saves it, presses
 * retry or disables it (the Onocoy login guard; see ntrip_server.h). Stored
 * with the slot's backoff record, so it survives restarts.
 */
typedef enum {
    NTRIP_STOP_NONE = 0,
    NTRIP_STOP_REJECTED,         /**< the caster refused the login three times */
    NTRIP_STOP_LOGIN_BLOCKED,    /**< "too many unsuccessful logins" three times */
    NTRIP_STOP_IN_USE,           /**< another stream uses the credential */
    NTRIP_STOP_KICKED,           /**< sessions kept being ended soon after login (another device on the credential) */
    NTRIP_STOP_DUPLICATE,        /**< the caster reports a duplicate of another stream */
    NTRIP_STOP_BANNED,           /**< the caster reports the credential or the connection as banned */
    NTRIP_STOP_COUNT,
} ntrip_stop_t;

#ifdef __cplusplus
}
#endif
