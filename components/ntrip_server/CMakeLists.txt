# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# ntrip_proto.c is pure logic (no IDF includes); ntrip_host_rules.c (the
# Onocoy preset and the host rules) and ntrip_cfg_blob.c (a slot's stored
# record) are pure logic on the public config type; all three are also
# compiled into the host test app.
idf_component_register(
    SRCS "ntrip_server.c" "ntrip_proto.c" "ntrip_host_rules.c" "ntrip_cfg_blob.c" "ntrip_tx_diag_core.c" "ntrip_io.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event rtcm_hub tcp_diag
    PRIV_REQUIRES settings lifecycle ledger diag esp-tls mbedtls esp_netif esp_wifi lwip esp_timer
                  esp_app_format)
