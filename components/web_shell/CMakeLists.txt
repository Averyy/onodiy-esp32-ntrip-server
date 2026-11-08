# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Product-neutral pure web cores, also compiled directly by the host tests.
# Public dependencies supply the snapshot and JSON types in include/.
idf_component_register(
    SRCS
        "web_json.c"
        "web_conn.c"
        "web_pw.c"
        "web_assets.c"
        "web_shell_core.c"
        "web_shell_parse.c"
        "web_shell_build.c"
        "web_shell_enums.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server espressif__cjson diag lifecycle network
    PRIV_REQUIRES settings mbedtls)
