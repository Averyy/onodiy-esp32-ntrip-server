# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Pure logic: no IDF/RTOS includes so it also builds for the linux host target.
idf_component_register(
    SRCS "gnss_framing.c" "gnss_rtcm3.c"
    INCLUDE_DIRS "include")
