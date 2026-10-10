# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# pkg_header.c is pure logic (no IDF includes) and is also compiled into the
# host test app; the whole component also builds for the linux target (IDF's
# emulated flash). mbedtls provides psa/crypto.h (PSA Crypto API, hardware
# SHA driver); esp_system the task watchdog.
idf_component_register(
    SRCS "pkg_store.c" "pkg_header.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    PRIV_REQUIRES esp_partition mbedtls esp_system)
