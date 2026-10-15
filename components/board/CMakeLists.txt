# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# board_pins.c is pure logic (no IDF includes) and is also compiled into the
# host test app.
idf_component_register(
    SRCS "board.c" "board_pins.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES esp_driver_gpio)
