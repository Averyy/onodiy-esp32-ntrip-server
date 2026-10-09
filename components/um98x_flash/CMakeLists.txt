# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "um98x_flash.c"
    INCLUDE_DIRS "include"
    REQUIRES gnss_link gnss_types
    PRIV_REQUIRES pkg_store)
