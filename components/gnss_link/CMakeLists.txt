# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "gnss_link.c"
    INCLUDE_DIRS "include"
    REQUIRES gnss_framing
    PRIV_REQUIRES board settings esp_driver_uart esp_timer)
