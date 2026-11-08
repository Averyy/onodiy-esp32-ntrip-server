# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# An ESP-IDF application outside the station that uses only the receiver
# components of onodiy-esp32-ntrip-server (docs/component-reuse.md). CI
# copies this directory out of the repository and builds it there
# (tools/ci/external_app.sh):
#
#   idf.py -D STATION_DIR=<checkout> [-D STATION_FLASH=8MB] set-target esp32s3
#   idf.py -D STATION_DIR=<checkout> [-D STATION_FLASH=8MB] build
#
# STATION_DIR: a checkout of this repository at the revision the product
# pins (by default the checkout this directory sits in). STATION_UPDATER=ON
# adds the receiver updater (um98x_flash and the diag and pkg_store it needs). STATION_FLASH: the
# station layout the image is for, 16MB (partitions.csv: the 16 MB boards)
# or 8MB (partitions-8mb.csv: the NTRIP-X); it goes into the
# generated sdkconfig, so another layout needs another build directory and
# sdkconfig.
cmake_minimum_required(VERSION 3.22)

set(STATION_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "onodiy-esp32-ntrip-server checkout (pinned revision)")
get_filename_component(STATION_DIR "${STATION_DIR}" ABSOLUTE)
set(STATION_FLASH "16MB" CACHE STRING "Station layout: 16MB (partitions.csv) or 8MB (partitions-8mb.csv)")
option(STATION_UPDATER "Also the receiver updater (um98x_flash with diag and pkg_store)" OFF)

# The supported receiver components, each by its path: nothing else of the
# station is visible to the build.
set(STATION_COMPONENTS board settings gnss_types gnss_framing gnss_link gnss_obs um98x_ctl leds)
if(STATION_UPDATER)
    list(APPEND STATION_COMPONENTS um98x_flash diag pkg_store)
endif()
foreach(_c ${STATION_COMPONENTS})
    if(NOT EXISTS "${STATION_DIR}/components/${_c}/CMakeLists.txt")
        message(FATAL_ERROR "STATION_DIR ${STATION_DIR} has no component ${_c}")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${STATION_DIR}/components/${_c}")
endforeach()

# The station's partition layout: NVS where the station keeps the shared
# keys, and on the 8 MB board its board.<id> marker partition.
if(STATION_FLASH STREQUAL "16MB")
    set(_table "partitions.csv")
elseif(STATION_FLASH STREQUAL "8MB")
    set(_table "partitions-8mb.csv")
else()
    message(FATAL_ERROR "STATION_FLASH must be 16MB or 8MB, not '${STATION_FLASH}'")
endif()
set(_layout "${CMAKE_BINARY_DIR}/station_layout.defaults")
file(WRITE "${_layout}"
    "CONFIG_ESPTOOLPY_FLASHSIZE_${STATION_FLASH}=y\n"
    "CONFIG_PARTITION_TABLE_CUSTOM=y\n"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${STATION_DIR}/${_table}\"\n"
    "CONFIG_PARTITION_TABLE_OFFSET=0x10000\n")
if(STATION_UPDATER)
    # The updater checks and sends a PSRAM copy of the package (and stages it
    # there on the 8 MB layout): the boards' octal PSRAM, as the station sets it.
    file(APPEND "${_layout}"
        "CONFIG_SPIRAM=y\n"
        "CONFIG_SPIRAM_MODE_OCT=y\n"
        "CONFIG_SPIRAM_SPEED_80M=y\n"
        "CONFIG_SPIRAM_USE_MALLOC=y\n"
        # diag, which the updater links, reads the tasks' run-time statistics.
        "CONFIG_FREERTOS_USE_TRACE_FACILITY=y\n"
        "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y\n"
        "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y\n"
        "CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y\n")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${_layout}")

set(COMPONENTS main)
if(STATION_UPDATER)
    list(APPEND COMPONENTS um98x_flash pkg_store)   # main takes them as optional requirements
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(external_receiver)

if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(WARNING "the receiver components are built and tested with ESP-IDF 6.1 only")
endif()

# The station's strict warnings on the shared components here as well, so a
# warning that only an outside configuration shows fails in this repository
# first. A product may keep IDF's default flags for them instead.
foreach(_c ${STATION_COMPONENTS})
    idf_component_get_property(_lib ${_c} COMPONENT_LIB)
    get_target_property(_type ${_lib} TYPE)
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${_lib} PRIVATE -Wall -Wextra -Werror $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
    endif()
endforeach()
