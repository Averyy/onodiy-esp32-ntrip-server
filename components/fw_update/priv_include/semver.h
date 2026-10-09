/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Release version parsing and ordering (pure logic, host-testable).
 *
 * Accepted: "vMAJOR.MINOR.PATCH" or "vMAJOR.MINOR.PATCH-PRE" (leading 'v'
 * optional, numbers without leading zeros, PRE = dot-separated identifiers of
 * [0-9A-Za-z-]). Build metadata ("+...") is rejected: releases never carry
 * it, so a dev build ("v0.3.0-4-g47fc47b-dirty+dev") is never a release.
 * The build keeps "+dev" inside the 31 characters esp_app_desc_t holds
 * (top-level CMakeLists.txt): `git describe` output alone would parse as a
 * pre-release of the tag it follows.
 *
 * Ordering follows Semantic Versioning 2.0.0 section 11: a pre-release sorts
 * before its release; pre-release identifiers compare numerically when both
 * are numeric, numeric ones sort before alphanumeric ones, and a shorter
 * list of identifiers sorts first when all shared ones are equal.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEMVER_PRE_MAX 32

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    char pre[SEMVER_PRE_MAX];   /**< "" for a release */
} semver_t;

/** Parse; false if `s` is not a release version. */
bool semver_parse(const char *s, semver_t *out);

/** <0, 0, >0 like strcmp. */
int semver_compare(const semver_t *a, const semver_t *b);

#ifdef __cplusplus
}
#endif
