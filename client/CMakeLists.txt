# HIDway client - Windows GUI application.
add_executable(hidway-client WIN32
    src/main.c
    src/config.c
    src/net.c
    ${HIDWAY_COMMON}/keymap_sc2hid.c
)

target_include_directories(hidway-client PRIVATE src ${HIDWAY_COMMON})

if (MSVC)
    target_compile_options(hidway-client PRIVATE /W4)
    # Standard C string/stdio functions (strcpy, fopen, ...) are used
    # deliberately and bounded; silence MSVC's _s-variant deprecations.
    target_compile_definitions(hidway-client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

target_link_libraries(hidway-client PRIVATE user32 gdi32 ws2_32 winmm dwmapi shell32)
