# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
include_guard(GLOBAL)

# Embed a product's ordered composition in its own target. All selected sources,
# manifests and pinned vendor hashes participate in configure/build dependencies.
function(web_shell_ui_bundle)
    cmake_parse_arguments(UI "" "TARGET;MANIFEST;PRODUCT_ROOT;ASSET_MANIFEST;SYMBOL_PREFIX;MAX_GZ;MAX_PROJECT_GZ" "" ${ARGN})
    foreach(_arg TARGET MANIFEST PRODUCT_ROOT ASSET_MANIFEST SYMBOL_PREFIX MAX_GZ MAX_PROJECT_GZ)
        if(NOT UI_${_arg})
            message(FATAL_ERROR "web_shell_ui_bundle requires ${_arg}")
        endif()
    endforeach()
    if(UI_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown UI arguments: ${UI_UNPARSED_ARGUMENTS}")
    endif()
    set(_bundler "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/ui_bundle.py")
    find_program(_web_ui_uv uv REQUIRED)
    if(COMMAND idf_build_get_property)
        idf_build_get_property(_web_ui_python PYTHON)
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(_web_ui_python "${Python3_EXECUTABLE}")
    endif()
    set(_command "${_web_ui_uv}" run --no-project "${_web_ui_python}" "${_bundler}"
        --manifest "${UI_MANIFEST}" --product-root "${UI_PRODUCT_ROOT}"
        --asset-manifest "${UI_ASSET_MANIFEST}" --symbol-prefix "${UI_SYMBOL_PREFIX}"
        --max-gz "${UI_MAX_GZ}" --max-project-gz "${UI_MAX_PROJECT_GZ}")
    execute_process(COMMAND ${_command} --list-inputs
        RESULT_VARIABLE _result OUTPUT_VARIABLE _inputs ERROR_VARIABLE _error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "UI composition failed: ${_error}")
    endif()
    string(REPLACE "\n" ";" _inputs "${_inputs}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs} "${_bundler}")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/${UI_SYMBOL_PREFIX}.c")
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/${UI_SYMBOL_PREFIX}.h")
    add_custom_command(OUTPUT "${_out}" "${_header}"
        COMMAND ${_command} --c-out "${_out}"
        DEPENDS "${_bundler}" ${_inputs}
        COMMENT "Checking and embedding ${UI_SYMBOL_PREFIX} UI"
        VERBATIM)
    target_sources(${UI_TARGET} PRIVATE "${_out}")
    target_include_directories(${UI_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
