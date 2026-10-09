# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# settings_codec.c is pure logic (no IDF includes). The host test app builds
# this whole component for the linux target (NVS on emulated flash).
#
# nvs_flash is public: the getters return its error codes (ESP_ERR_NVS_*).
idf_component_register(
    SRCS "settings.c" "settings_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash)
