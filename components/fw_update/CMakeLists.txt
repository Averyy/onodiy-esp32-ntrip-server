# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# semver.c, update_policy.c, app_trial.c, manifest.c and release_url.c hold
# the decisions (no RTOS or driver code; manifest.c uses cJSON and PSA
# Crypto) and are also compiled into the host test app. mbedtls provides
# psa/crypto.h (hashing, the optional manifest signature) and the CA bundle.
# Downloads use esp_http_client and esp_ota_* directly (not esp_https_ota,
# which follows redirects on its own, past the release-host check).
idf_component_register(
    SRCS
        "fw_update.c"
        "fw_trial.c"
        "fw_install.c"
        "fw_http.c"
        "manifest.c"
        "update_policy.c"
        "app_trial.c"
        "semver.c"
        "release_url.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES
        app_update
        esp_app_format
        esp_http_client
        esp-tls
        esp_partition
        esp_timer
        esp_hw_support
        mbedtls
        pkg_store
        settings
        lifecycle
        diag
        station
        network
        ntrip_server
        rtcm_hub)
