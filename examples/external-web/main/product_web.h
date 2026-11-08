/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
esp_err_t product_web_init(void);
/* App task requests recovery; HTTP tick performs the auth reset. */
void product_web_recover(void);
