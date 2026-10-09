# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "diag.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espcoredump esp_partition)
