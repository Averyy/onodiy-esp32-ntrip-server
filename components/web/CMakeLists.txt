# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# web_json.c, web_core.c, web_pw.c, web_enums.c, web_build.c and web_parse.c
# are pure logic (no RTOS or driver calls) and are also compiled into the
# host test app; web.c, web_api.c, web_auth.c, web_ws.c and web_worker.c are
# the server around them.
#
# mbedtls provides psa/crypto.h (PBKDF2-HMAC-SHA256 for the admin password).
# um98x_ctl (profile codec, WGS84/ECEF) and um98x_flash (package facts) are
# lower layers the API needs directly.
idf_component_register(
    SRCS
        "web.c"
        "web_api.c"
        "web_auth.c"
        "web_ws.c"
        "web_worker.c"
        "web_json.c"
        "web_core.c"
        "web_pw.c"
        "web_enums.c"
        "web_build.c"
        "web_parse.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES
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
        station
        health
        fw_update)

# Embedded UI: every file directly in ui/ is gzip-compressed into a C table
# at build time (ui_pack.py; names and rules in API.md, "UI assets"). Adding
# or removing a file re-runs the configure step (CONFIGURE_DEPENDS).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB _ui_files CONFIGURE_DEPENDS LIST_DIRECTORIES false "${COMPONENT_DIR}/ui/*")
    set(_ui_c "${CMAKE_CURRENT_BINARY_DIR}/web_assets_gen.c")
    idf_build_get_property(_python PYTHON)
    add_custom_command(
        OUTPUT "${_ui_c}"
        COMMAND "${_python}" "${COMPONENT_DIR}/ui_pack.py" "${COMPONENT_DIR}/ui" "${_ui_c}"
        DEPENDS ${_ui_files} "${COMPONENT_DIR}/ui_pack.py"
        COMMENT "Packing the web UI assets"
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE "${_ui_c}")
endif()
