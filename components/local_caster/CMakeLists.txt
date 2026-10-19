# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# lcast_core.c is pure logic (no IDF or RTOS calls; it uses the public
# config type and diag.h's record helpers) and is also compiled into the host
# test app. local_caster.c is the task, the sockets, the hub consumers, the
# stored config and the status.
idf_component_register(
    SRCS "local_caster.c" "lcast_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES rtcm_hub gnss_obs settings diag lifecycle esp_event esp_netif lwip esp_timer esp_app_format)
