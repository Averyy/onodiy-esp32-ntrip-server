# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Host unit tests (ESP-IDF "linux" target, Unity): the pure cores, plus the
# settings and pkg_store components on IDF's emulated flash (NVS and raw
# partitions build for the linux target; partitions.csv adds a package slot).
#
#   cd test/host
#   idf.py --preview set-target linux
#   idf.py build
#   ./build/onocoydiy_host_tests.elf   # exit status 0 = all passed, 1 = a test failed
#
# CI builds and runs them on Linux (gcc, glibc); tools/ci/host_tests_linux.sh
# does the same locally in CI's container.
#
# Rule for the firmware: decision logic lives in pure cores (no IDF or RTOS
# includes, injected clock, I/O behind callbacks) next to the component that
# uses them, and every core is compiled and tested here (main/CMakeLists.txt).

cmake_minimum_required(VERSION 3.22)

# Whole components that build for the linux target (no drivers, no Wi-Fi).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/gnss_framing"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/gnss_types"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/settings"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/pkg_store")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(onocoydiy_host_tests)

foreach(_comp main gnss_framing settings pkg_store)
    idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
    target_compile_options(${_lib} PRIVATE
        -Wall -Wextra -Werror
        $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
endforeach()

# IDF's mbedtls built for the linux target: its PSA core (tfpsacrypto) calls
# the component's allocator (esp_mbedtls_mem_calloc/free, port/esp_mem.c) and
# mbedtls_ms_time (port/esp_platform_time.c, in the TLS library) without
# declaring either link, so both archives come before it on the link line.
# GNU ld (Linux) resolves static archives in link order and fails there;
# macOS ld64 resolves them in any order. LINK_ONLY: the link edges alone, no
# compile flags. CMake repeats the archives of the cycle this makes.
idf_component_get_property(_mbedtls_lib mbedtls COMPONENT_LIB)
target_link_libraries(tfpsacrypto INTERFACE
    "$<LINK_ONLY:${_mbedtls_lib}>"
    "$<LINK_ONLY:mbedtls>")
