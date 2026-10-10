# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# flash_journal.c is pure logic (no IDF includes) and is also compiled into
# the host test app.
idf_component_register(
    SRCS "um98x_flash.c" "flash_journal.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_link gnss_types
    PRIV_REQUIRES pkg_store settings)
