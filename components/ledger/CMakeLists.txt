# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# ledger_core.c is pure logic (no IDF includes) and is also compiled into the
# host test app.
idf_component_register(
    SRCS "ledger.c" "ledger_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES settings diag esp_timer esp_system)
