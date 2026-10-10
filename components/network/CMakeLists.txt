# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# net_core.c, net_ladder.c and captive_dns.c are pure logic (no IDF
# includes) and are also compiled into the host test app.
idf_component_register(
    SRCS "network.c" "net_core.c" "net_ladder.c" "captive_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES settings diag lifecycle ledger esp_wifi esp_netif lwip esp_timer)
