cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH environment variable is not set")
endif()
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH environment variable is not set")
endif()
if(NOT IS_DIRECTORY "$ENV{IDF_PATH}/tools/cmake")
    message(FATAL_ERROR "IDF_PATH does not point to an ESP-IDF checkout: $ENV{IDF_PATH}")
endif()

set(ESP_MATTER_PATH "$ENV{ESP_MATTER_PATH}")
set(MATTER_SDK_PATH "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip")

if(NOT IS_DIRECTORY "${ESP_MATTER_PATH}" OR NOT IS_DIRECTORY "${MATTER_SDK_PATH}")
    message(FATAL_ERROR "ESP_MATTER_PATH does not point to a complete ESP-Matter checkout: ${ESP_MATTER_PATH}")
endif()

set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
)

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")

project(srex_c6_matter_thread_lock)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
