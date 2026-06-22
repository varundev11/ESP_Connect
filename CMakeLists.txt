cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{ESP_HOMEKIT_SDK_PATH})
    set(ESP_HOMEKIT_SDK_PATH $ENV{ESP_HOMEKIT_SDK_PATH})
else()
    set(ESP_HOMEKIT_SDK_PATH "/home/varun/esp/esp-homekit-sdk")
endif()

if(EXISTS "${ESP_HOMEKIT_SDK_PATH}/components")
    list(APPEND EXTRA_COMPONENT_DIRS
        "${ESP_HOMEKIT_SDK_PATH}/components/button"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/mu_srp"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/hkdf-sha"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/esp_hap_extras"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/esp_hap_apple_profiles"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/esp_hap_core"
        "${ESP_HOMEKIT_SDK_PATH}/components/homekit/esp_hap_platform"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_connect_ota)
