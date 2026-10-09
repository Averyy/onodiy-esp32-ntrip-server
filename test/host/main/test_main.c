/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Non-interactive Unity runner for the linux host target: runs every
 * TEST_CASE and exits with status 0 if all passed and 1 if any failed, so CI
 * can use the exit status directly.
 */
#include <stdlib.h>

#include "unity.h"
#include "unity_test_runner.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    exit(failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
