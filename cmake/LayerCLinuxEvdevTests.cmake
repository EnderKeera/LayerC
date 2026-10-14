layerc_add_test(layerc_linux_evdev_input_tests
    SOURCES tests/linux_evdev_input_tests.cpp
    LINK layerc_evdev TIMEOUT 30 LABELS required headless evdev)
layerc_add_test(layerc_linux_evdev_keymap_tests
    SOURCES tests/linux_evdev_keymap_tests.cpp
    LINK layerc_evdev TIMEOUT 30 LABELS required headless evdev)
if(LAYERC_HAVE_XKBCOMMON)
    target_link_libraries(layerc_linux_evdev_input_tests PRIVATE PkgConfig::LAYERC_XKBCOMMON)
    target_link_libraries(layerc_linux_evdev_keymap_tests PRIVATE PkgConfig::LAYERC_XKBCOMMON)
endif()

# Compile the same mapper tests without xkbcommon, even on fully equipped CI.
# No product source or target is altered to simulate a missing dependency.
layerc_add_test(layerc_linux_evdev_us_keymap_tests
    SOURCES tests/linux_evdev_keymap_tests.cpp
        src/platform/linux/evdev_keymap.cpp src/platform/linux/keysym_vk.cpp
    TIMEOUT 30 LABELS required headless evdev)
target_include_directories(layerc_linux_evdev_us_keymap_tests PRIVATE src/platform/linux)

# The daemon route runs in the X11-enabled session engine; no display needed.
if(LAYERC_HAVE_X11 AND TARGET layerc_daemon_linux)
    layerc_add_test(layerc_linux_evdev_daemon_tests
        SOURCES tests/linux_evdev_daemon_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30 LABELS required headless evdev)
endif()

add_test(NAME layerc_linux_evdev_boundary
    COMMAND ${CMAKE_COMMAND} "-DLAYERC_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tools/tests/test_evdev_boundary.cmake")
set_tests_properties(layerc_linux_evdev_boundary PROPERTIES
    TIMEOUT 10 LABELS "required;headless;evdev")
