# Windows-only, non-shipping FLTK trial. Native Linux keeps its distribution's
# FLTK with the existing 1.4 floor; Flatpak pins the same upstream archive.
# No download occurs while configuring, building or running LayerC.
if(NOT WIN32)
    message(FATAL_ERROR "The vendored FLTK build is Windows-only")
endif()

# A function confines upstream helper variables and directory-wide flags.
function(layerc_add_fltk_vendor)
    foreach(option IN ITEMS
            FLTK_BUILD_SHARED_LIBS FLTK_BUILD_GL FLTK_BUILD_FORMS
            FLTK_BUILD_FLUID FLTK_BUILD_FLTK_OPTIONS FLTK_BUILD_TEST
            FLTK_BUILD_EXAMPLES FLTK_BUILD_HTML_DOCS FLTK_BUILD_PDF_DOCS
            FLTK_BUILD_FLUID_DOCS)
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
        message(FATAL_ERROR "LayerC's Windows FLTK build must be version 1.4.5")
    endif()
endfunction()
layerc_add_fltk_vendor()

add_library(layerc_fltk_vendor INTERFACE)
target_link_libraries(layerc_fltk_vendor INTERFACE fltk::fltk)
