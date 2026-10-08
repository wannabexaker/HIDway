# HIDway client - Windows GUI application, plus the hidway-probe console tool
# (reachability/RTT to the relay; shares the client's network code).
if (MSVC)
    enable_language(RC)
endif ()

add_executable(hidway-client WIN32
    src/main.c
    src/config.c
    src/net.c
    ${HIDWAY_COMMON}/keymap_sc2hid.c
    ${HIDWAY_COMMON}/hidway_crypto.c
    $<$<BOOL:${MSVC}>:app.rc>
)

add_executable(hidway-probe
    ${CMAKE_CURRENT_LIST_DIR}/../tools/probe/hidway_probe.c
    src/config.c
    src/net.c
    ${HIDWAY_COMMON}/hidway_crypto.c
)

foreach (t hidway-client hidway-probe)
    target_include_directories(${t} PRIVATE src ${HIDWAY_COMMON})
    target_link_libraries(${t} PRIVATE hidway_monocypher ws2_32 bcrypt)
    if (MSVC)
        # /utf-8: string literals (e.g. "·", "↓", "✓") are UTF-8 in the binary.
        target_compile_options(${t} PRIVATE /W4 /utf-8)
        # Standard C string/stdio functions (strcpy, fopen, ...) are used
        # deliberately and bounded; silence MSVC's _s-variant deprecations.
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
endforeach ()

target_link_libraries(hidway-client PRIVATE
    user32 gdi32 winmm dwmapi shell32 uxtheme gdiplus)
target_link_libraries(hidway-probe PRIVATE winmm)
