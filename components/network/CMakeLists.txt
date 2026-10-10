# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# net_core.c, net_ladder.c and captive_dns.c are pure logic (no IDF
# includes) and are also compiled into the host test app.
#
# esp_eth is used only by the QEMU development build
# (CONFIG_NETWORK_QEMU_ETHERNET); requirements cannot depend on the
# configuration, so it is always listed. A station image links none of it.
idf_component_register(
    SRCS "network.c" "net_core.c" "net_ladder.c" "captive_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES settings diag lifecycle ledger esp_wifi esp_eth esp_netif lwip esp_timer)
