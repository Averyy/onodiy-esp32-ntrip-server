# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# diag_logring.c, diag_console_core.c, diag_mirror_core.c, diag_journal_core.c,
# diag_task_core.c and diag_crc.c are pure logic (no IDF includes) and are
# also compiled into the host test app.
#
# esp_driver_usb_serial_jtag: the console's copy on the built-in USB
# (CONFIG_DIAG_CONSOLE_USB_MIRROR); requirements cannot depend on the
# configuration, so it is always listed, and a build without the copy links
# none of it.
idf_component_register(
    SRCS "diag.c" "diag_log.c" "diag_journal.c"
         "diag_logring.c" "diag_console_core.c" "diag_mirror_core.c" "diag_journal_core.c" "diag_task_core.c"
         "diag_crc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES log
    PRIV_REQUIRES espcoredump esp_partition esp_driver_uart esp_driver_usb_serial_jtag esp_mm esp_timer esp_system
                  hal esp_app_format)
