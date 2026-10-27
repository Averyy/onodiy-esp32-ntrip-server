# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
idf_component_register(SRCS "tcp_diag.c" INCLUDE_DIRS "include" PRIV_REQUIRES lwip)
# The lwIP hook in network references this even when no sender is active.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u net_tcp_getsockopt")
