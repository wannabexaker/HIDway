cmake_minimum_required(VERSION 3.13)

set(HIDWAY_PICO_SDK_VERSION "2.3.1" CACHE STRING "Pico SDK version under ~/.pico-sdk/sdk")

if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Board type")
endif ()

include(pico_sdk_import.cmake)

project(hidway_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)

option(HIDWAY_T0 "Phase-0 go/no-go test mode: BOOTSEL holds W and moves the mouse" ON)
set(HIDWAY_USB_VID "0xCAFE" CACHE STRING "USB vendor ID (development default)")
set(HIDWAY_USB_PID "0x4877" CACHE STRING "USB product ID")

pico_sdk_init()

set(HIDWAY_COMMON ${CMAKE_CURRENT_LIST_DIR}/../common)

add_executable(hidway_fw
    src/main.c
    src/usb_descriptors.c
    src/hid_out.c
    src/bootsel.c
    src/led.c
    src/t0.c
    ${HIDWAY_COMMON}/hidway_kbd.c
    ${HIDWAY_COMMON}/hidway_motion.c
)

target_include_directories(hidway_fw PRIVATE src ${HIDWAY_COMMON})

target_compile_definitions(hidway_fw PRIVATE
    HIDWAY_USB_VID=${HIDWAY_USB_VID}
    HIDWAY_USB_PID=${HIDWAY_USB_PID}
    HIDWAY_T0=$<BOOL:${HIDWAY_T0}>
)

target_compile_options(hidway_fw PRIVATE -Wall)

target_link_libraries(hidway_fw PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_watchdog
    hardware_sync
    tinyusb_device
)

if (PICO_CYW43_SUPPORTED)
    target_link_libraries(hidway_fw PRIVATE pico_cyw43_arch_none)
endif ()

# UART0 is reserved for the Raspberry Pi link; USB is the HID device.
pico_enable_stdio_usb(hidway_fw 0)
pico_enable_stdio_uart(hidway_fw 0)

pico_add_extra_outputs(hidway_fw)
