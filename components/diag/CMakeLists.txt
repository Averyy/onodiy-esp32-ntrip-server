# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# diag_logring.c, diag_journal_core.c, diag_task_core.c and diag_crc.c are
# pure logic (no IDF includes) and are also compiled into the host test app.
idf_component_register(
    SRCS "diag.c" "diag_log.c" "diag_journal.c"
         "diag_logring.c" "diag_journal_core.c" "diag_task_core.c" "diag_crc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES log
    PRIV_REQUIRES espcoredump esp_partition esp_driver_uart esp_mm esp_timer esp_system hal esp_app_format)
