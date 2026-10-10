# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Public header depends only on gnss_types, so readers of station status do
# not pull in the GNSS link or the command layer.
idf_component_register(
    SRCS "station.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event gnss_types
    PRIV_REQUIRES
        board
        esp_hw_support
        gnss_link
        gnss_obs
        rtcm_hub
        pkg_store
        um98x_ctl
        um98x_flash
        ntrip_server
        settings
        lifecycle
        ledger)
