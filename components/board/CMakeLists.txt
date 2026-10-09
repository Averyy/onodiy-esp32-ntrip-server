# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio)
