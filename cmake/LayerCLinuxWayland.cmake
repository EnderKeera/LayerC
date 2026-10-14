# Wayland lane. The transport (layerc_wayland_connection) needs only
# wayland-client; the native providers (outputs, foreign toplevels, layer-shell
# overlay) additionally need the scanner, protocols, cairo and xkbcommon and
# are advertised only when all of them are found (LAYERC_HAVE_WAYLAND).
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

pkg_check_modules(LAYERC_WAYLAND_NATIVE QUIET IMPORTED_TARGET cairo xkbcommon)
pkg_check_modules(LAYERC_WAYLAND_PROTOCOLS QUIET wayland-protocols)
pkg_check_modules(LAYERC_WAYLAND_SCANNER QUIET wayland-scanner)
if(NOT LAYERC_WAYLAND_NATIVE_FOUND OR NOT LAYERC_WAYLAND_PROTOCOLS_FOUND
        OR NOT LAYERC_WAYLAND_SCANNER_FOUND)
    return()
endif()
pkg_get_variable(LAYERC_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(LAYERC_WAYLAND_SCANNER_BIN wayland-scanner wayland_scanner)
if(NOT LAYERC_WAYLAND_SCANNER_BIN)
    find_program(LAYERC_WAYLAND_SCANNER_BIN wayland-scanner)
endif()
if(NOT LAYERC_WAYLAND_PROTOCOLS_DIR OR NOT LAYERC_WAYLAND_SCANNER_BIN)
    return()
endif()

set(LAYERC_WAYLAND_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
file(MAKE_DIRECTORY "${LAYERC_WAYLAND_GENERATED_DIR}")

# Generates client glue for one protocol XML into <output>-client-protocol.h
# and <output>-protocol.c; appends the C source to <sources_var>.
function(layerc_wayland_protocol sources_var xml output)
    set(header "${LAYERC_WAYLAND_GENERATED_DIR}/${output}-client-protocol.h")
    set(code "${LAYERC_WAYLAND_GENERATED_DIR}/${output}-protocol.c")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${LAYERC_WAYLAND_SCANNER_BIN}" client-header "${xml}" "${header}"
        DEPENDS "${xml}" VERBATIM)
    add_custom_command(OUTPUT "${code}"
        COMMAND "${LAYERC_WAYLAND_SCANNER_BIN}" private-code "${xml}" "${code}"
        DEPENDS "${xml}" VERBATIM)
    set(${sources_var} ${${sources_var}} "${header}" "${code}" PARENT_SCOPE)
endfunction()

set(layerc_wayland_protocol_sources)
layerc_wayland_protocol(layerc_wayland_protocol_sources
    "${LAYERC_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml" xdg-shell)
layerc_wayland_protocol(layerc_wayland_protocol_sources
    "${LAYERC_WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml"
    xdg-output-unstable-v1)
layerc_wayland_protocol(layerc_wayland_protocol_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland-protocols/wlr-layer-shell-unstable-v1.xml"
    wlr-layer-shell-unstable-v1)
layerc_wayland_protocol(layerc_wayland_protocol_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland-protocols/wlr-foreign-toplevel-management-unstable-v1.xml"
    wlr-foreign-toplevel-management-unstable-v1)
# Generated glue is not project code: no project warnings, and its headers are
# SYSTEM so their C casts do not warn in C++ translation units.
add_library(layerc_wayland_protocols STATIC ${layerc_wayland_protocol_sources})
target_include_directories(layerc_wayland_protocols SYSTEM PUBLIC
    "${LAYERC_WAYLAND_GENERATED_DIR}")
target_link_libraries(layerc_wayland_protocols PUBLIC PkgConfig::LAYERC_WAYLAND_CLIENT)

add_library(layerc_wayland STATIC
    src/platform/linux/wayland_monitors.cpp
    src/platform/linux/wayland_toplevels.cpp
    src/platform/linux/wayland_overlay.cpp)
target_include_directories(layerc_wayland PUBLIC src/platform/linux)
target_link_libraries(layerc_wayland PUBLIC
    layerc_wayland_connection layerc_wayland_protocols layerc_linux
    PkgConfig::LAYERC_WAYLAND_NATIVE)
target_compile_definitions(layerc_wayland PUBLIC LAYERC_HAVE_WAYLAND)
layerc_enable_warnings(layerc_wayland)
