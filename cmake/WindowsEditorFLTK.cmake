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
    src/editor/fltk/platform/win32/settings_store.cpp
    src/editor/fltk/platform/win32/contrast_palette.cpp
    src/editor/fltk/platform/win32/folder_opener.cpp
    src/editor/fltk/platform/win32/tasks.cpp
    src/editor/fltk/platform/win32/overlay_preview.cpp
    src/editor/fltk/platform/win32/trace_relay.cpp
    src/editor/fltk/platform/win32/daemon_hub.cpp
    src/editor/fltk/platform/win32/daemon_hub_edit.cpp
    src/editor/fltk/platform/win32/daemon_hub_edit_requests.cpp
    src/editor/fltk/platform/win32/product_daemon_hub.cpp
    src/editor/fltk/platform/win32/daemon_link.cpp
    src/editor/fltk/platform/win32/automation_trace_client.cpp)
target_include_directories(layerc_editor_fltk_support PUBLIC src/editor/fltk)
target_link_libraries(layerc_editor_fltk_support PUBLIC
    layerc_editor_live layerc_editor_core layerc_editor_contrast layerc_runtime
    layerc_windows layerc_bundle Threads::Threads)
layerc_win32(layerc_editor_fltk_support)

add_library(layerc_editor_fltk STATIC
    ${LAYERC_EDITOR_FLTK_PORTABLE_GUI_SOURCES}
    src/editor/fltk/platform/win32/editor_accessibility.cpp
    src/editor/fltk/platform/win32/desktop_font.cpp
    src/editor/fltk/platform/win32/live_draft_watch.cpp
    src/editor/fltk/platform/win32/calibration_service.cpp
    src/editor/fltk/platform/win32/binding_events.cpp
    src/editor/fltk/platform/win32/hid_binding_capture.cpp
    src/editor/fltk/platform/win32/startup.cpp
    src/editor/fltk/platform/win32/window_settings.cpp
    src/editor/fltk/platform/win32/window_chrome.cpp
    src/editor/fltk/platform/win32/desktop_listener.cpp
    src/editor/fltk/platform/win32/on_window_edit.cpp)
target_include_directories(layerc_editor_fltk PUBLIC src/editor/fltk)
# Calibration types only; the Windows capture backend arrives with its service.
target_link_libraries(layerc_editor_fltk PUBLIC
    layerc_editor_fltk_support layerc_editor_fltk_uia layerc_capture_common
    PRIVATE comctl32 xinput9_1_0)
layerc_win32(layerc_editor_fltk)

if(LAYERC_BUILD_EDITOR)
    add_executable(layere_fltk WIN32
        src/editor/fltk/platform/win32/main.cpp
        resources/windows/layere_fltk.rc)
    target_link_libraries(layere_fltk PRIVATE layerc_editor_fltk)
    layerc_win32_exe(layere_fltk)
endif()
