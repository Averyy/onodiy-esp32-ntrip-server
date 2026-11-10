# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Product-specific pure cores are also compiled into the host test app.
# web_shell holds the reusable JSON, connection, password, asset and helper
# cores and HTTP runtime; station routes, snapshots, events and job bodies stay here.
# um98x_ctl (profile codec, WGS84/ECEF) and um98x_flash (package facts) are
# lower layers the API needs directly.
idf_component_register(
    SRCS
        "web_station.c"
        "web_station_provider.c"
        "web_api.c"
        "web_station_jobs.c"
        "web_core.c"
        "web_enums.c"
        "web_build.c"
        "web_build_diag.c"
        "web_parse.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES
        web_shell
        esp_http_server
        esp_app_format
        app_update
        esp_partition
        esp_netif
        esp_timer
        esp_hw_support
        esp_psram
        spi_flash
        lwip
        mbedtls
        settings
        diag
        lifecycle
        ledger
        gnss_types
        gnss_framing
        gnss_obs
        gnss_link
        rtcm_hub
        pkg_store
        um98x_ctl
        um98x_flash
        network
        ntrip_server
        local_caster
        monitor
        onocoy_report
        station
        health
        fw_update
        board)

# Product-owned assets and metadata, generated from the explicit station composition.
idf_component_get_property(_web_shell_dir web_shell COMPONENT_DIR)
include("${_web_shell_dir}/cmake/web_ui.cmake")
web_shell_ui_bundle(
    TARGET ${COMPONENT_LIB}
    MANIFEST "${COMPONENT_DIR}/ui/product.json"
    PRODUCT_ROOT "${COMPONENT_DIR}/ui"
    ASSET_MANIFEST "${COMPONENT_DIR}/ui/assets.json"
    SYMBOL_PREFIX station_ui
    MAX_PROJECT_GZ 163840
    MAX_GZ 655360)
