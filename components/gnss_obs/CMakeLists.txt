# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "gnss_obs.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES gnss_framing)
