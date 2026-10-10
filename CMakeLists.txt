# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Top-level ESP-IDF project for the onocoydiy-esp-um982 station firmware.
# Build from the repository root:  idf.py set-target esp32s3 && idf.py build

cmake_minimum_required(VERSION 3.22)

# Application version (lands in esp_app_desc_t.version, which holds 31
# characters; IDF silently cuts anything longer).
#
# Release builds: CI passes -DRELEASE_VERSION=vX.Y.Z on tag builds, and only
# those are release versions. The tag must look like a release, fit in 31
# characters and point at HEAD, or configure fails. The value is taken for
# this configure only and dropped from the cache, so the next reconfigure of
# the same build directory (a commit, checkout or staging change, or
# `idf.py reconfigure`) goes back to a dev version instead of naming dev
# images after the release. -DPROJECT_VER is refused because it would stick
# in the cache.
#
# Every other build gets `git describe` output plus "+dev" (e.g.
# "v0.3.0-4-g47fc47b-dirty+dev"), the describe part cut to 27 characters so
# "+dev" always survives the 31-character limit. The firmware rejects any
# version with "+", so a dev build is never taken for a release, never
# offered an update by the daily check and never offered as a downgrade
# target. (`git describe`
# output alone would parse as a pre-release.) The version is fixed when CMake
# configures: the git files below re-trigger that on commits, checkouts and
# staging, but not on unstaged edits, so run `idf.py reconfigure` before a
# local build whose version string (and -dirty flag) matters.
if(DEFINED CACHE{PROJECT_VER})
    message(FATAL_ERROR "-DPROJECT_VER is not used here: pass -DRELEASE_VERSION=vX.Y.Z for a release build "
                        "(and remove PROJECT_VER from the build directory's cache, or run idf.py fullclean)")
endif()
set(_release_version "${RELEASE_VERSION}")
unset(RELEASE_VERSION CACHE)
if(NOT "${_release_version}" STREQUAL "")
    string(LENGTH "${_release_version}" _ver_len)
    if(NOT _release_version MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$" OR _ver_len GREATER 31)
        message(FATAL_ERROR "RELEASE_VERSION \"${_release_version}\" is not a release tag "
                            "(vMAJOR.MINOR.PATCH[-PRE], at most 31 characters)")
    endif()
    execute_process(
        COMMAND git tag --points-at HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE _head_tags
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(REPLACE "\n" ";" _head_tags "${_head_tags}")
    if(NOT _release_version IN_LIST _head_tags)
        message(FATAL_ERROR "RELEASE_VERSION ${_release_version}: HEAD is not tagged ${_release_version}")
    endif()
    set(PROJECT_VER "${_release_version}")
    message(STATUS "Release build ${PROJECT_VER}")
else()
    execute_process(
        COMMAND git describe --tags --match "v[0-9]*" --always --dirty
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE _git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rc)
    if(NOT _git_rc EQUAL 0 OR "${_git_describe}" STREQUAL "")
        set(_git_describe "unknown")
    endif()
    string(SUBSTRING "${_git_describe}" 0 27 _git_describe)
    set(PROJECT_VER "${_git_describe}+dev")
endif()
execute_process(
    COMMAND git rev-parse --absolute-git-dir
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _git_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git symbolic-ref -q HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _git_head_ref
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_git_dir)
    foreach(_f HEAD index packed-refs ${_git_head_ref})
        if(EXISTS "${_git_dir}/${_f}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_dir}/${_f}")
        endif()
    endforeach()
endif()
string(LENGTH "${PROJECT_VER}" _ver_len)
if(_ver_len GREATER 31)
    message(FATAL_ERROR "PROJECT_VER \"${PROJECT_VER}\" is longer than the 31 characters esp_app_desc_t holds")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(onocoydiy-esp-um982)

# Our components, by layer. A component may only depend on components of a
# LOWER layer; peers talk through events. The configure step fails on a
# same-layer or upward edge, so the graph stays acyclic as the implementation
# grows. Checked: declared REQUIRES/PRIV_REQUIRES and the libraries each
# component actually links (which also catches target_link_libraries and
# idf_component_optional_requires). Relative include paths into another
# component are rejected by CI (.github/workflows/build.yml). Every directory
# under components/ must be listed here.
set(_layer_0 board settings diag gnss_framing gnss_types)
set(_layer_1 gnss_link rtcm_hub pkg_store lifecycle ledger gnss_obs)
set(_layer_2 um98x_ctl um98x_flash ntrip_server network)
set(_layer_3 station)
set(_layer_4 fw_update health bench)
set(_layer_5 web)
set(_layer_6 main)

set(_our_components "")
foreach(_layer RANGE 0 6)
    foreach(_comp ${_layer_${_layer}})
        set(_layer_of_${_comp} ${_layer})
        list(APPEND _our_components ${_comp})
    endforeach()
endforeach()

file(GLOB _component_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/components/*")
foreach(_dir ${_component_dirs})
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
        get_filename_component(_name "${_dir}" NAME)
        if(NOT DEFINED _layer_of_${_name})
            message(FATAL_ERROR "components/${_name} has no layer: add it to the layer lists in CMakeLists.txt")
        endif()
    endif()
endforeach()

foreach(_comp ${_our_components})
    idf_component_get_property(_reqs ${_comp} REQUIRES)
    idf_component_get_property(_priv ${_comp} PRIV_REQUIRES)
    idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
    get_target_property(_type ${_lib} TYPE)

    # Linked libraries: component libraries appear as __idf_<name> or
    # idf::<name>, possibly inside generator expressions.
    set(_linked "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        if(_prop STREQUAL "LINK_LIBRARIES" AND _type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        get_target_property(_libs ${_lib} ${_prop})
        if(_libs)
            string(REGEX MATCHALL "(__idf_|idf::)[A-Za-z0-9_]+" _names "${_libs}")
            foreach(_name ${_names})
                string(REGEX REPLACE "^(__idf_|idf::)" "" _name "${_name}")
                list(APPEND _linked ${_name})
            endforeach()
        endif()
    endforeach()

    foreach(_dep ${_reqs} ${_priv} ${_linked})
        if(_dep STREQUAL _comp)
            continue()
        endif()
        if(DEFINED _layer_of_${_dep} AND NOT _layer_of_${_dep} LESS _layer_of_${_comp})
            message(FATAL_ERROR "layering: ${_comp} (L${_layer_of_${_comp}}) must not depend on "
                                "${_dep} (L${_layer_of_${_dep}}); only lower layers are allowed")
        endif()
    endforeach()

    # Strict warnings for our own code only; IDF and managed components keep
    # their default flags.
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${_lib} PRIVATE
            -Wall -Wextra -Werror
            $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>)
    endif()
endforeach()
