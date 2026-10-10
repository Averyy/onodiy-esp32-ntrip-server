# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Pure cores are compiled straight from the firmware tree; their components
# as a whole need the real target. settings builds whole (NVS runs on IDF's
# emulated flash for the linux target).
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
        "test_versiona.c"
        "test_um98x_reply.c"
        "test_um98x_txn.c"
        "test_um98x_plan.c"
        "test_um98x_exec.c"
        "test_flash_journal.c"
        "${_fw}/lifecycle/boot_guard.c"
        "${_fw}/lifecycle/reboot_gate.c"
        "${_fw}/lifecycle/boot_history.c"
        "${_fw}/fw_update/semver.c"
        "${_fw}/um98x_ctl/um98x_profile.c"
        "${_fw}/um98x_ctl/um98x_base.c"
        "${_fw}/um98x_ctl/um98x_reply.c"
        "${_fw}/um98x_ctl/um98x_txn.c"
        "${_fw}/um98x_ctl/um98x_plan.c"
        "${_fw}/um98x_ctl/um98x_exec.c"
        "${_fw}/um98x_flash/flash_journal.c"
    INCLUDE_DIRS
        "."
    PRIV_INCLUDE_DIRS
        "${_fw}/lifecycle/priv_include"
        "${_fw}/fw_update/priv_include"
        "${_fw}/um98x_ctl/include"
        "${_fw}/um98x_ctl/priv_include"
        "${_fw}/um98x_flash/priv_include"
    REQUIRES unity gnss_framing gnss_types settings nvs_flash esp_partition esp_event
    WHOLE_ARCHIVE)

# The WGS84 conversion uses libm (part of libc on macOS, separate on Linux).
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
