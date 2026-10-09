# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# mbedtls provides psa/crypto.h (PSA Crypto API, hardware SHA driver).
idf_component_register(
    SRCS "pkg_store.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_partition mbedtls)
