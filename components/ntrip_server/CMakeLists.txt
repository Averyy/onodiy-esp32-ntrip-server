# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# ntrip_proto.c is pure logic (no IDF includes) and is also compiled into the
# host test app.
idf_component_register(
    SRCS "ntrip_server.c" "ntrip_proto.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES rtcm_hub settings lifecycle ledger esp-tls mbedtls esp_netif esp_wifi lwip esp_timer
                  esp_app_format)
