# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# monitor_core.c is pure logic (no IDF includes) and is also compiled into the
# host test app.
idf_component_register(
    SRCS "monitor.c" "monitor_core.c" "monitor_diag.c" "monitor_diag_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES network ntrip_server lwip esp_timer heap diag)
