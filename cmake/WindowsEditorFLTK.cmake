# Phase 0 trial: intentionally absent from Runtime installation/packaging.
include(cmake/LayerCFLTKVendor.cmake)

# The toolkit-side accessibility layer the Linux AT-SPI adaptor also uses.
add_library(layerc_editor_fltk_a11y STATIC
    ${LAYERC_EDITOR_FLTK_A11Y_SOURCES}
    src/editor/fltk/platform/win32/window_origin.cpp)
target_link_libraries(layerc_editor_fltk_a11y PUBLIC
    layerc_editor_accessibility layerc_fltk_vendor)
layerc_win32(layerc_editor_fltk_a11y)

add_library(layerc_editor_fltk_uia STATIC
    src/editor/fltk/a11y/uia/context.cpp
    src/editor/fltk/a11y/uia/provider.cpp
    src/editor/fltk/a11y/uia/adaptor.cpp
    src/editor/fltk/a11y/uia/events.cpp
    src/editor/fltk/a11y/uia/text_provider.cpp
    src/editor/fltk/a11y/uia/patterns.cpp
    src/editor/fltk/a11y/uia/text_range.cpp)
target_include_directories(layerc_editor_fltk_uia PRIVATE src/editor/fltk)
target_link_libraries(layerc_editor_fltk_uia PUBLIC
    layerc_editor_fltk_a11y layerc_editor_accessibility layerc_fltk_vendor
    PRIVATE uiautomationcore oleaut32 ole32 comctl32 uuid)
layerc_win32(layerc_editor_fltk_uia)

find_package(Threads REQUIRED)
add_library(layerc_editor_fltk_support STATIC
    ${LAYERC_EDITOR_FLTK_PORTABLE_SUPPORT_SOURCES}
    src/editor/fltk/platform/win32/paths.cpp
    src/editor/fltk/platform/win32/folder_opener.cpp
    src/editor/fltk/platform/win32/tasks.cpp
    src/editor/fltk/platform/win32/overlay_preview.cpp)
target_include_directories(layerc_editor_fltk_support PUBLIC src/editor/fltk)
target_link_libraries(layerc_editor_fltk_support PUBLIC
    layerc_editor_core layerc_runtime layerc_windows layerc_bundle Threads::Threads)
layerc_win32(layerc_editor_fltk_support)

add_library(layerc_editor_fltk STATIC
    ${LAYERC_EDITOR_FLTK_PORTABLE_GUI_SOURCES}
    src/editor/fltk/platform/win32/editor_accessibility.cpp
    src/editor/fltk/platform/win32/desktop_font.cpp
    src/editor/fltk/platform/win32/live_draft_watch.cpp)
target_include_directories(layerc_editor_fltk PUBLIC src/editor/fltk)
# Calibration types only; the Windows capture backend arrives with its service.
target_link_libraries(layerc_editor_fltk PUBLIC
    layerc_editor_fltk_support layerc_editor_fltk_uia layerc_capture_common)
layerc_win32(layerc_editor_fltk)

if(LAYERC_BUILD_EDITOR)
    add_executable(layere_fltk WIN32 src/editor/fltk/platform/win32/main.cpp)
    target_include_directories(layere_fltk PRIVATE src/editor)
    target_link_libraries(layere_fltk PRIVATE layerc_editor_fltk_uia layerc_windows)
    layerc_win32_exe(layere_fltk)
endif()
