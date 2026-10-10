# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# mbedtls provides psa/crypto.h (PBKDF2-HMAC-SHA256 for the admin password).
# web_assets.c is pure logic (no IDF includes) and is also compiled into the
# host test app.
idf_component_register(
    SRCS "web.c" "web_assets.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES
        esp_http_server
        esp_app_format
        mbedtls
        settings
        diag
        lifecycle
        ledger
        gnss_obs
        gnss_link
        rtcm_hub
        pkg_store
        network
        ntrip_server
        station
        health
        fw_update)

# Browser UI (ui/): tools/ui_bundle.py checks the page, the scripts and the
# styles (structure, and what the Content-Security-Policy would block), joins
# ui/js/*.js into app.js, gzips every asset and writes the asset table
# (priv_include/web_assets.h) as a C file into the build directory. Any check
# failing, or the compressed UI growing past UI_GZ_BUDGET bytes, fails the
# build. Adding a file under ui/js/ reconfigures (CONFIGURE_DEPENDS).
set(UI_GZ_BUDGET 153600)
file(GLOB _ui_js CONFIGURE_DEPENDS "${COMPONENT_DIR}/ui/js/*.js")
set(_ui_inputs
    "${COMPONENT_DIR}/ui/index.html"
    "${COMPONENT_DIR}/ui/app.css"
    "${COMPONENT_DIR}/ui/favicon.svg"
    ${_ui_js})
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
