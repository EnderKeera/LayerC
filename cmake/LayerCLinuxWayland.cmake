# Transport foundation for the Wayland lane. Native rendering/tracking and
# scanner/protocol dependencies are added with those adapters, not advertised
# merely because a connection library was found.
option(LAYERC_ENABLE_WAYLAND "Build the optional Wayland connection" ON)
if(NOT LAYERC_ENABLE_WAYLAND)
    return()
endif()
find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    return()
endif()
pkg_check_modules(LAYERC_WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
if(NOT LAYERC_WAYLAND_CLIENT_FOUND)
    return()
endif()
add_library(layerc_wayland_connection STATIC
    src/platform/linux/wayland_connection.cpp)
target_include_directories(layerc_wayland_connection PUBLIC src/platform/linux)
target_link_libraries(layerc_wayland_connection PUBLIC
    layerc_runtime PkgConfig::LAYERC_WAYLAND_CLIENT)
layerc_enable_warnings(layerc_wayland_connection)
