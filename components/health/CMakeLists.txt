# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "health.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_timer
        board
        settings
        lifecycle
        ledger
        gnss_link
        gnss_obs
        rtcm_hub
        ntrip_server
        network
        station)
