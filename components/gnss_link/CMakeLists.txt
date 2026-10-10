# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# gnss_link_core.c is pure logic (no driver or RTOS includes) and is also
# compiled into the host test app.
idf_component_register(
    SRCS "gnss_link.c" "gnss_link_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_framing
    PRIV_REQUIRES board settings esp_driver_uart esp_timer)
