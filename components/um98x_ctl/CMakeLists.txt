# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# um98x_profile.c, um98x_base.c, um98x_reply.c, um98x_txn.c, um98x_plan.c
# and um98x_exec.c are pure logic (no IDF includes) and are also compiled
# into the host test app; um98x_ctl.c is the RTOS/link wrapper around them.
idf_component_register(
    SRCS
        "um98x_ctl.c"
        "um98x_profile.c"
        "um98x_base.c"
        "um98x_reply.c"
        "um98x_txn.c"
        "um98x_plan.c"
        "um98x_exec.c"
        "um98x_bands.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_link gnss_types
    PRIV_REQUIRES settings gnss_obs gnss_framing esp_timer)
