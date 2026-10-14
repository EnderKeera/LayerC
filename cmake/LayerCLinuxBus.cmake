# Bus lane (docs/Wayland.md): window reports, the GNOME overlay frame, portal
# restore tokens, and PipeWire frame conversion are pure and always built; the
# sd-bus and PipeWire transports need their development packages.
add_library(layerc_bus_model STATIC
    src/platform/linux/dbus_window_report.cpp
    src/platform/linux/gnome_bridge_frame.cpp
    src/platform/linux/portal_tokens.cpp
    src/platform/linux/pipewire_frame.cpp)
target_include_directories(layerc_bus_model PUBLIC src/platform/linux)
target_link_libraries(layerc_bus_model PUBLIC layerc_linux layerc_capture_common layerc_core)
layerc_enable_warnings(layerc_bus_model)

find_package(PkgConfig QUIET)
set(LAYERC_HAVE_SDBUS OFF)
set(LAYERC_HAVE_PIPEWIRE OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(LAYERC_SDBUS QUIET IMPORTED_TARGET libsystemd)
    if(NOT LAYERC_SDBUS_FOUND)
        pkg_check_modules(LAYERC_SDBUS QUIET IMPORTED_TARGET libelogind)
    endif()
    set(LAYERC_HAVE_SDBUS ${LAYERC_SDBUS_FOUND})
    pkg_check_modules(LAYERC_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    set(LAYERC_HAVE_PIPEWIRE ${LAYERC_PIPEWIRE_FOUND})
endif()
