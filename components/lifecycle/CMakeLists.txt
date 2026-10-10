# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# boot_guard.c, reboot_gate.c and boot_history.c are pure logic (no IDF
# includes) and are also compiled into the host test app.
idf_component_register(
    SRCS "lifecycle.c" "boot_guard.c" "reboot_gate.c" "boot_history.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_event
    PRIV_REQUIRES settings diag esp_timer esp_app_format)
