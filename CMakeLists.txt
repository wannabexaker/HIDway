# Host-side build: unit tests for common/ (and later the Windows client and
# the Raspberry Pi relay). The Pico firmware is a separate CMake project in
# firmware/ because it needs the ARM cross toolchain.
cmake_minimum_required(VERSION 3.20)
project(hidway_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HIDWAY_COMMON ${CMAKE_CURRENT_LIST_DIR}/common)

enable_testing()
add_subdirectory(tests)

if (WIN32)
    add_subdirectory(client)
endif ()
