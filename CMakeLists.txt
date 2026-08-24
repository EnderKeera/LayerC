cmake_minimum_required(VERSION 3.28)

project(LayerC VERSION 2.4.0 LANGUAGES C CXX)

option(LAYERC_BUILD_DAEMON "Build the native layerd daemon" ON)
option(LAYERC_BUILD_EDITOR "Build the native LayerE editor" ON)
option(LAYERC_BUILD_TESTS "Build LayerC tests" ON)
option(LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS
    "Register tests that require an unlocked interactive Windows desktop" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(layerc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic)
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
    src/core/automation_runtime.cpp
    src/core/automation_trace.cpp
    src/core/automation_validation.cpp
    src/core/automation_effects.cpp
    src/core/cooldown_engine.cpp
    src/core/input_matcher.cpp
    src/core/hid.cpp
    src/core/hash.cpp
    src/core/live_edit.cpp
    src/core/json.cpp
    src/core/json_value.cpp
    src/core/utf8.cpp
    src/core/profile.cpp
    src/core/profile_fields.cpp
    src/core/profile_serialize.cpp
    src/core/runtime_instances.cpp
    src/core/visual.cpp
    src/core/pixel_evaluator.cpp
    src/core/template_matcher.cpp
    src/core/visual_trigger_engine.cpp
    src/core/calibration.cpp
    src/core/companion.cpp
)
target_include_directories(layerc_core PUBLIC src/core/include)
target_compile_features(layerc_core PUBLIC cxx_std_20)

layerc_enable_warnings(layerc_core)

add_library(layerc_runtime STATIC
    src/runtime/daemon_kernel.cpp
    src/runtime/runtime_services.cpp
)
target_include_directories(layerc_runtime PUBLIC
    src/runtime/include
    src/core/include
)
target_link_libraries(layerc_runtime PUBLIC layerc_core)
target_compile_features(layerc_runtime PUBLIC cxx_std_20)
layerc_enable_warnings(layerc_runtime)

add_library(layerc_miniz STATIC
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
)
include(GenerateExportHeader)
generate_export_header(layerc_miniz
    BASE_NAME miniz
    EXPORT_FILE_NAME
        "${CMAKE_CURRENT_BINARY_DIR}/generated/miniz_export.h")
target_include_directories(layerc_miniz PUBLIC
    third_party/miniz
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(layerc_miniz PUBLIC
    MINIZ_NO_ZLIB_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(layerc_miniz PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON)

if(WIN32)
    add_library(layerc_windows STATIC
        src/platform/windows.cpp
        src/platform/window_discovery.cpp
        src/platform/text.cpp
        src/platform/profile_store.cpp
        src/platform/profile_transaction.cpp
        src/platform/visual_assets.cpp
        src/platform/live_channel.cpp
        src/platform/framed_pipe.cpp
        src/platform/hid_device.cpp
        src/platform/hid_device_inference.cpp
    )
    target_include_directories(layerc_windows PUBLIC
        src/platform/include
        src/core/include
    )
    target_link_libraries(layerc_windows PUBLIC
        layerc_core user32 advapi32 shell32 ole32 gdi32 shcore
        windowscodecs bcrypt hid)
    target_compile_definitions(layerc_windows PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    layerc_enable_warnings(layerc_windows)

    add_library(layerc_capture STATIC
        src/capture/window_capture.cpp
        src/capture/static_window_snapshot.cpp
        src/capture/d3d_capture_support.cpp
        src/capture/wgc_backend.cpp
        src/capture/desktop_duplication_backend.cpp
    )
    target_include_directories(layerc_capture PUBLIC
        src/capture/include
        src/core/include
    )
    target_include_directories(layerc_capture PRIVATE src/capture)
    target_link_libraries(layerc_capture PUBLIC layerc_core layerc_windows)
    target_link_libraries(layerc_capture PRIVATE
        d3d11 dxgi dwmapi user32 gdi32)
    target_compile_definitions(layerc_capture PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    layerc_enable_warnings(layerc_capture)

    add_library(layerc_bundle STATIC
        src/bundle/pack_detail.cpp
        src/bundle/profile_pack.cpp
    )
    target_include_directories(layerc_bundle PUBLIC
        src/bundle/include
        src/core/include
        src/platform/include)
    target_link_libraries(layerc_bundle PUBLIC
        layerc_core layerc_windows)
    target_link_libraries(layerc_bundle PRIVATE layerc_miniz)
    target_compile_definitions(layerc_bundle PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_definitions(layerc_bundle PRIVATE
        MINIZ_NO_ZLIB_APIS
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
    layerc_enable_warnings(layerc_bundle)
endif()

if(WIN32 AND LAYERC_BUILD_DAEMON)
    add_executable(layerd WIN32
        src/daemon/main.cpp
        src/daemon/daemon_app.cpp
        src/daemon/daemon_services.cpp
        src/daemon/daemon_automation.cpp
        src/daemon/daemon_sessions.cpp
        src/daemon/daemon_input.cpp
        src/daemon/daemon_hid.cpp
        src/daemon/hid_resolver.cpp
        src/daemon/daemon_overlays.cpp
        src/daemon/overlay_collection.cpp
        src/daemon/daemon_live_edit.cpp
        src/daemon/daemon_diagnostics.cpp
        src/daemon/daemon_visual.cpp
        src/daemon/isolated_visual_protocol.cpp
        src/daemon/isolated_visual_worker.cpp
        src/daemon/visual_capture_worker.cpp
        src/daemon/overlay_window.cpp
        src/daemon/overlay_render.cpp
        src/gamebar/GameBarBridge.cpp
        src/gamebar/GameBarTransport.cpp
        src/gamebar/GameBarAppServiceClient.cpp
        src/gamebar/GameBarRetryPolicy.cpp
    )
    target_include_directories(layerd PRIVATE src/daemon src/gamebar)
    target_link_libraries(layerd PRIVATE
        layerc_core layerc_runtime layerc_windows layerc_capture
        shell32 user32 gdi32 xinput9_1_0)
    target_link_libraries(layerd PRIVATE psapi)
    target_link_libraries(layerd PRIVATE hid bcrypt)
    target_compile_definitions(layerd PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    layerc_enable_warnings(layerd)
    if(MSVC)
        target_link_libraries(layerd PRIVATE runtimeobject)
        target_link_libraries(layerd PRIVATE delayimp)
        target_link_options(layerd PRIVATE
            /DELAYLOAD:xinput9_1_0.dll
        )
    elseif(MINGW)
        target_link_options(layerd PRIVATE -municode)
    endif()
    set_target_properties(layerd PROPERTIES OUTPUT_NAME "layerd")
endif()

if(WIN32 AND LAYERC_BUILD_DAEMON)
    add_executable(layerworker WIN32
        src/worker/main.cpp
        src/daemon/visual_capture_worker.cpp
        src/daemon/isolated_visual_protocol.cpp
    )
    target_include_directories(layerworker PRIVATE src/daemon)
    target_link_libraries(layerworker PRIVATE
        layerc_core layerc_windows layerc_capture
        shell32 user32 gdi32)
    target_compile_definitions(layerworker PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    layerc_enable_warnings(layerworker)
    if(MINGW)
        target_link_options(layerworker PRIVATE -municode)
    endif()
    set_target_properties(layerworker PROPERTIES OUTPUT_NAME "layerworker")
endif()

option(LAYERC_BUILD_GAMEBAR_HOST "Build the C++/WinRT Game Bar companion host" ON)
if(WIN32 AND MSVC AND LAYERC_BUILD_GAMEBAR_HOST)
    include(cmake/GameBarWidget.cmake)
endif()


if(WIN32 AND LAYERC_BUILD_EDITOR)
    add_executable(layere WIN32
        src/editor/main.cpp
        src/editor/editor_dialog.cpp
        src/editor/editor_layout.cpp
        src/editor/editor_profiles.cpp
        src/editor/editor_profile_support.cpp
        src/editor/editor_schema.cpp
        src/editor/editor_cooldowns.cpp
        src/editor/editor_bars.cpp
        src/editor/editor_tasks.cpp
        src/editor/editor_theme.cpp
        src/editor/editor_draw.cpp
        src/editor/editor_appearance.cpp
        src/editor/editor_color_picker.cpp
        src/editor/editor_controls.cpp
        src/editor/editor_live_edit.cpp
        src/editor/editor_visual.cpp
        src/editor/editor_automation.cpp
        src/editor/editor_automation_canvas.cpp
        src/editor/editor_automation_simulator.cpp
        src/editor/editor_packs.cpp
        src/editor/editor_diagnostics.cpp
        src/editor/editor_hid.cpp
        src/editor/editor_resources.rc
    )
    target_include_directories(layere PRIVATE src/editor)
    target_link_libraries(layere PRIVATE
        layerc_core
        layerc_windows
        layerc_bundle
        comctl32
        comdlg32
        dwmapi
        shell32
        uxtheme
        user32
        gdi32
    )
    target_compile_definitions(layere PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    layerc_enable_warnings(layere)
    if(MINGW)
        target_link_options(layere PRIVATE -municode)
    endif()
    set_target_properties(layere PROPERTIES OUTPUT_NAME "layere")
endif()

if(WIN32 AND LAYERC_BUILD_EDITOR)
    add_executable(layercal WIN32
        src/calibration/main.cpp
        src/calibration/calibration_window.cpp
        src/calibration/calibration_templates.cpp
        src/calibration/calibration_resources.rc
    )
    target_include_directories(layercal PRIVATE src/calibration)
    target_link_libraries(layercal PRIVATE
        layerc_core layerc_windows layerc_capture
        comctl32 dwmapi shell32 uxtheme user32 gdi32)
    target_compile_definitions(layercal PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    layerc_enable_warnings(layercal)
    if(MINGW)
        target_link_options(layercal PRIVATE -municode)
    endif()
    set_target_properties(layercal PROPERTIES OUTPUT_NAME "layercal")
endif()

set(LAYERC_MINGW_RUNTIME_DLLS)
set(LAYERC_MINGW_RUNTIME_COMPLETE TRUE)
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
        else()
            set(LAYERC_MINGW_RUNTIME_COMPLETE FALSE)
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
           NOT TARGET layercal OR NOT TARGET layerworker)
        message(STATUS "Game Bar MSIX target disabled: full package requires layerd, layere, layercal, and layerworker")
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
                -Manifest "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/Package.appxmanifest"
                -Asset "${LAYERC_GAMEBAR_WIDGET_ASSET}"
                -GameBarPrivateWinmd "${LAYERC_GAMEBAR_NUGET_PACKAGES}/Microsoft.Gaming.XboxGameBar.7.3.2607010/private/Microsoft.Gaming.XboxGameBar.Private.winmd"
                -DesktopRuntimeDirectory "${LAYERC_MSVC_REDIST_DIR}"
                -LayerD "$<TARGET_FILE:layerd>"
                -LayerE "$<TARGET_FILE:layere>"
                -LayerCal "$<TARGET_FILE:layercal>"
                -LayerWorker "$<TARGET_FILE:layerworker>"
            COMMAND "${LAYERC_MAKEAPPX}" pack
                /d "${LAYERC_GAMEBAR_PACKAGE_DIR}"
                /p "${LAYERC_GAMEBAR_MSIX}" /o
            BYPRODUCTS "${LAYERC_GAMEBAR_MSIX}"
            DEPENDS layerc_gamebar_host layerd layere layercal layerworker
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/Package.appxmanifest"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/StageGameBarPackage.ps1"
            VERBATIM)
    endif()
endif()

if(TARGET layerd)
    install(TARGETS layerd RUNTIME DESTINATION . COMPONENT Runtime)
endif()
if(TARGET layere)
    install(TARGETS layere RUNTIME DESTINATION . COMPONENT Runtime)
endif()
if(TARGET layercal)
    install(TARGETS layercal RUNTIME DESTINATION . COMPONENT Runtime)
endif()
if(TARGET layerworker)
    install(TARGETS layerworker RUNTIME DESTINATION . COMPONENT Runtime)
endif()
# The portable ZIP intentionally remains desktop-only. Game Bar package
# inputs are staged only by the explicit layerc_gamebar_msix target.
install(FILES
    README.md
    PLAN.md
    THIRD_PARTY_NOTICES.md
    DESTINATION . COMPONENT Runtime)
install(FILES third_party/miniz/LICENSE third_party/miniz/UPSTREAM.md
    DESTINATION licenses/miniz COMPONENT Runtime)

if(MINGW AND LAYERC_MINGW_RUNTIME_DLLS)
    install(FILES ${LAYERC_MINGW_RUNTIME_DLLS}
        DESTINATION . COMPONENT Runtime)
endif()

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "LayerC")
set(CPACK_PACKAGE_VENDOR "LayerC")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "LayerC-${PROJECT_VERSION}-windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_MONOLITHIC_INSTALL ON)
include(CPack)

if(LAYERC_BUILD_TESTS)
    enable_testing()

    add_executable(layerc_runtime_tests
        tests/runtime_services_tests.cpp
        tests/test_support.cpp
    )
    target_link_libraries(layerc_runtime_tests PRIVATE layerc_runtime)
    layerc_enable_warnings(layerc_runtime_tests)
    add_test(NAME layerc_runtime_tests COMMAND layerc_runtime_tests)
    add_executable(layerc_core_tests
        tests/core_tests.cpp
        tests/test_support.cpp
        tests/cooldown_input_tests.cpp
        tests/automation_tests.cpp
        tests/automation_value_tests.cpp
        tests/automation_contract_tests.cpp
        tests/profile_tests.cpp
        tests/runtime_json_tests.cpp
        tests/live_edit_tests.cpp
        tests/visual_profile_tests.cpp
        tests/pixel_engine_tests.cpp
    )
    target_link_libraries(layerc_core_tests PRIVATE layerc_core)
    add_test(NAME layerc_core_tests COMMAND layerc_core_tests)
    set_tests_properties(layerc_core_tests PROPERTIES TIMEOUT 30)

    add_executable(layerc_automation_benchmark
        tools/automation_benchmark.cpp)
    target_link_libraries(layerc_automation_benchmark PRIVATE layerc_core)
    layerc_enable_warnings(layerc_automation_benchmark)

    if(WIN32)
        add_executable(layerc_capture_tests
            tests/capture_math_tests.cpp
            tests/test_support.cpp)
        target_link_libraries(layerc_capture_tests PRIVATE layerc_capture)
        add_test(NAME layerc_capture_tests COMMAND layerc_capture_tests)
        set_tests_properties(layerc_capture_tests PROPERTIES TIMEOUT 30)

        add_executable(layerc_capture_integration_tests
            tests/capture_integration_tests.cpp)
        target_link_libraries(layerc_capture_integration_tests
            PRIVATE layerc_capture)
        if(LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS)
            add_test(NAME layerc_capture_integration_tests
                COMMAND layerc_capture_integration_tests)
            set_tests_properties(layerc_capture_integration_tests PROPERTIES
                TIMEOUT 30 LABELS "interactive")
        endif()

        add_executable(layerc_capture_probe tools/capture_probe.cpp)
        target_link_libraries(layerc_capture_probe PRIVATE layerc_capture)
        target_compile_definitions(layerc_capture_probe PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MINGW)
            target_link_options(layerc_capture_probe PRIVATE -municode)
        endif()

        add_executable(lctest WIN32 tools/lctest.cpp)
        target_compile_definitions(lctest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MINGW)
            target_link_options(lctest PRIVATE -municode)
        endif()
        set_target_properties(lctest PROPERTIES OUTPUT_NAME "lctest")

        add_executable(layerc_overlay_tests
            tests/overlay_window_tests.cpp
            tests/test_support.cpp
            src/daemon/overlay_window.cpp
            src/daemon/overlay_render.cpp
        )
        target_include_directories(layerc_overlay_tests PRIVATE src/daemon)
        target_link_libraries(layerc_overlay_tests PRIVATE
            layerc_core layerc_windows user32 gdi32)
        target_compile_definitions(layerc_overlay_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MINGW)
            target_link_options(layerc_overlay_tests PRIVATE -municode)
        endif()
        layerc_enable_warnings(layerc_overlay_tests)
        add_dependencies(layerc_overlay_tests lctest)
        add_test(NAME layerc_overlay_tests COMMAND layerc_overlay_tests $<TARGET_FILE:lctest>)
        set_tests_properties(layerc_overlay_tests PROPERTIES
            TIMEOUT 45
            SKIP_RETURN_CODE 77)

        add_executable(layerc_platform_tests
            tests/platform_live_tests.cpp
            tests/test_support.cpp)
        target_link_libraries(layerc_platform_tests PRIVATE layerc_core layerc_windows)
        target_compile_definitions(layerc_platform_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        layerc_enable_warnings(layerc_platform_tests)
        add_test(NAME layerc_platform_tests COMMAND layerc_platform_tests)
        set_tests_properties(layerc_platform_tests PROPERTIES TIMEOUT 30)

        add_executable(layerc_bundle_tests
            tests/profile_pack_tests.cpp
            tests/test_support.cpp)
        target_link_libraries(layerc_bundle_tests PRIVATE
            layerc_bundle layerc_miniz)
        target_compile_definitions(layerc_bundle_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        layerc_enable_warnings(layerc_bundle_tests)
        add_test(NAME layerc_bundle_tests COMMAND layerc_bundle_tests)
        set_tests_properties(layerc_bundle_tests PROPERTIES TIMEOUT 45)

        add_executable(layerc_isolated_visual_tests
            tests/isolated_visual_protocol_tests.cpp
            tests/test_support.cpp
            src/daemon/isolated_visual_protocol.cpp)
        target_include_directories(layerc_isolated_visual_tests PRIVATE
            src/daemon)
        target_link_libraries(layerc_isolated_visual_tests PRIVATE
            layerc_core layerc_windows layerc_capture)
        target_compile_definitions(layerc_isolated_visual_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        layerc_enable_warnings(layerc_isolated_visual_tests)
        add_test(NAME layerc_isolated_visual_tests
            COMMAND layerc_isolated_visual_tests)
        set_tests_properties(layerc_isolated_visual_tests PROPERTIES
            TIMEOUT 30)

        add_executable(layerc_companion_tests
            tests/companion_tests.cpp
            tests/test_support.cpp
            src/gamebar/GameBarBridge.cpp
            src/gamebar/GameBarTransport.cpp
            src/gamebar/GameBarAppServiceClient.cpp
            src/gamebar/GameBarRetryPolicy.cpp
        )
        target_include_directories(layerc_companion_tests PRIVATE src/gamebar)
        target_link_libraries(layerc_companion_tests PRIVATE
            layerc_core layerc_windows)
        target_compile_definitions(layerc_companion_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        layerc_enable_warnings(layerc_companion_tests)
        if(MSVC)
            target_link_libraries(layerc_companion_tests PRIVATE runtimeobject)
        endif()
        add_test(NAME layerc_companion_tests COMMAND layerc_companion_tests)
        set_tests_properties(layerc_companion_tests PROPERTIES TIMEOUT 30)


        add_executable(layerc_editor_tests
            tests/editor_dialog_tests.cpp
            tests/test_support.cpp
            src/editor/editor_theme.cpp
            src/editor/editor_schema.cpp
            src/editor/editor_resources.rc
            src/calibration/calibration_resources.rc
        )
        target_include_directories(layerc_editor_tests PRIVATE src/editor)
        target_link_libraries(layerc_editor_tests PRIVATE
            layerc_core layerc_windows comctl32 dwmapi uxtheme user32 gdi32)
        target_compile_definitions(layerc_editor_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        layerc_enable_warnings(layerc_editor_tests)
        add_test(NAME layerc_editor_tests COMMAND layerc_editor_tests)
        set_tests_properties(layerc_editor_tests PROPERTIES TIMEOUT 30)
        if(TARGET layere)
            # Avoid compiling the large theming translation unit twice at the
            # same time on memory-constrained MinGW builders.
            add_dependencies(layerc_editor_tests layere)
        endif()

        if(MINGW)
            get_filename_component(LAYERC_MINGW_RUNTIME_DIR
                "${CMAKE_CXX_COMPILER}" DIRECTORY)
            set_tests_properties(
                layerc_core_tests
                layerc_overlay_tests
                layerc_platform_tests
                layerc_bundle_tests
                layerc_isolated_visual_tests
                 layerc_editor_tests
                 layerc_capture_tests
                PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${LAYERC_MINGW_RUNTIME_DIR}"
            )
            if(LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS)
                set_tests_properties(layerc_capture_integration_tests
                    PROPERTIES ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${LAYERC_MINGW_RUNTIME_DIR}")
            endif()
        endif()
    endif()
endif()
