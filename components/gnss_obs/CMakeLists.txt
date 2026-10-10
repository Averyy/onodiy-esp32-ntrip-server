# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# gnss_obs_core.c is pure logic (no IDF includes) and is also compiled into
# the host test app. gnss_framing is public: gnss_obs.h uses its RTCM types.
idf_component_register(
    SRCS "gnss_obs.c" "gnss_obs_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_framing
    PRIV_REQUIRES esp_timer)
