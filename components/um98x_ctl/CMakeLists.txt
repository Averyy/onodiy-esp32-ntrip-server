# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# um98x_profile.c is pure logic (no IDF includes) and is also compiled into
# the host test app.
idf_component_register(
    SRCS "um98x_ctl.c" "um98x_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES gnss_link gnss_types
    PRIV_REQUIRES settings)
