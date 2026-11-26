include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native/native_component.cmake")
h2_bazel_native_component_sources(OTG_ESP_VHCI_SOURCES otg_esp_vhci)

idf_component_register(
  SRCS ${OTG_ESP_VHCI_SOURCES}
  INCLUDE_DIRS
    "include"
    "${H2_REPO_ROOT}/libs/bt/hci/include"
    "$ENV{H2_GIZOS_ROOT}/pal/include"
  REQUIRES
    bt
    nvs_flash)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
