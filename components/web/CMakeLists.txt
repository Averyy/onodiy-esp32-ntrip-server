# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# web_json.c, web_core.c, web_conn.c, web_pw.c, web_enums.c, web_build.c,
# web_parse.c and web_assets.c are pure logic (no RTOS or driver calls) and
# are also compiled into the host test app; web.c, web_api.c, web_auth.c,
# web_ws.c and web_worker.c are the server around them.
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
        "web_conn.c"
        "web_pw.c"
        "web_enums.c"
        "web_build.c"
        "web_parse.c"
        "web_assets.c"
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
        local_caster
        monitor
        onocoy_report
        station
        health
        fw_update
        board)

# Browser UI (ui/): tools/ui_bundle.py checks the page, the scripts and the
# styles (structure, and what the Content-Security-Policy would block), joins
# ui/js/*.js into app.js without their comments, gzips every asset and
# writes the asset table (priv_include/web_assets.h) as a C file into the
# build directory. Any check failing, or the compressed UI growing past
# UI_GZ_BUDGET bytes, fails the build. Adding a file under ui/js/
# reconfigures (CONFIGURE_DEPENDS).
set(UI_GZ_BUDGET 655360)
file(GLOB _ui_js CONFIGURE_DEPENDS "${COMPONENT_DIR}/ui/js/*.js")
file(GLOB _ui_vendor CONFIGURE_DEPENDS "${COMPONENT_DIR}/ui/vendor/*")
set(_ui_inputs
    "${COMPONENT_DIR}/ui/index.html"
    "${COMPONENT_DIR}/ui/app.css"
    "${COMPONENT_DIR}/ui/favicon.svg"
    ${_ui_js} ${_ui_vendor})
set(_ui_c "${CMAKE_CURRENT_BINARY_DIR}/web_ui_assets.c")
idf_build_get_property(_python PYTHON)
add_custom_command(
    OUTPUT "${_ui_c}"
    COMMAND "${_python}" "${COMPONENT_DIR}/tools/ui_bundle.py"
            --ui "${COMPONENT_DIR}/ui" --c-out "${_ui_c}" --max-gz ${UI_GZ_BUDGET}
    DEPENDS "${COMPONENT_DIR}/tools/ui_bundle.py" ${_ui_inputs}
    COMMENT "Checking and embedding the web UI"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE "${_ui_c}")
