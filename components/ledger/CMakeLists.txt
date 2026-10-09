# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "ledger.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES settings esp_timer)
