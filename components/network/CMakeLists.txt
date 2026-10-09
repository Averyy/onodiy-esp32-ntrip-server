# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "network.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES settings esp_wifi esp_netif lwip esp_timer)
