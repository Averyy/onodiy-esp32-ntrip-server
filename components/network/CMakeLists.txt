# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# net_core.c, net_ladder.c and captive_dns.c are pure logic (no IDF
# includes) and are also compiled into the host test app.
#
# esp_eth is used only by the QEMU development build
# (CONFIG_NETWORK_QEMU_ETHERNET); requirements cannot depend on the
# configuration, so it is always listed. A station image links none of it.
#
# linker.lf places the mDNS responder's static buffers in PSRAM (see the
# file).
#
# net_tw.c is an lwIP hook (lwip_hooks/net_lwip_hooks.h): a client's reset
# ends a TIME_WAIT the station holds. ESP-IDF takes a project's hooks
# through ESP_IDF_LWIP_HOOK_FILENAME, defined for the lwIP library alone
# (ESP-IDF's lwIP guide, "Customized lwIP Hooks"). Only lwIP's tcp_in.c
# calls net_tw_inpacket, which is in this component's library: "-u" makes
# the linker take it whatever the order of the two libraries on the link
# line.
idf_component_register(
    SRCS "network.c" "net_core.c" "net_ladder.c" "captive_dns.c" "net_tw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include" "lwip_hooks"
    LDFRAGMENTS "linker.lf"
    REQUIRES esp_event
    PRIV_REQUIRES settings diag lifecycle ledger esp_wifi esp_eth esp_netif lwip esp_timer)

idf_component_get_property(_lwip_lib lwip COMPONENT_LIB)
target_include_directories(${_lwip_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lwip_hooks")
target_compile_definitions(${_lwip_lib} PRIVATE "ESP_IDF_LWIP_HOOK_FILENAME=\"net_lwip_hooks.h\"")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u net_tw_inpacket")
