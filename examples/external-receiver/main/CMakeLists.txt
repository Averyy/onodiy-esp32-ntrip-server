# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "example_receiver.c"
    INCLUDE_DIRS "."
    REQUIRES board settings gnss_link gnss_obs um98x_ctl leds nvs_flash esp_event)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
