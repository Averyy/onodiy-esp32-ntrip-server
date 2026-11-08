# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(SRCS "app_main.c" "product_web.c" INCLUDE_DIRS "."
    PRIV_REQUIRES web_shell settings diag lifecycle network nvs_flash esp_driver_gpio esp_timer esp_netif)
include("${STATION_DIR}/components/web_shell/cmake/web_ui.cmake")
web_shell_ui_bundle(TARGET ${COMPONENT_LIB}
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../ui/product.json"
    PRODUCT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../ui"
    ASSET_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../ui/assets.json"
    SYMBOL_PREFIX counter_ui MAX_PROJECT_GZ 163840 MAX_GZ 163840)
