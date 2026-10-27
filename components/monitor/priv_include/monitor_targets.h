/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The normal monitor and temporary diagnostics use the same cached IPv4
 * caster selection. This does no DNS lookup and changes no caster state. */
bool monitor_server_target(uint32_t *addr, char *name, size_t size);
