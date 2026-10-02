# Locate the Raspberry Pi Pico SDK.
#
# Order: -DPICO_SDK_PATH=..., then the PICO_SDK_PATH environment variable,
# then the layout used by the Raspberry Pi Pico VS Code extension
# (~/.pico-sdk/sdk/<version>).

if (DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()

if (NOT PICO_SDK_PATH)
    if (WIN32)
        set(_hidway_home $ENV{USERPROFILE})
    else ()
        set(_hidway_home $ENV{HOME})
    endif ()
    set(_hidway_default_sdk "${_hidway_home}/.pico-sdk/sdk/${HIDWAY_PICO_SDK_VERSION}")
    if (EXISTS "${_hidway_default_sdk}/pico_sdk_init.cmake")
        set(PICO_SDK_PATH "${_hidway_default_sdk}")
    endif ()
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Pico SDK not found. Set PICO_SDK_PATH (environment or -D).")
endif ()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if (NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "'${PICO_SDK_PATH}' does not look like a Pico SDK")
endif ()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
