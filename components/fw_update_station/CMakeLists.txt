# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# The station's side of the app and receiver updater (fw_update_set_product):
# everything fw_update needs to know about the station lives here, so the
# updater itself links no station, caster or hub code. fw_station_map.c is
# pure logic and also compiled into the host test app.
idf_component_register(
    SRCS "fw_update_station.c" "fw_station_map.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_common
    PRIV_REQUIRES fw_update station ntrip_server rtcm_hub network um98x_flash)
