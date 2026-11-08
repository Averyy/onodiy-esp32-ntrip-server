# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "example_receiver.c"
    INCLUDE_DIRS "."
    REQUIRES board settings gnss_link gnss_obs um98x_ctl leds nvs_flash esp_event)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# The receiver updater when the project builds it (STATION_UPDATER=ON adds it
# to COMPONENTS; requirements are read in a pass that does not see options).
idf_build_get_property(_build_components BUILD_COMPONENTS)
if(um98x_flash IN_LIST _build_components)
    idf_component_optional_requires(PRIVATE um98x_flash pkg_store)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EXAMPLE_UPDATER=1)
endif()
if(fw_update IN_LIST _build_components)
    idf_component_optional_requires(PRIVATE fw_update lifecycle)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EXAMPLE_APP_UPDATE=1)
endif()
