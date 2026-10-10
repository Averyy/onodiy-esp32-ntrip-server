# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Pure cores are compiled straight from the firmware tree; their components
# as a whole need the real target. settings and pkg_store build whole (NVS
# and the package slot run on IDF's emulated flash for the linux target).
set(_fw "${CMAKE_CURRENT_LIST_DIR}/../../../components")

idf_component_register(
    SRCS
        "test_main.c"
        "test_gnss_framing.c"
        "test_settings_codec.c"
        "test_settings.c"
        "test_boot_guard.c"
        "test_reboot_gate.c"
        "test_boot_history.c"
        "test_semver.c"
        "test_um98x_profile.c"
        "test_flash_journal.c"
        "test_manifest.c"
        "test_update_policy.c"
        "test_app_trial.c"
        "test_pkg_header.c"
        "test_pkg_store.c"
        "${_fw}/lifecycle/boot_guard.c"
        "${_fw}/lifecycle/reboot_gate.c"
        "${_fw}/lifecycle/boot_history.c"
        "${_fw}/fw_update/semver.c"
        "${_fw}/fw_update/manifest.c"
        "${_fw}/fw_update/update_policy.c"
        "${_fw}/fw_update/app_trial.c"
        "${_fw}/um98x_ctl/um98x_profile.c"
        "${_fw}/um98x_flash/flash_journal.c"
    INCLUDE_DIRS
        "."
    PRIV_INCLUDE_DIRS
        "${_fw}/lifecycle/priv_include"
        "${_fw}/fw_update/priv_include"
        "${_fw}/um98x_ctl/include"
        "${_fw}/um98x_flash/priv_include"
        "${_fw}/pkg_store/priv_include"
    REQUIRES unity gnss_framing gnss_types settings pkg_store nvs_flash esp_partition esp_event mbedtls
    WHOLE_ARCHIVE)

# The WGS84 conversion uses libm (part of libc on macOS, separate on Linux).
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
