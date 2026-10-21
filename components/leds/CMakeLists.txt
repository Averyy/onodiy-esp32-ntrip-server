# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# led_core.c is pure logic (no IDF includes) and is also compiled into the
# host test app; leds.c drives the board's LED pins with it.
idf_component_register(
    SRCS "leds.c" "led_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES board esp_driver_gpio esp_driver_ledc esp_timer)
