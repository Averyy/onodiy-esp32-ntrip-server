# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# semver.c is pure logic (no IDF includes) and is also compiled into the host
# test app. mbedtls provides psa/crypto.h for streaming SHA-256.
idf_component_register(
    SRCS "fw_update.c" "semver.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES
        app_update
        esp_app_format
        esp_http_client
        esp-tls
        mbedtls
        pkg_store
        settings
        lifecycle
        ledger
        station
        network
        ntrip_server
        rtcm_hub)
