# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Host unit tests (ESP-IDF "linux" target, Unity): the pure cores, plus the
# settings component on IDF's emulated flash (NVS builds for the linux target).
#
#   cd test/host
#   idf.py --preview set-target linux
#   idf.py build
#   ./build/onocoydiy_host_tests.elf   # exit status 0 = all passed, 1 = a test failed
#
# Rule for the firmware: decision logic lives in pure cores (no IDF or RTOS
# includes, injected clock, I/O behind callbacks) next to the component that
# uses them, and every core is compiled and tested here (main/CMakeLists.txt).

cmake_minimum_required(VERSION 3.22)

# Whole components that build for the linux target (no drivers, no Wi-Fi).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/gnss_framing"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/gnss_types"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(onocoydiy_host_tests)

foreach(_comp main gnss_framing settings)
    idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
    target_compile_options(${_lib} PRIVATE
        -Wall -Wextra -Werror
        $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
endforeach()
