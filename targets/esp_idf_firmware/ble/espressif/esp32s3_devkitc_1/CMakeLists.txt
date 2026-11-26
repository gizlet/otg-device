cmake_minimum_required(VERSION 3.16)

set(H2_ESP_TARGET "esp32s3")
set(H2_ESP_BOARD_DIR "$ENV{H2_GIZOS_ROOT}/boards/espressif/esp32s3_devkitc_1/${H2_ESP_TARGET}" CACHE PATH "GizOS board component directory" FORCE)

if(NOT DEFINED IDF_TARGET)
  set(IDF_TARGET "${H2_ESP_TARGET}" CACHE STRING "ESP-IDF target")
endif()
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "ESP-IDF sdkconfig" FORCE)
set(SDKCONFIG_DEFAULTS
  "${H2_ESP_BOARD_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.ble.defaults"
  CACHE STRING "ESP-IDF sdkconfig defaults" FORCE)

include("$ENV{H2_BAZEL_COMPONENT_MANIFEST}")
set(EXTRA_COMPONENT_DIRS ${H2_BAZEL_COMPONENT_DIRS})
set(COMPONENTS ${H2_BAZEL_COMPONENT_NAMES})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(otg_ble)
