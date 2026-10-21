/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * pkg_store as on a board without the package partition (PSRAM staging),
 * for the host tests: pkg_store_init() decides that from the partition
 * table, which the tests' table always has.
 */
#pragma once

#include "esp_err.h"

/** pkg_store_init() without looking for the partition: PSRAM staging (CONFIG_PKG_STORE_RAM), else no package area. */
esp_err_t pkg_store_init_ram(void);
