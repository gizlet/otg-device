include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(OTG_MAIN_SOURCES main)

idf_component_register(
  SRCS ${OTG_MAIN_SOURCES}
  INCLUDE_DIRS
    "${H2_REPO_ROOT}/apps/ble/include"
    "${H2_REPO_ROOT}/libs/bt/hci/include"
    "$ENV{H2_GIZOS_ROOT}/pal/include"
    "$ENV{H2_GIZOS_ROOT}/libs/runtime/include"
  REQUIRES
    h2_esp_board
    h2_esp_target_task_policy
    h2_firmware_lib
    h2_pal_core
    otg_esp_vhci
    otg_usb_bth)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
