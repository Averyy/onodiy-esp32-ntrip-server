# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Product-neutral HTTP runtime and pure cores (cores compiled directly by host tests).
# Public dependencies supply the snapshot and JSON types in include/.
idf_component_register(
    SRCS
        "web_shell.c"
        "web_shell_auth.c"
        "web_shell_routes.c"
        "web_shell_ws.c"
        "web_shell_worker.c"
        "web_shell_registry.c"
        "web_shell_service.c"
        "web_shell_errors.c"
        "web_shell_error_build.c"
        "web_shell_auth_parse.c"
        "web_json.c"
        "web_conn.c"
        "web_pw.c"
        "web_assets.c"
        "web_shell_core.c"
        "web_shell_parse.c"
        "web_shell_build.c"
        "web_shell_enums.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_http_server espressif__cjson diag lifecycle network
    PRIV_REQUIRES settings mbedtls esp_netif esp_timer esp_app_format
                  app_update esp_partition esp_hw_support esp_psram spi_flash lwip)
