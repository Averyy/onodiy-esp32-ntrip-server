# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# The flasher's decision logic is pure (no IDF includes) and also compiled
# into the host test app: flash_journal.c (journal codec, boot-time table),
# pkg_check.c and lzo1x.c (package rules), fl_rx.c, xmodem_tx.c, fl_proto.c
# and fl_session.c (the receiver side of a session), fl_util.c.
# um98x_flash.c and um98x_bench.c run them over the GNSS link.
idf_component_register(
    SRCS
        "um98x_flash.c"
        "um98x_bench.c"
        "flash_journal.c"
        "fl_util.c"
        "fl_rx.c"
        "xmodem_tx.c"
        "lzo1x.c"
        "pkg_check.c"
        "fl_proto.c"
        "fl_session.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES gnss_link gnss_types
    PRIV_REQUIRES diag gnss_framing pkg_store settings mbedtls esp_timer esp_system esp_hw_support heap)
