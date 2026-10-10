# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# rtcm_hub_core.c and rtcm_epoch.c are pure logic (no IDF includes) and are
# also compiled into the host test app.
idf_component_register(
    SRCS "rtcm_hub.c" "rtcm_hub_core.c" "rtcm_epoch.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES gnss_framing esp_timer)
