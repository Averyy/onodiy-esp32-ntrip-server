# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "ntrip_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES rtcm_hub settings lifecycle ledger esp-tls esp_netif lwip esp_timer)
