# The pinned FLTK 1.4.5 build. Windows always uses it. Native Linux links its
# distribution's FLTK with the 1.4 floor and uses this build only as the
# source-build fallback of cmake/LinuxEditorGUI.cmake when that is missing;
# Flatpak pins the same upstream archive. No download occurs while
# configuring, building or running LayerC.
if(NOT WIN32 AND NOT LAYERC_FLTK_BUNDLED)
    message(FATAL_ERROR "The vendored FLTK build is the Linux fallback only when the system lacks FLTK 1.4")
endif()

# A function confines upstream helper variables and directory-wide flags.
function(layerc_add_fltk_vendor)
    set(disabled
            FLTK_BUILD_SHARED_LIBS FLTK_BUILD_GL FLTK_BUILD_FORMS
            FLTK_BUILD_FLUID FLTK_BUILD_FLTK_OPTIONS FLTK_BUILD_TEST
            FLTK_BUILD_EXAMPLES FLTK_BUILD_HTML_DOCS FLTK_BUILD_PDF_DOCS
            FLTK_BUILD_FLUID_DOCS)
    if(NOT WIN32)
        # The editor runs on X11 (XWayland under Wayland) until the Wayland
        # editor transition and text uses Xft, as the distribution builds do.
        # The editor links only the base library, as on Windows, so the image
        # libraries never look for system copies.
        list(APPEND disabled FLTK_BACKEND_WAYLAND FLTK_USE_PANGO
            FLTK_USE_SYSTEM_LIBJPEG FLTK_USE_SYSTEM_LIBPNG FLTK_USE_SYSTEM_ZLIB)
        set(FLTK_BACKEND_X11 ON CACHE BOOL "LayerC's pinned FLTK draws through X11" FORCE)
        set(FLTK_USE_XFT ON CACHE BOOL "LayerC's pinned FLTK uses Xft text" FORCE)
    endif()
    foreach(option IN LISTS disabled)
        set(${option} OFF CACHE BOOL "Disabled in LayerC's pinned FLTK build" FORCE)
    endforeach()
    # Upstream sources are not written for unity builds (the CI preset turns
    # them on); this function-scoped value keeps them as separate units.
    set(CMAKE_UNITY_BUILD OFF)
    # SYSTEM makes upstream includes external to MSVC/MinGW diagnostics.
    # EXCLUDE_FROM_ALL keeps upstream tools and install rules out of LayerC.
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/fltk"
        "${PROJECT_BINARY_DIR}/third_party/fltk" EXCLUDE_FROM_ALL SYSTEM)
    get_directory_property(layerc_fltk_version
        DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/fltk" DEFINITION FLTK_VERSION)
    if(NOT layerc_fltk_version VERSION_EQUAL "1.4.5")
        message(FATAL_ERROR "LayerC's pinned FLTK build must be version 1.4.5")
    endif()
endfunction()
layerc_add_fltk_vendor()

add_library(layerc_fltk_vendor INTERFACE)
target_link_libraries(layerc_fltk_vendor INTERFACE fltk::fltk)
