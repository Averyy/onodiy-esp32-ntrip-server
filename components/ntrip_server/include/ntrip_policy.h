/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Cross-slot caster rules (types only, no IDF includes): shared by the
 * public API (ntrip_server_set_config names the rule a save breaks) and the
 * pure protocol core that decides it, so there is one definition.
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

#ifdef __cplusplus
}
#endif
