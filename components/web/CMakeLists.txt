# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# mbedtls provides psa/crypto.h (PBKDF2-HMAC-SHA256 for the admin password).
idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
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
