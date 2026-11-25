include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(OTG_USB_BTH_SOURCES otg_usb_bth)

idf_component_register(
  SRCS ${OTG_USB_BTH_SOURCES}
  INCLUDE_DIRS
    "include"
    "${H2_REPO_ROOT}/libs/bt/hci/include"
    "$ENV{H2_GIZOS_ROOT}/pal/include"
  REQUIRES
    espressif__esp_tinyusb
    esp_hw_support
    h2_firmware_lib)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
