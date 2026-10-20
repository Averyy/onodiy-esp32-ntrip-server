# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "app_main.c" "boot_info.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_app_format
        app_update
        esp_psram
        spi_flash
        board
        settings
        diag
        lifecycle
        ledger
        health
        monitor
        gnss_link
        gnss_obs
        rtcm_hub
        um98x_ctl
        um98x_flash
        pkg_store
        ntrip_server
        local_caster
        network
        web
        fw_update
        station
        bench)
