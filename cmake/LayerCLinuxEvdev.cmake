# Standalone passive evdev provider. Deliberately not linked into layerd here;
# the display-adapter integrator owns opt-in/demand and event-loop wiring.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LAYERC_XKBCOMMON QUIET IMPORTED_TARGET xkbcommon)
endif()
set(LAYERC_HAVE_XKBCOMMON ${LAYERC_XKBCOMMON_FOUND})

add_library(layerc_evdev STATIC
    src/platform/linux/keysym_vk.cpp
    src/platform/linux/evdev_keymap.cpp
    src/platform/linux/evdev_input.cpp)
target_include_directories(layerc_evdev PUBLIC src/platform/linux)
target_link_libraries(layerc_evdev PUBLIC layerc_core layerc_runtime)
if(LAYERC_HAVE_XKBCOMMON)
    target_compile_definitions(layerc_evdev PUBLIC LAYERC_HAVE_XKBCOMMON=1)
    target_link_libraries(layerc_evdev PRIVATE PkgConfig::LAYERC_XKBCOMMON)
endif()
layerc_enable_warnings(layerc_evdev)
