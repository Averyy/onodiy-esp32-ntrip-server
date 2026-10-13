# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Bench console for bring-up (CONFIG_BENCH_CONSOLE, default off). Without the
# option bench.c compiles to two no-op functions.
idf_component_register(
    SRCS "bench.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES console diag esp_driver_uart station um98x_flash pkg_store settings lifecycle esp_timer gnss_link network)
