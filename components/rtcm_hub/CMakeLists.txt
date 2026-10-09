# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "rtcm_hub.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES gnss_framing esp_timer)
