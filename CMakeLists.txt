cmake_minimum_required(VERSION 3.28)

project(LayerC VERSION 2.6.2 LANGUAGES C CXX)

set(LAYERC_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
file(MAKE_DIRECTORY "${LAYERC_GENERATED_INCLUDE_DIR}/layerc/core")
configure_file(cmake/version.hpp.in
    "${LAYERC_GENERATED_INCLUDE_DIR}/layerc/core/version.hpp" @ONLY)

option(LAYERC_BUILD_DAEMON "Build the native layerd daemon" ON)
option(LAYERC_BUILD_EDITOR "Build the native LayerE editor" ON)
# Editor CI and release jobs set this so a missing Linux editor fails
# configuration instead of being skipped.
option(LAYERC_REQUIRE_EDITOR "Fail configuration when Linux LayerE cannot be built" OFF)
if(LAYERC_REQUIRE_EDITOR AND NOT LAYERC_BUILD_EDITOR)
    message(FATAL_ERROR "LAYERC_REQUIRE_EDITOR needs LAYERC_BUILD_EDITOR=ON")
endif()
option(LAYERC_BUILD_TESTS "Build LayerC tests" ON)
option(LAYERC_INSTALL_SYSTEMD_USER_UNIT
    "Install the optional LayerC systemd user unit (Linux; never auto-enabled)" OFF)
set(LAYERC_SYSTEMD_USER_UNIT_DIR "lib/systemd/user" CACHE STRING
    "Install directory for the LayerC user unit, relative to CMAKE_INSTALL_PREFIX")
option(LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS
    "Register tests that require an unlocked interactive Windows desktop" OFF)
option(LAYERC_MSVC_ANALYZE
    "Run MSVC static analysis for every LayerC target" OFF)

if(LAYERC_MSVC_ANALYZE AND NOT MSVC)
    message(FATAL_ERROR "LAYERC_MSVC_ANALYZE requires the MSVC compiler")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(layerc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        if(LAYERC_MSVC_ANALYZE)
            # Analyze project code only: <...> system headers are external.
            # C6553 is the Windows SDK 10.0.26100 winreg.h annotating the
            # RegOpenKeyExW value parameter, reported at every call site.
            target_compile_options(${target} PRIVATE
                /analyze /analyze:external- /external:anglebrackets
                /external:W0 /wd6553 /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Shared Win32 preprocessor contract: UNICODE everywhere, lean Windows
# headers, no min/max macros. Public so static libs propagate the contract
# to every dependent exe/test (mirrors the historical per-target block).
function(layerc_win32 target)
    target_compile_definitions(${target} PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    layerc_enable_warnings(${target})
endfunction()

# Native executables and tests need Common Controls 6 for themed controls,
# cue banners, empty-list markup, and MinGW's named subclass API imports.
# MSVC merges the dependency into its generated UAC manifest; MinGW embeds a
# complete manifest that replaces mingw-w64's default one.
function(layerc_common_controls target)
    if(MSVC)
        target_sources(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/resources/windows/common_controls.manifest")
    elseif(MINGW)
        target_sources(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/resources/windows/common_controls_mingw.rc")
    endif()
endfunction()

# Executables entered through wWinMain/wmain; MinGW links those with -municode.
function(layerc_win32_exe target)
    layerc_win32(${target})
    layerc_common_controls(${target})
    if(MINGW)
        target_link_options(${target} PRIVATE -municode)
    endif()
endfunction()

if(MINGW)
    # Ninja otherwise scales to every logical CPU; MinGW's C++ front end can
    # exhaust memory when several Win32-heavy translation units compile at
    # once. Link and resource jobs remain unconstrained.
    set_property(GLOBAL PROPERTY JOB_POOLS layerc_mingw_compile=1)
    set(CMAKE_JOB_POOL_COMPILE layerc_mingw_compile)
endif()

add_library(layerc_core STATIC
    src/core/automation_profile.cpp
    src/core/automation_value_codec.cpp
    src/core/automation_session.cpp
    src/core/automation_checkpoint.cpp
    src/core/automation_ops.cpp
    src/core/automation_runtime.cpp
    src/core/automation_runtime_effects.cpp
    src/core/automation_runtime_evaluation.cpp
    src/core/automation_trace.cpp
    src/core/automation_trace_control.cpp
    src/core/live_draft_control.cpp
    src/core/automation_validation.cpp
    src/core/automation_effects.cpp
    src/core/cooldown_engine.cpp
    src/core/cooldown_groups.cpp
    src/core/panel_presentation.cpp
    src/core/input_matcher.cpp
    src/core/hid.cpp
    src/core/hash_portable.cpp
    src/core/base64.cpp
    src/core/live_edit.cpp
    src/core/json.cpp
    src/core/json_reader.cpp
    src/core/game_match.cpp
    src/core/json_value.cpp
    src/core/utf8.cpp
    src/core/profile.cpp
    src/core/profile_validation.cpp
    src/core/profile_serialize.cpp
    src/core/panel_geometry.cpp
    src/core/runtime_instances.cpp
    src/core/visual.cpp
    src/core/pixel_evaluator.cpp
    src/core/template_prepare.cpp
    src/core/template_search.cpp
    src/core/visual_trigger_engine.cpp
    src/core/calibration.cpp
    src/core/companion.cpp
    src/core/companion_snapshot.cpp
    src/core/update_manifest.cpp
)
target_include_directories(layerc_core PUBLIC
    src/core/include "${LAYERC_GENERATED_INCLUDE_DIR}")
target_compile_features(layerc_core PUBLIC cxx_std_20)

layerc_enable_warnings(layerc_core)

# Build-time tool that writes the launch-time updater manifest; CI signs its
# output. It never ships in the install tree.
add_executable(layerc_manifest tools/update_manifest.cpp)
target_link_libraries(layerc_manifest PRIVATE layerc_core)
layerc_enable_warnings(layerc_manifest)

# Durable file write, publish, flush, and lock primitives: one TU per platform
# plus portable compositions. No dependencies, so the platform library and the
# launcher share one implementation without the launcher pulling in the rest
# of the platform layer. The Windows TU reuses two header-only platform helpers.
add_library(layerc_durable STATIC src/durable/durable_write.cpp)
target_include_directories(layerc_durable PUBLIC src/durable/include)
target_compile_features(layerc_durable PUBLIC cxx_std_20)
if(WIN32)
    target_sources(layerc_durable PRIVATE src/durable/durable_file.cpp)
    target_include_directories(layerc_durable PRIVATE
        src/platform/win32 src/platform/win32/include)
    target_compile_definitions(layerc_durable PRIVATE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    layerc_win32(layerc_durable)
else()
    target_sources(layerc_durable PRIVATE src/durable/linux/durable_file.cpp)
    layerc_enable_warnings(layerc_durable)
endif()

# Update engine for the Windows launcher. OS durability lives in
# layerc_durable, so its behavior is tested on every CI platform.
add_library(layerc_launcher_core STATIC
    src/launcher/update_engine.cpp
    src/launcher/signature.cpp)
target_include_directories(layerc_launcher_core PUBLIC src/launcher/include)
target_link_libraries(layerc_launcher_core PUBLIC layerc_core layerc_durable)
layerc_enable_warnings(layerc_launcher_core)

# The release key and source are baked into layerc.exe. The public key is
# read from packaging/windows/update-signing-key.hex when present. Without a
# key the launcher is still built and tested but omitted from the portable ZIP.
set(layerc_release_key_file
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/update-signing-key.hex")
set(layerc_default_release_key "")
if(EXISTS "${layerc_release_key_file}")
    file(READ "${layerc_release_key_file}" layerc_default_release_key)
    string(STRIP "${layerc_default_release_key}" layerc_default_release_key)
endif()
set(LAYERC_UPDATE_PUBLIC_KEY "${layerc_default_release_key}" CACHE STRING
    "Uncompressed P-256 public key (hex, 04...) that signs update manifests")
set(LAYERC_UPDATE_REPOSITORY "EnderKeera/LayerC" CACHE STRING
    "GitHub owner/repository the launcher downloads releases from")
set(LAYERC_UPDATE_CHANNEL "rolling" CACHE STRING
    "Release tag and manifest channel the launcher follows")
file(MAKE_DIRECTORY "${LAYERC_GENERATED_INCLUDE_DIR}/layerc/launcher")
configure_file(cmake/release_key.hpp.in
    "${LAYERC_GENERATED_INCLUDE_DIR}/layerc/launcher/release_key.hpp" @ONLY)

if(WIN32)
    add_library(layerc_launcher_win STATIC
        src/launcher/win32/release_client.cpp
        src/launcher/win32/install.cpp)
    target_include_directories(layerc_launcher_win PUBLIC src/launcher/win32)
    target_link_libraries(layerc_launcher_win PUBLIC
        layerc_launcher_core bcrypt winhttp advapi32 ole32 shell32 uuid)
    # BCryptHash is declared only for Windows 10 targets (mingw-w64 headers).
    target_compile_definitions(layerc_launcher_win PUBLIC
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    layerc_win32(layerc_launcher_win)

    # Built as layerc.exe. Only this program links an HTTP client.
    add_executable(layerc_launcher WIN32 src/launcher/win32/main.cpp)
    set_target_properties(layerc_launcher PROPERTIES OUTPUT_NAME layerc)
    target_link_libraries(layerc_launcher PRIVATE layerc_launcher_win user32)
    layerc_win32_exe(layerc_launcher)
endif()

add_library(layerc_runtime STATIC
    src/runtime/daemon_kernel.cpp
    src/runtime/diagnostics_parse.cpp
    src/runtime/diagnostics_serialize.cpp
    src/runtime/diagnostics_linux.cpp
    src/runtime/runtime_services.cpp
)
target_include_directories(layerc_runtime PUBLIC src/runtime/include)
target_link_libraries(layerc_runtime PUBLIC layerc_core)
layerc_enable_warnings(layerc_runtime)

add_library(layerc_capture_common STATIC
    src/capture/capture.cpp
    src/capture/visual_evaluation.cpp
)
target_include_directories(layerc_capture_common PUBLIC
    src/capture/include
)
target_link_libraries(layerc_capture_common PUBLIC layerc_core)
layerc_enable_warnings(layerc_capture_common)

add_library(layerc_miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)
target_include_directories(layerc_miniz PUBLIC
    third_party/miniz)
target_compile_definitions(layerc_miniz PUBLIC
    MINIZ_NO_ZLIB_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(layerc_miniz PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

add_library(layerc_bundle STATIC
    src/bundle/pack_detail.cpp
    src/bundle/pack_import.cpp
    src/bundle/pack_validate.cpp
    src/bundle/profile_pack.cpp
)
target_include_directories(layerc_bundle PUBLIC src/bundle/include)
target_link_libraries(layerc_bundle PUBLIC layerc_core)
target_link_libraries(layerc_bundle PRIVATE layerc_miniz)
# Warnings for the bundle's own TUs live here, once, so the platform
# branches below only add the platform link plus the Win32 contract.
layerc_enable_warnings(layerc_bundle)

if(WIN32)
    add_library(layerc_windows STATIC
        src/platform/win32/settings_store.cpp
        src/platform/win32/editor_appearance.cpp
        src/platform/win32/window_query.cpp
        src/platform/win32/win32_error.cpp
        src/platform/win32/platform_events.cpp
        src/platform/win32/native_theme.cpp
        src/platform/win32/native_controls.cpp
        src/platform/win32/window_discovery.cpp
        src/platform/text.cpp
        src/platform/win32/path_text.cpp
        src/platform/win32/profile_store.cpp
        src/platform/profile_store_data.cpp
        src/platform/automation_persistence.cpp
        src/platform/profile_store_layout.cpp
        src/platform/profile_bundle_load.cpp
        src/platform/win32/profile_file_guard.cpp
        src/platform/profile_transaction.cpp
        src/platform/win32/visual_assets.cpp
        src/platform/visual_asset_load.cpp
        src/platform/template_png.cpp
        src/platform/win32/live_channel.cpp
        src/platform/win32/framed_pipe.cpp
        src/platform/win32/bounded_helper.cpp
        src/platform/win32/hid_device.cpp
        src/platform/win32/hid_device_inference.cpp
    )
    target_include_directories(layerc_windows PUBLIC
        src/platform/include src/platform/win32/include)
    target_link_libraries(layerc_windows PUBLIC
        layerc_core layerc_durable user32 advapi32 shell32 ole32 gdi32 shcore
        bcrypt hid comctl32 dwmapi uxtheme)
    # miniz serves the shared template-asset PNG codec.
    target_link_libraries(layerc_windows PRIVATE layerc_miniz)
    target_compile_definitions(layerc_windows PUBLIC
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    layerc_win32(layerc_windows)

    add_library(layerc_capture STATIC
        src/capture/window_capture.cpp
        src/capture/static_window_snapshot.cpp
        src/capture/d3d_capture_support.cpp
        src/capture/wgc_backend.cpp
        src/capture/desktop_duplication_backend.cpp
    )
    target_include_directories(layerc_capture PRIVATE src/capture)
    target_link_libraries(layerc_capture PUBLIC
        layerc_capture_common layerc_windows)
    target_link_libraries(layerc_capture PRIVATE
        d3d11 dxgi dwmapi user32 gdi32)
    layerc_win32(layerc_capture)

    target_link_libraries(layerc_bundle PUBLIC layerc_windows)
    # layerc_win32 re-applies warnings (already set above) to propagate the
    # UNICODE/lean-headers contract to every dependent exe and test.
    layerc_win32(layerc_bundle)
endif()

if(WIN32 AND (LAYERC_BUILD_DAEMON OR LAYERC_BUILD_TESTS))
    add_library(layerc_overlay STATIC
        src/daemon/overlay_window.cpp
        src/daemon/overlay_render.cpp
        src/daemon/overlay_collection.cpp)
    target_include_directories(layerc_overlay PUBLIC src/daemon)
    target_link_libraries(layerc_overlay PUBLIC layerc_core layerc_windows
        PRIVATE user32 gdi32)
    layerc_win32(layerc_overlay)

    add_library(layerc_isolated STATIC
        src/daemon/isolated_visual_protocol.cpp
        src/daemon/visual_capture_worker.cpp)
    target_include_directories(layerc_isolated PUBLIC src/daemon)
    target_link_libraries(layerc_isolated PUBLIC
        layerc_core layerc_windows layerc_capture)
    layerc_win32(layerc_isolated)

    # C++/WinRT is MSVC-only; MinGW links an inert client that reports the
    # App Service as unavailable.
    if(MSVC)
        set(layerc_gamebar_client src/gamebar/GameBarAppServiceClient.cpp)
    else()
        set(layerc_gamebar_client src/gamebar/GameBarAppServiceClient_stub.cpp)
    endif()
    add_library(layerc_gamebar STATIC
        src/gamebar/GameBarTransport.cpp ${layerc_gamebar_client})
    target_include_directories(layerc_gamebar PUBLIC src/gamebar)
    target_link_libraries(layerc_gamebar PUBLIC layerc_core layerc_windows)
    layerc_win32(layerc_gamebar)
    if(MSVC)
        target_link_libraries(layerc_gamebar PUBLIC runtimeobject)
    endif()
endif()

if(WIN32 AND (LAYERC_BUILD_EDITOR OR LAYERC_BUILD_TESTS))
    add_library(layerc_editor_ui STATIC
        src/editor/editor_dialog.cpp
        src/editor/editor_layout.cpp
        src/editor/editor_shell.cpp
        src/editor/editor_prompt.cpp
        src/editor/editor_profiles.cpp
        src/editor/editor_profile_fields.cpp
        src/editor/editor_util.cpp
        src/editor/editor_cooldowns.cpp
        src/editor/editor_bars.cpp
        src/editor/editor_tasks.cpp
        src/editor/editor_theme.cpp
        src/editor/editor_draw.cpp
        src/editor/editor_appearance.cpp
        src/editor/editor_color_picker.cpp
        src/editor/editor_live_edit.cpp
        src/editor/editor_visual.cpp
        src/editor/calibration_session.cpp
        src/editor/editor_automation.cpp
        src/editor/editor_automation_layout.cpp
        src/editor/editor_automation_nodes.cpp
        src/editor/editor_automation_simulator.cpp
        src/editor/editor_packs.cpp
        src/editor/editor_diagnostics.cpp
        src/editor/editor_hid.cpp)
    target_include_directories(layerc_editor_ui PUBLIC src/editor)
    target_link_libraries(layerc_editor_ui PUBLIC
        layerc_editor_core layerc_core layerc_runtime layerc_windows layerc_bundle
        comctl32 comdlg32 dwmapi shell32 uxtheme user32 gdi32)
    layerc_win32(layerc_editor_ui)

    add_library(layerc_calibration_ui STATIC
        src/calibration/calibration_mode.cpp
        src/calibration/calibration_window.cpp
        src/calibration/calibration_preview.cpp
        src/calibration/calibration_capture.cpp
        src/calibration/calibration_templates.cpp)
    target_include_directories(layerc_calibration_ui PUBLIC src/calibration)
    target_link_libraries(layerc_calibration_ui PUBLIC
        layerc_core layerc_windows layerc_capture
        comctl32 dwmapi shell32 uxtheme user32 gdi32)
    layerc_win32(layerc_calibration_ui)
endif()

if(NOT WIN32)
    add_library(layerc_linux STATIC
        src/platform/linux/profile_store.cpp
        src/platform/profile_store_data.cpp
        src/platform/automation_persistence.cpp
        src/platform/profile_store_layout.cpp
        src/platform/profile_bundle_load.cpp
        src/platform/linux/profile_file_guard.cpp
        src/platform/profile_transaction.cpp
        src/platform/text.cpp
        src/platform/linux/path_text.cpp
        src/platform/linux/visual_assets.cpp
        src/platform/visual_asset_load.cpp
        src/platform/template_png.cpp
        src/platform/linux/event_loop.cpp
        src/platform/linux/control_channel.cpp
    )
    target_include_directories(layerc_linux PUBLIC
        src/platform/include src/platform/linux/include)
    target_include_directories(layerc_linux PRIVATE src/platform/linux)
    target_link_libraries(layerc_linux PUBLIC layerc_core layerc_durable)
    # miniz serves the shared template-asset PNG codec; the daemon consumes
    # only verified images through the visual-asset contract.
    target_link_libraries(layerc_linux PRIVATE layerc_miniz)
    layerc_enable_warnings(layerc_linux)

    target_link_libraries(layerc_bundle PUBLIC layerc_linux)

    # X11 adapters. Without the X11 development packages the daemon
    # builds without input/overlay/capture and reports those capabilities as
    # unavailable, so a container without X stays configurable and green.
    # Xtst is only used to inject events in the input test, never by layerd.
    find_package(X11 COMPONENTS
        Xext Xi Xrandr Xcomposite Xdamage Xfixes
        OPTIONAL_COMPONENTS Xtst)
    set(LAYERC_HAVE_X11 ${X11_FOUND})
    if(LAYERC_HAVE_X11 AND NOT (X11_Xext_FOUND AND X11_Xi_FOUND AND
                                X11_Xrandr_FOUND AND X11_Xcomposite_FOUND AND
                                X11_Xdamage_FOUND AND X11_Xfixes_FOUND))
      message(STATUS "X11 found but required adapter extensions are incomplete; "
                     "building without X11 adapters")
      set(LAYERC_HAVE_X11 OFF)
    endif()
    if(LAYERC_HAVE_X11)
        add_library(layerc_x11 STATIC
            src/platform/linux/x11_capture.cpp
            src/platform/linux/x11_display.cpp
            src/platform/linux/x11_input.cpp
            src/platform/linux/x11_monitor.cpp
            src/platform/linux/x11_overlay.cpp
            src/platform/linux/x11_window.cpp
        )
        target_include_directories(layerc_x11 PUBLIC
            ${X11_INCLUDE_DIR}
            src/platform/include)
        target_include_directories(layerc_x11 PRIVATE src/platform/linux)
        target_link_libraries(layerc_x11 PUBLIC
            layerc_capture_common layerc_core layerc_runtime ${X11_LIBRARIES}
            ${X11_Xext_LIB} ${X11_Xi_LIB} ${X11_Xrandr_LIB}
            ${X11_Xcomposite_LIB} ${X11_Xdamage_LIB} ${X11_Xfixes_LIB})
        layerc_enable_warnings(layerc_x11)

        add_executable(layercal src/calibration/linux_main.cpp)
        target_include_directories(layercal PRIVATE src/platform/linux)
        target_link_libraries(layercal PRIVATE layerc_x11)
        layerc_enable_warnings(layercal)
    endif()

    if(LAYERC_BUILD_DAEMON OR LAYERC_BUILD_TESTS)
        add_library(layerc_daemon_linux STATIC
            src/daemon/linux/daemon_host.cpp
            src/daemon/linux/live_draft.cpp)
        target_include_directories(layerc_daemon_linux PUBLIC
            src/daemon/linux
            src/platform/linux)
        target_link_libraries(layerc_daemon_linux PUBLIC
            layerc_runtime layerc_linux)
        if(LAYERC_HAVE_X11)
            target_sources(layerc_daemon_linux PRIVATE
                src/daemon/linux/input_service.cpp
                src/daemon/linux/automation_trace.cpp
                src/daemon/linux/overlay_service.cpp
                src/daemon/linux/visual_service.cpp)
            target_link_libraries(layerc_daemon_linux PUBLIC layerc_x11)
        endif()
        layerc_enable_warnings(layerc_daemon_linux)
    endif()

    if(LAYERC_BUILD_DAEMON)
        add_executable(layerd
            src/daemon/linux/main.cpp
            src/daemon/linux/x11_adapters.cpp)
        target_link_libraries(layerd PRIVATE layerc_daemon_linux)
        if(NOT LAYERC_HAVE_X11)
            target_compile_definitions(layerd PRIVATE LAYERC_NO_X11)
        endif()
        layerc_enable_warnings(layerd)

        add_executable(layerctl
            src/ctl/layerctl.cpp
        )
        target_include_directories(layerctl PRIVATE
            src/platform/linux)
        target_link_libraries(layerctl PRIVATE layerc_bundle layerc_runtime)
        layerc_enable_warnings(layerctl)
    endif()
endif()

# Portable LayerE behavior shared by every editor frontend: the document,
# history, commit pipeline, and profile/automation helpers. It has no toolkit
# or platform headers; the platform library supplies the profile store.
if(LAYERC_BUILD_EDITOR OR LAYERC_BUILD_TESTS)
    add_library(layerc_editor_core STATIC
        src/editor/common/automation_graph.cpp
        src/editor/common/automation_presets.cpp
        src/editor/common/automation_simulator.cpp
        src/editor/common/document_commit.cpp
        src/editor/common/profile_document.cpp
        src/editor/common/profile_edit.cpp
        src/editor/common/runtime_diagnostics_view.cpp
        src/editor/common/visual_assignment.cpp
        src/editor/common/visual_calibration.cpp)
    target_include_directories(layerc_editor_core PUBLIC src/editor/common)
    target_link_libraries(layerc_editor_core PUBLIC layerc_runtime)
    if(WIN32)
        target_link_libraries(layerc_editor_core PUBLIC layerc_core layerc_windows)
        layerc_win32(layerc_editor_core)
    else()
        target_link_libraries(layerc_editor_core PUBLIC layerc_core layerc_linux)
        layerc_enable_warnings(layerc_editor_core)

        # Linux editor services shared by the frontend and its tests: the
        # authenticated control client for daemon status and reload.
        add_library(layerc_editor_linux STATIC
            src/editor/linux/daemon_link.cpp
            src/editor/linux/automation_trace_client.cpp
            src/editor/linux/live_draft_client.cpp
            src/editor/linux/editor_model.cpp
            src/editor/linux/editor_session.cpp
            src/editor/linux/profile_mutations.cpp
            src/editor/linux/editor_appearance.cpp
            src/editor/linux/task_list.cpp)
        target_include_directories(layerc_editor_linux PUBLIC src/editor/linux)
        target_include_directories(layerc_editor_linux PRIVATE src/platform/linux)
        find_package(Threads REQUIRED)
        target_link_libraries(layerc_editor_linux PUBLIC
            layerc_editor_core layerc_runtime layerc_linux layerc_bundle Threads::Threads)
        layerc_enable_warnings(layerc_editor_linux)
        if(LAYERC_BUILD_EDITOR AND LAYERC_HAVE_X11)
            include(cmake/LinuxEditorGUI.cmake)
        elseif(LAYERC_REQUIRE_EDITOR)
            message(FATAL_ERROR "Linux LayerE required but not buildable: X11 development files were not found")
        endif()
    endif()
endif()

# The Windows services are independent of the shell and are linked unchanged
# by both layerd and its direct-owner tests.
if(WIN32 AND (LAYERC_BUILD_DAEMON OR LAYERC_BUILD_TESTS))
    add_library(layerc_daemon_windows STATIC
        src/daemon/daemon_state.cpp
        src/daemon/daemon_automation.cpp
        src/daemon/daemon_sessions.cpp
        src/daemon/session_coordinator.cpp
        src/daemon/daemon_input.cpp
        src/daemon/daemon_hid.cpp
        src/daemon/hid_resolver.cpp
        src/daemon/daemon_overlays.cpp
        src/daemon/daemon_companion.cpp
        src/daemon/daemon_live_edit.cpp
        src/daemon/daemon_visual.cpp
        src/daemon/isolated_visual_worker.cpp)
    target_include_directories(layerc_daemon_windows PUBLIC src/daemon)
    target_link_libraries(layerc_daemon_windows PUBLIC
        layerc_core layerc_runtime layerc_windows layerc_capture
        layerc_overlay layerc_isolated layerc_gamebar
        user32 gdi32 xinput9_1_0 psapi hid bcrypt)
    layerc_win32(layerc_daemon_windows)
endif()

if(WIN32 AND LAYERC_BUILD_DAEMON)
    add_executable(layerd WIN32
        src/daemon/main.cpp
        src/daemon/daemon_app.cpp
        src/daemon/daemon_services.cpp
        src/daemon/daemon_diagnostics.cpp
        resources/windows/layerd.rc)
    target_link_libraries(layerd PRIVATE layerc_daemon_windows shell32)
    layerc_win32_exe(layerd)
    if(MSVC)
        target_link_libraries(layerd PRIVATE delayimp)
        target_link_options(layerd PRIVATE /DELAYLOAD:xinput9_1_0.dll)
    endif()

    add_executable(layerw WIN32 src/worker/main.cpp)
    target_include_directories(layerw PRIVATE src/daemon)
    target_link_libraries(layerw PRIVATE
        layerc_isolated shell32 user32 gdi32)
    layerc_win32_exe(layerw)
endif()

option(LAYERC_BUILD_GAMEBAR_HOST "Build the C++/WinRT Game Bar companion host" ON)
if(WIN32 AND MSVC AND LAYERC_BUILD_GAMEBAR_HOST)
    include(cmake/GameBarWidget.cmake)
endif()

if(WIN32 AND LAYERC_BUILD_EDITOR)
    add_executable(layere WIN32
        src/editor/main.cpp
        src/editor/editor_resources.rc)
    target_include_directories(layere PRIVATE src/editor)
    target_link_libraries(layere PRIVATE layerc_editor_ui)
    layerc_win32_exe(layere)

    add_executable(layercal WIN32
        src/calibration/main.cpp
        src/calibration/calibration_resources.rc)
    target_include_directories(layercal PRIVATE src/calibration)
    target_link_libraries(layercal PRIVATE layerc_calibration_ui)
    layerc_win32_exe(layercal)
endif()

set(LAYERC_MINGW_RUNTIME_DLLS)
if(MINGW)
    get_filename_component(LAYERC_MINGW_BIN_DIR
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll IN ITEMS
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            libwinpthread-1.dll)
        string(MAKE_C_IDENTIFIER "${runtime_dll}" runtime_dll_id)
        find_file(LAYERC_${runtime_dll_id}_PATH
            NAMES ${runtime_dll}
            PATHS "${LAYERC_MINGW_BIN_DIR}"
            NO_DEFAULT_PATH)
        if(LAYERC_${runtime_dll_id}_PATH)
            list(APPEND LAYERC_MINGW_RUNTIME_DLLS
                "${LAYERC_${runtime_dll_id}_PATH}")
        endif()
    endforeach()
endif()

if(TARGET layerc_gamebar_host)
    find_program(LAYERC_MAKEAPPX makeappx.exe
        HINTS "${LAYERC_WINSDK_ROOT}/bin/${LAYERC_WINSDK_VER}/x64")
    file(GLOB LAYERC_MSVC_REDIST_DIRS LIST_DIRECTORIES true
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT"
        "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
    list(SORT LAYERC_MSVC_REDIST_DIRS ORDER DESCENDING)
    set(LAYERC_MSVC_REDIST_DIR)
    foreach(_redist IN LISTS LAYERC_MSVC_REDIST_DIRS)
        if(EXISTS "${_redist}/msvcp140.dll" AND
           EXISTS "${_redist}/vcruntime140.dll" AND
           EXISTS "${_redist}/vcruntime140_1.dll")
            set(LAYERC_MSVC_REDIST_DIR "${_redist}")
            break()
        endif()
    endforeach()
    if(NOT LAYERC_MAKEAPPX)
        message(STATUS "makeappx not found; unsigned Game Bar MSIX target is not configured")
    elseif(NOT LAYERC_MSVC_REDIST_DIR)
        message(STATUS "Game Bar MSIX target disabled: x64 MSVC runtime redistributables were not found")
    elseif(NOT TARGET layerd OR NOT TARGET layere OR
           NOT TARGET layercal OR NOT TARGET layerw)
        message(STATUS "Game Bar MSIX target disabled: full package requires layerd, layere, layercal, and layerw")
    else()
        set(LAYERC_GAMEBAR_PACKAGE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/gamebar-package/$<CONFIG>")
        set(LAYERC_GAMEBAR_MSIX_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/gamebar-msix/$<CONFIG>")
        set(LAYERC_GAMEBAR_MSIX
            "${LAYERC_GAMEBAR_MSIX_DIR}/LayerC.GameBar.v1.msix")
        add_custom_target(layerc_gamebar_msix
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${LAYERC_GAMEBAR_MSIX_DIR}"
            COMMAND "${LAYERC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/StageGameBarPackage.ps1"
                -WidgetOutput "${LAYERC_GAMEBAR_WIDGET_OUTPUT_DIR}"
                -Destination "${LAYERC_GAMEBAR_PACKAGE_DIR}"
                -Manifest "${LAYERC_GAMEBAR_MANIFEST}"
                -Asset "${LAYERC_GAMEBAR_WIDGET_ASSET}"
                -GameBarPrivateWinmd "${LAYERC_GAMEBAR_NUGET_PACKAGES}/Microsoft.Gaming.XboxGameBar.7.3.2607010/private/Microsoft.Gaming.XboxGameBar.Private.winmd"
                -DesktopRuntimeDirectory "${LAYERC_MSVC_REDIST_DIR}"
                -LayerD "$<TARGET_FILE:layerd>"
                -LayerE "$<TARGET_FILE:layere>"
                -LayerCal "$<TARGET_FILE:layercal>"
                -LayerW "$<TARGET_FILE:layerw>"
            COMMAND "${LAYERC_MAKEAPPX}" pack
                /d "${LAYERC_GAMEBAR_PACKAGE_DIR}"
                /p "${LAYERC_GAMEBAR_MSIX}" /o
            BYPRODUCTS "${LAYERC_GAMEBAR_MSIX}"
            DEPENDS layerc_gamebar_host layerd layere layercal layerw
                "${LAYERC_GAMEBAR_MANIFEST}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/StageGameBarPackage.ps1"
            VERBATIM)
    endif()
endif()

if(WIN32)
    foreach(layerc_runtime_target IN ITEMS layerc_launcher layerd layere layercal layerw)
        # A keyless launcher cannot bootstrap a first build. Portable ZIPs
        # start via layerd.exe until a release key is provisioned.
        if(layerc_runtime_target STREQUAL "layerc_launcher" AND
           "${LAYERC_UPDATE_PUBLIC_KEY}" STREQUAL "")
            continue()
        endif()
        if(TARGET ${layerc_runtime_target})
            install(TARGETS ${layerc_runtime_target}
                RUNTIME DESTINATION . COMPONENT Runtime)
        endif()
    endforeach()
    # The portable ZIP intentionally remains desktop-only. Game Bar package
    # inputs are staged only by the explicit layerc_gamebar_msix target.
    install(FILES README.md DESTINATION . COMPONENT Runtime)
    set(layerc_notice_dir .)
    set(layerc_miniz_notice_dir licenses/miniz)

    if(MINGW AND LAYERC_MINGW_RUNTIME_DLLS)
        install(FILES ${LAYERC_MINGW_RUNTIME_DLLS}
            DESTINATION . COMPONENT Runtime)
    endif()
else()
    # GNUInstallDirs layout for the Linux foreground host and CLIs.
    # flatpak/"make install" consumers rely on these alternate commands.
    include(GNUInstallDirs)
    foreach(layerc_linux_target IN ITEMS layerd layerctl layercal layere)
        if(TARGET ${layerc_linux_target})
            install(TARGETS ${layerc_linux_target}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
        endif()
    endforeach()
    if(TARGET layere)
        # The editor makes the install a desktop application: one launcher,
        # its icon, and the desktop-application form of the same component.
        install(FILES packaging/linux/io.github.EnderKeera.LayerC.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT Runtime)
        foreach(layerc_icon_size IN ITEMS 48 64 128 256)
            set(layerc_icon_dir "${layerc_icon_size}x${layerc_icon_size}")
            install(FILES
                packaging/linux/icons/${layerc_icon_dir}/io.github.EnderKeera.LayerC.png
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${layerc_icon_dir}/apps
                COMPONENT Runtime)
        endforeach()
        install(FILES packaging/linux/io.github.EnderKeera.LayerC.editor.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
            RENAME io.github.EnderKeera.LayerC.metainfo.xml COMPONENT Runtime)
    elseif(TARGET layerd)
        install(FILES
            packaging/flatpak/io.github.EnderKeera.LayerC.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT Runtime)
    endif()

    if(LAYERC_INSTALL_SYSTEMD_USER_UNIT)
        set(LAYERC_SYSTEMD_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/io.github.EnderKeera.LayerC.service.in"
            "${CMAKE_CURRENT_BINARY_DIR}/io.github.EnderKeera.LayerC.service"
            @ONLY NEWLINE_STYLE LF)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.EnderKeera.LayerC.service"
            DESTINATION "${LAYERC_SYSTEMD_USER_UNIT_DIR}" COMPONENT Runtime)
    endif()
    set(layerc_notice_dir ${CMAKE_INSTALL_DATADIR}/licenses/LayerC)
    set(layerc_miniz_notice_dir ${layerc_notice_dir}/miniz)
endif()
# Every installed program statically links miniz (through layerc_bundle or the
# Linux template decoder), so every install carries its MIT notice.
install(FILES THIRD_PARTY_NOTICES.md
    DESTINATION ${layerc_notice_dir} COMPONENT Runtime)
install(FILES third_party/miniz/LICENSE third_party/miniz/UPSTREAM.md
    DESTINATION ${layerc_miniz_notice_dir} COMPONENT Runtime)

if(WIN32)
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "LayerC")
set(CPACK_PACKAGE_VENDOR "LayerC")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "LayerC-${PROJECT_VERSION}-windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_MONOLITHIC_INSTALL ON)
include(CPack)
else()
# Native Debian package. Configure it with CMAKE_INSTALL_PREFIX=/usr; the
# optional user unit embeds the configured binary path, so the package installs
# under that same prefix. dpkg-shlibdeps derives the library dependencies, and
# the editor adds the AT-SPI floor its accessibility bridge needs (Debian 13:
# trixie-backports).
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "layerc")
set(CPACK_PACKAGE_VENDOR "LayerC")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "EnderKeera <EnderKeera@users.noreply.github.com>")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_PACKAGE_DESCRIPTION
    "LayerC runs a passive, click-through cooldown overlay on X11 from the keyboard and mouse input it observes; it never injects input or reads game memory. layerctl and layercal manage profiles, bundles and calibration.")
if(TARGET layere)
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Passive X11 cooldown overlay, command-line tools and LayerE editor")
    string(APPEND CPACK_PACKAGE_DESCRIPTION
        " The LayerE editor creates and edits profiles, cooldowns, bindings and panels and asks the daemon to reload them.")
else()
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Passive X11 cooldown overlay and command-line tools")
endif()
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_STRIP_FILES ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/EnderKeera/LayerC")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
if(TARGET layere)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libatk1.0-0t64 (>= 2.62), libatk-bridge2.0-0t64 (>= 2.62), libatspi2.0-0t64 (>= 2.62)")
endif()
include(CPack)
endif()

if(LAYERC_BUILD_TESTS)
    include(cmake/LayerCTests.cmake)
endif()
