# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Public header depends only on gnss_types, so readers of station status do
# not pull in the GNSS link or the command layer. station_core.c and
# station_obs.c and sg_test_core.c are pure logic (no IDF includes) and are
# also compiled into the host test app; station.c is the task around them.
idf_component_register(
    SRCS "station.c" "station_core.c" "station_obs.c" "sg_test_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event gnss_types
    PRIV_REQUIRES
        board
        diag
        esp_hw_support
        esp_timer
        gnss_framing
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
