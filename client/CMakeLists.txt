# HIDway client - Windows GUI application.
if (MSVC)
    enable_language(RC)
endif ()

add_executable(hidway-client WIN32
    src/main.c
    src/config.c
    src/net.c
    ${HIDWAY_COMMON}/keymap_sc2hid.c
    $<$<BOOL:${MSVC}>:app.rc>
)

target_include_directories(hidway-client PRIVATE src ${HIDWAY_COMMON})

if (MSVC)
    # /utf-8: string literals (e.g. "·", "↓", "✓") are UTF-8 in the binary.
    target_compile_options(hidway-client PRIVATE /W4 /utf-8)
    # Standard C string/stdio functions (strcpy, fopen, ...) are used
    # deliberately and bounded; silence MSVC's _s-variant deprecations.
    target_compile_definitions(hidway-client PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

target_link_libraries(hidway-client PRIVATE
    user32 gdi32 ws2_32 winmm dwmapi shell32 uxtheme gdiplus)
