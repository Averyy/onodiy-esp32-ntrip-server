/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station as the updater's product: how its states read for the app
 * trial and for starting a receiver update.
 */
#include "fw_station_map.h"
#include "unity.h"

TEST_CASE("station as product: which states excuse a trial and which allow a receiver update", "[fw_update]")
{
    static const struct {
        station_state_t state;
        bool problem;
        bool ready;
    } k[] = {
        { STATION_STATE_IDLE, false, false },           { STATION_STATE_STREAMING, false, true },
        { STATION_STATE_DETECTING, true, false },       { STATION_STATE_CONFIGURING, true, false },
        { STATION_STATE_RECOVERING, true, false },      { STATION_STATE_NO_RECEIVER, true, false },
        { STATION_STATE_FLASHING, true, false },        { STATION_STATE_FLASH_RECOVERY, true, false },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        TEST_ASSERT_EQUAL(k[i].problem, fw_station_receiver_problem(k[i].state));
        TEST_ASSERT_EQUAL(k[i].ready, fw_station_receiver_ready(k[i].state));
    }
}

TEST_CASE("station as product: the hub counts as fed within 5 s of its latest frame", "[fw_update]")
{
    TEST_ASSERT_TRUE(fw_station_hub_fed(0));
    TEST_ASSERT_TRUE(fw_station_hub_fed(5000));
    TEST_ASSERT_FALSE(fw_station_hub_fed(5001));
    TEST_ASSERT_FALSE(fw_station_hub_fed(UINT32_MAX));
}
