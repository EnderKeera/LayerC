# Linux editor. Missing dependencies keep source/headless builds working;
# editor CI and release jobs set LAYERC_REQUIRE_EDITOR (CMakeLists.txt) so
# they cannot skip it.
macro(layerc_editor_unavailable reason)
    if(LAYERC_REQUIRE_EDITOR)
        message(FATAL_ERROR "Linux LayerE required but not buildable: ${reason}")
    endif()
    message(STATUS "Linux LayerE not built: ${reason}")
    return()
endmacro()
set(LAYERC_EDITOR_SYSROOT "" CACHE PATH
    "Optional extracted dependency root for Linux editor development")
find_program(LAYERC_FLTK_CONFIG
    NAMES "${CMAKE_LIBRARY_ARCHITECTURE}-fltk-config" fltk-config)
if(NOT LAYERC_FLTK_CONFIG)
    layerc_editor_unavailable("FLTK 1.4 development files unavailable")
endif()
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --version
    OUTPUT_VARIABLE LAYERC_FLTK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE layerc_fltk_result)
if(NOT layerc_fltk_result EQUAL 0)
    layerc_editor_unavailable("${LAYERC_FLTK_CONFIG} --version failed")
endif()
if(LAYERC_FLTK_VERSION VERSION_LESS 1.4)
    layerc_editor_unavailable("FLTK 1.4 or later required")
endif()
# Screen-reader access is part of the editor, not an optional extra. ATK and
# its AT-SPI adaptor before 2.62 crash during cleanup (at-spi2-core c4ddb104,
# 8db3f580), so older development files leave LayerE unbuilt like FLTK does.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    set(layerc_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
    if(LAYERC_EDITOR_SYSROOT)
        # The version gate must see the sysroot's packages, not the host's.
        set(layerc_sysroot_lib "${LAYERC_EDITOR_SYSROOT}/usr/lib")
        set(ENV{PKG_CONFIG_PATH} "${layerc_sysroot_lib}/${CMAKE_LIBRARY_ARCHITECTURE}/pkgconfig:${layerc_sysroot_lib}/pkgconfig:${LAYERC_EDITOR_SYSROOT}/usr/share/pkgconfig:${layerc_pkg_config_path}")
    endif()
    # FindPkgConfig caches a passed check; re-run it when the root changes.
    if(NOT "${LAYERC_EDITOR_SYSROOT}" STREQUAL "${LAYERC_ATK_CHECKED_ROOT}")
        unset(__pkg_config_checked_LAYERC_ATK CACHE)
        set(LAYERC_ATK_CHECKED_ROOT "${LAYERC_EDITOR_SYSROOT}" CACHE INTERNAL "")
    endif()
    pkg_check_modules(LAYERC_ATK QUIET "atk>=2.62" "atk-bridge-2.0>=2.62" "atspi-2>=2.62")
    set(ENV{PKG_CONFIG_PATH} "${layerc_pkg_config_path}")
endif()
if(NOT LAYERC_ATK_FOUND)
    layerc_editor_unavailable("ATK/AT-SPI 2.62 or later development files unavailable")
endif()
# The editor's own AT-SPI bridge speaks D-Bus directly, so it needs the sd-bus
# lane the Wayland modules already probe. A build without it would ship the
# editor without its accessibility connection, so it is unavailable.
if(NOT LAYERC_HAVE_SDBUS)
    layerc_editor_unavailable("sd-bus development files unavailable for the AT-SPI bridge")
endif()
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --cxxflags
    OUTPUT_VARIABLE LAYERC_FLTK_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE layerc_fltk_cxx_result)
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --ldflags
    OUTPUT_VARIABLE LAYERC_FLTK_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE layerc_fltk_ld_result)
if(NOT layerc_fltk_cxx_result EQUAL 0 OR NOT layerc_fltk_ld_result EQUAL 0)
    layerc_editor_unavailable("${LAYERC_FLTK_CONFIG} did not report its build flags")
endif()
separate_arguments(LAYERC_FLTK_CXXFLAGS UNIX_COMMAND "${LAYERC_FLTK_CXXFLAGS}")
separate_arguments(LAYERC_FLTK_LDFLAGS UNIX_COMMAND "${LAYERC_FLTK_LDFLAGS}")
if(LAYERC_EDITOR_SYSROOT)
    foreach(list_name LAYERC_FLTK_CXXFLAGS LAYERC_FLTK_LDFLAGS)
        set(relocated)
        foreach(flag IN LISTS ${list_name})
            if(flag MATCHES "^-([IL])(/.*)$")
                set(flag_kind "${CMAKE_MATCH_1}")
                set(flag_path "${CMAKE_MATCH_2}")
                if(EXISTS "${LAYERC_EDITOR_SYSROOT}${flag_path}")
                    list(APPEND relocated
                        "-${flag_kind}${LAYERC_EDITOR_SYSROOT}${flag_path}")
                endif()
            endif()
            list(APPEND relocated "${flag}")
        endforeach()
        set(${list_name} ${relocated})
    endforeach()
    list(PREPEND LAYERC_FLTK_LDFLAGS
        "-L${LAYERC_EDITOR_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
endif()
# GLib/ATK headers are system headers: project warnings must not apply.
set(LAYERC_ATK_INCLUDES)
foreach(dir IN LISTS LAYERC_ATK_INCLUDE_DIRS)
    # A compiler default root (the Flatpak SDK's pkg-config reports
    # /usr/include) must not be re-added: it would break #include_next.
    cmake_path(NORMAL_PATH dir OUTPUT_VARIABLE layerc_atk_dir)
    string(REGEX REPLACE "(.)/$" "\\1" layerc_atk_dir "${layerc_atk_dir}")
    if(layerc_atk_dir IN_LIST CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES OR
       "${LAYERC_EDITOR_SYSROOT}${layerc_atk_dir}" IN_LIST
       CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        continue()
    endif()
    if(LAYERC_EDITOR_SYSROOT AND EXISTS "${LAYERC_EDITOR_SYSROOT}${dir}")
        list(APPEND LAYERC_ATK_INCLUDES -isystem "${LAYERC_EDITOR_SYSROOT}${dir}")
    elseif(EXISTS "${dir}")
        # A sysroot may supply only ATK/AT-SPI; GLib then comes from the host.
        list(APPEND LAYERC_ATK_INCLUDES -isystem "${dir}")
    endif()
endforeach()
if(LAYERC_EDITOR_SYSROOT)
    # The sysroot's own libraries resolve through its -L directory above.
    set(LAYERC_ATK_LINK atk-bridge-2.0 atk-1.0 atspi gobject-2.0 glib-2.0)
else()
    # Full paths from the version-checked packages, including prefix installs.
    set(LAYERC_ATK_LINK ${LAYERC_ATK_LINK_LIBRARIES})
endif()
# The GUI-only library gives widget tests the real frontend without linking
# any toolkit into shared core, the daemon, or CLI tools.
add_library(layerc_editor_fltk STATIC
    src/editor/fltk/a11y/atspi/adaptor.cpp
    src/editor/fltk/a11y/atspi/events.cpp
    src/editor/fltk/a11y/atspi/loop_bus.cpp
    src/editor/fltk/a11y/atspi/mapping.cpp
    src/editor/fltk/a11y/atspi/objects.cpp
    src/editor/fltk/a11y/atspi/objects_accessible.cpp
    src/editor/fltk/a11y/atspi/objects_action_value.cpp
    src/editor/fltk/a11y/atspi/objects_component.cpp
    src/editor/fltk/a11y/atspi/objects_editable.cpp
    src/editor/fltk/a11y/atspi/objects_selection.cpp
    src/editor/fltk/a11y/atspi/objects_services.cpp
    src/editor/fltk/a11y/atspi/objects_text.cpp
    src/editor/fltk/a11y/atspi/status.cpp
    src/editor/fltk/a11y/fltk_actions.cpp
    src/editor/fltk/a11y/fltk_semantics.cpp
    src/editor/fltk/a11y/fltk_tree.cpp
    src/editor/fltk/a11y/input.cpp
    src/editor/fltk/platform/linux/window_origin.cpp
    src/editor/fltk/editor_accessibility.cpp
    src/editor/fltk/accessibility_semantics.cpp
    src/editor/fltk/accessibility_tree.cpp
    src/editor/fltk/editor_app.cpp
    src/editor/fltk/editor_style.cpp
    src/editor/fltk/editor_scroll.cpp
    src/editor/fltk/editor_popup.cpp
    src/editor/fltk/editor_color_chooser.cpp
    src/editor/fltk/editor_autosave.cpp
    src/editor/fltk/diagnostics_window.cpp
    src/editor/fltk/appearance_window.cpp
    src/editor/fltk/editor_tools.cpp
    src/editor/fltk/editor_placement.cpp
    src/editor/fltk/tasks_window.cpp
    src/editor/fltk/platform/linux/tasks.cpp
    src/editor/fltk/automation_editor.cpp
    src/editor/fltk/visual_editor.cpp
    src/editor/fltk/calibration_window.cpp
    src/editor/fltk/calibration_preview.cpp
    src/editor/fltk/editor_forms.cpp
    src/editor/fltk/editor_geometry.cpp
    src/editor/fltk/editor_import.cpp
    src/editor/fltk/cooldown_groups_window.cpp
    src/editor/fltk/export_selection_window.cpp
    src/editor/fltk/editor_commands.cpp
    src/editor/fltk/editor_pages.cpp
    src/editor/fltk/panel_item_dialog.cpp
    src/editor/fltk/panel_items_controls.cpp
    src/editor/fltk/editor_menus.cpp
    src/editor/fltk/editor_preview.cpp
    src/editor/fltk/binding_capture.cpp
    src/editor/fltk/platform/linux/binding_events.cpp
    src/editor/fltk/platform/linux/calibration_source.cpp
    src/editor/fltk/platform/linux/desktop_font.cpp
    src/editor/fltk/platform/linux/live_draft_watch.cpp)
target_include_directories(layerc_editor_fltk PRIVATE src/platform/linux)
if(LAYERC_EDITOR_SYSROOT)
    # fltk-config omits /usr/include when it is the compiler's default root.
    target_include_directories(layerc_editor_fltk SYSTEM PUBLIC
        "${LAYERC_EDITOR_SYSROOT}/usr/include")
endif()
target_compile_options(layerc_editor_fltk PUBLIC ${LAYERC_FLTK_CXXFLAGS})
# Only the adapter sees the GLib/ATK headers; it links only what it calls,
# and the loader resolves their own dependencies such as D-Bus.
set_source_files_properties(
    src/editor/fltk/editor_accessibility.cpp
    src/editor/fltk/accessibility_semantics.cpp
    src/editor/fltk/accessibility_tree.cpp PROPERTIES
    COMPILE_OPTIONS "${LAYERC_ATK_INCLUDES};${LAYERC_ATK_CFLAGS_OTHER}")
target_link_libraries(layerc_editor_fltk PUBLIC
    layerc_editor_linux layerc_editor_accessibility layerc_x11
    PkgConfig::LAYERC_SDBUS ${LAYERC_FLTK_LDFLAGS} ${LAYERC_ATK_LINK})
layerc_enable_warnings(layerc_editor_fltk)
add_executable(layere src/editor/fltk/main.cpp)
target_link_libraries(layere PRIVATE layerc_editor_fltk)
layerc_enable_warnings(layere)

# Extracted development dependencies must be runnable without environment
# overrides. BUILD_RPATH is stripped by install; no private path is shipped.
function(layerc_editor_build_runtime target)
    if(LAYERC_EDITOR_SYSROOT)
        set_target_properties(${target} PROPERTIES BUILD_RPATH
            "${LAYERC_EDITOR_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE};${LAYERC_EDITOR_SYSROOT}/usr/lib")
    endif()
endfunction()
layerc_editor_build_runtime(layere)
