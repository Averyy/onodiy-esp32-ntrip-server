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

# IDF's components form static-library cycles, and esp_common has CMake
# repeat their archives on the link line (LINK_INTERFACE_MULTIPLICITY 4), as
# GNU ld needs. macOS ld64 resolves archives in any order and warns of every
# repeat ("ignoring duplicate libraries"). With CMP0156 (CMake 3.29) CMake
# repeats archives only for a linker that needs it, and with CMP0179 (3.31)
# keeps the first occurrence, the one ld64 uses. IDF's project.cmake sets the
# policies of 3.22 for the code that creates the executable, so these are the
# defaults it starts from, set before it is included. GNU ld keeps the repeats.
set(CMAKE_POLICY_DEFAULT_CMP0156 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0179 NEW)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(onocoydiy_host_tests)

# hal has the linker keep its assertion handler (-u __assert_func, at HAL
# assertion level 2). Mach-O names C functions with a leading underscore, so
# on macOS the option asks for the C function _assert_func, which IDF's linux
# component defines (linux/assert_func.c, macOS only) just for it. That object
# defines __assert_func as well, and esp_rom's linux port, linked for the ROM
# output functions, defines it too (esp_rom/linux/esp_rom_sys.c): ld64 warns of
# the duplicate symbol. Nothing linked here calls __assert_func (no hal object
# is), so on macOS the option goes, and the second definition with it. On
# Linux the option names __assert_func itself and esp_rom's is the only one.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    idf_component_get_property(_hal_lib hal COMPONENT_LIB)
    get_target_property(_hal_link ${_hal_lib} INTERFACE_LINK_LIBRARIES)
    if(_hal_link)
        list(REMOVE_ITEM _hal_link "-u __assert_func")
        set_property(TARGET ${_hal_lib} PROPERTY INTERFACE_LINK_LIBRARIES "${_hal_link}")
    endif()
endif()

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
