# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# local_caster_stub.c is a RAM-only stand-in for the interface (no task, no
# socket, nothing stored; see the file), so the web API and the UI can build
# against local_caster.h until the caster itself replaces it.
idf_component_register(
    SRCS "local_caster_stub.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES settings)
