add_executable(hidway_tests
    test_common.c
    ${HIDWAY_COMMON}/hidway_kbd.c
    ${HIDWAY_COMMON}/hidway_motion.c
    ${HIDWAY_COMMON}/keymap_sc2hid.c
)

target_include_directories(hidway_tests PRIVATE ${HIDWAY_COMMON})

if (MSVC)
    target_compile_options(hidway_tests PRIVATE /W4)
else ()
    target_compile_options(hidway_tests PRIVATE -Wall -Wextra)
endif ()

add_test(NAME hidway_common COMMAND hidway_tests)
