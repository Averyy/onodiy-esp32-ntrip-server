# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# health_ladder.c (GNSS recovery ladder) and health_mon.c (loss journal,
# link-loss alert, caster budgets, button, level) are pure logic (no IDF
# includes) and are also compiled into the host test app; health.c is the
# task around them.
idf_component_register(
    SRCS "health.c" "health_ladder.c" "health_mon.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES
        esp_timer
        esp_event
        esp_driver_gpio
        board
        diag
        settings
        lifecycle
        ledger
        leds
        gnss_link
        gnss_obs
        rtcm_hub
        ntrip_server
        network
        station)
