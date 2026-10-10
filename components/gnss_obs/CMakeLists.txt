# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# gnss_obs_core.c and gnss_obs_parse.c are pure logic (no IDF includes) and
# are also compiled into the host test app. gnss_framing and gnss_types are
# public: gnss_obs.h uses their RTCM and profile types.
idf_component_register(
    SRCS "gnss_obs.c" "gnss_obs_core.c" "gnss_obs_parse.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_framing gnss_types
    PRIV_REQUIRES esp_timer)
