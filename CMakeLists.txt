# Host-side build: unit tests for common/ (and later the Windows client and
# the Raspberry Pi relay). The Pico firmware is a separate CMake project in
# firmware/ because it needs the ARM cross toolchain.
cmake_minimum_required(VERSION 3.20)
project(hidway_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(HIDWAY_COMMON ${CMAKE_CURRENT_LIST_DIR}/common)

# Vendored Monocypher (unmodified upstream code) as its own library, so the
# strict warning level of our targets is not applied to it.
add_library(hidway_monocypher STATIC ${HIDWAY_COMMON}/vendor/monocypher/monocypher.c)
target_include_directories(hidway_monocypher PUBLIC ${HIDWAY_COMMON})

enable_testing()
add_subdirectory(tests)

if (WIN32)
    add_subdirectory(client)
endif ()
