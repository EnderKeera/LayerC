# Phase 0 trial: intentionally absent from Runtime installation/packaging.
include(cmake/LayerCFLTKVendor.cmake)

add_library(layerc_editor_fltk_uia STATIC
    src/editor/fltk/a11y/input.cpp
    src/editor/fltk/platform/win32/window_origin.cpp
    src/editor/fltk/a11y/uia/context.cpp
    src/editor/fltk/a11y/uia/provider.cpp
    src/editor/fltk/a11y/uia/bridge.cpp
    src/editor/fltk/a11y/uia/events.cpp
    src/editor/fltk/a11y/uia/text_provider.cpp
    src/editor/fltk/a11y/uia/text_range.cpp)
target_include_directories(layerc_editor_fltk_uia PRIVATE src/editor/fltk)
target_link_libraries(layerc_editor_fltk_uia PUBLIC
    layerc_editor_accessibility layerc_fltk_vendor
    PRIVATE uiautomationcore oleaut32 ole32 comctl32 uuid)
layerc_win32(layerc_editor_fltk_uia)

if(LAYERC_BUILD_EDITOR)
    add_executable(layere_fltk WIN32 src/editor/fltk/platform/win32/main.cpp)
    target_include_directories(layere_fltk PRIVATE src/editor)
    target_link_libraries(layere_fltk PRIVATE layerc_editor_fltk_uia layerc_windows)
    layerc_win32_exe(layere_fltk)
endif()
