# Test targets and CTest registration. Included from the top-level
# CMakeLists.txt when LAYERC_BUILD_TESTS is on; paths stay relative to the
# source root because include() keeps the including directory scope.

enable_testing()

# Test construction and registration live together so newly added suites
# inherit the same warnings, harness, labels, and MinGW runtime PATH.
function(layerc_add_test target)
    set(options NO_REGISTER WIN32 MINGW_UNICODE)
    set(one_value_args TIMEOUT SKIP_RETURN_CODE RESOURCE_LOCK)
    set(multi_value_args SOURCES LINK ARGUMENTS LABELS)
    cmake_parse_arguments(TEST "${options}" "${one_value_args}"
        "${multi_value_args}" ${ARGN})
    if(TEST_UNPARSED_ARGUMENTS OR NOT TEST_SOURCES)
        message(FATAL_ERROR "${target}: layerc_add_test requires SOURCES (unexpected: ${TEST_UNPARSED_ARGUMENTS})")
    endif()
    add_executable(${target} ${TEST_SOURCES} tests/test_support.cpp)
    if(TEST_LINK)
        target_link_libraries(${target} PRIVATE ${TEST_LINK})
    endif()
    if(TEST_MINGW_UNICODE)
        layerc_win32_exe(${target})
    elseif(TEST_WIN32)
        layerc_win32(${target})
        layerc_common_controls(${target})
    else()
        layerc_enable_warnings(${target})
    endif()
    if(TEST_NO_REGISTER)
        return()
    endif()

    add_test(NAME ${target} COMMAND ${target} ${TEST_ARGUMENTS})
    set(properties)
    if(TEST_TIMEOUT)
        list(APPEND properties TIMEOUT ${TEST_TIMEOUT})
    endif()
    if(TEST_SKIP_RETURN_CODE)
        list(APPEND properties SKIP_RETURN_CODE ${TEST_SKIP_RETURN_CODE})
    endif()
    if(TEST_LABELS)
        set(test_labels "${TEST_LABELS}")
    else()
        set(test_labels "required;headless")
    endif()
    if(TEST_RESOURCE_LOCK)
        list(APPEND properties RESOURCE_LOCK ${TEST_RESOURCE_LOCK})
    endif()
    if(MINGW)
        get_filename_component(mingw_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        list(APPEND properties ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${mingw_runtime_dir}")
    endif()
    set_tests_properties(${target} PROPERTIES ${properties} LABELS "${test_labels}")
endfunction()

layerc_add_test(layerc_runtime_tests
    SOURCES tests/runtime_services_tests.cpp tests/runtime_display_diagnostics_tests.cpp
    LINK layerc_runtime)

# Tooling contract: the shared benchmark result writer emits the documented
# keys without touching PowerShell's read-only $Host.
find_program(LAYERC_PWSH NAMES pwsh)
if(LAYERC_PWSH)
    add_test(NAME layerc_benchmark_result_tests
        COMMAND "${LAYERC_PWSH}" -NoProfile -NonInteractive -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_result_tests.ps1")
    set_tests_properties(layerc_benchmark_result_tests PROPERTIES
        TIMEOUT 60 LABELS "required;headless;tooling")
endif()
layerc_add_test(layerc_core_tests
    SOURCES tests/core_tests.cpp
    tests/cooldown_input_tests.cpp
    tests/automation_tests.cpp
    tests/automation_value_tests.cpp
    tests/automation_contract_tests.cpp
    tests/automation_session_tests.cpp
    tests/profile_tests.cpp
    tests/profile_item_tests.cpp
    tests/runtime_json_tests.cpp
    tests/utf8_tests.cpp
    tests/update_manifest_tests.cpp
    tests/game_match_tests.cpp
    tests/cooldown_group_tests.cpp
    tests/panel_presentation_tests.cpp
    tests/item_geometry_tests.cpp
    tests/panel_item_presentation_tests.cpp
    tests/cooldown_active_tests.cpp
    tests/automation_item_override_tests.cpp
    tests/profile_module_tests.cpp
    tests/live_edit_tests.cpp
    tests/visual_profile_tests.cpp
    tests/pixel_engine_tests.cpp
    LINK layerc_core TIMEOUT 30)

layerc_add_test(layerc_launcher_tests
    SOURCES tests/launcher_update_tests.cpp
    LINK layerc_launcher_core TIMEOUT 30)
layerc_add_test(layerc_template_png_tests
    SOURCES tests/template_png_tests.cpp
    LINK layerc_bundle layerc_miniz TIMEOUT 30)
layerc_add_test(layerc_automation_persistence_tests
    SOURCES tests/automation_persistence_tests.cpp
    LINK layerc_bundle TIMEOUT 30)
# Shared editor document, history, and commit behavior on every platform.
layerc_add_test(layerc_editor_document_tests
    SOURCES tests/editor_document_tests.cpp
        tests/editor_panel_items_tests.cpp
        tests/editor_panel_items_additional_tests.cpp
        tests/editor_document_store_tests.cpp
        tests/editor_document_ownership_tests.cpp
        tests/editor_document_scan_tests.cpp
    LINK layerc_editor_core TIMEOUT 30)
# Shared automation graph edits, simulator replay and trace files.
layerc_add_test(layerc_editor_automation_tests
    SOURCES tests/editor_automation_graph_tests.cpp
    LINK layerc_editor_core TIMEOUT 30)
# Shared Runtime Diagnostics text for both editors.
layerc_add_test(layerc_editor_diagnostics_view_tests
    SOURCES tests/editor_diagnostics_view_tests.cpp
    LINK layerc_editor_core TIMEOUT 30)
# Shared visual calibration model and calibration-result merge.
layerc_add_test(layerc_editor_visual_tests
    SOURCES tests/editor_visual_assignment_tests.cpp
    LINK layerc_editor_core TIMEOUT 30)
# Platform-neutral color math shared by both editors.
layerc_add_test(layerc_editor_color_tests
    SOURCES tests/editor_color_tests.cpp
    LINK layerc_editor_color TIMEOUT 30)
if(WIN32)
    layerc_add_test(layerc_launcher_win_tests
        SOURCES tests/launcher_win_tests.cpp
        LINK layerc_launcher_win TIMEOUT 30)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Fixtures deliberately name only fields relevant to each case; C++20
    # value-initializes the remaining aggregate members. Keep other -Wextra
    # diagnostics enabled without requiring hundreds of empty initializers.
    target_compile_options(layerc_core_tests PRIVATE -Wno-missing-field-initializers)
    target_compile_options(layerc_editor_document_tests PRIVATE -Wno-missing-field-initializers)
    target_compile_options(layerc_editor_diagnostics_view_tests PRIVATE -Wno-missing-field-initializers)
    target_compile_options(layerc_editor_automation_tests PRIVATE -Wno-missing-field-initializers)
    target_compile_options(layerc_editor_visual_tests PRIVATE -Wno-missing-field-initializers)
endif()

if(NOT WIN32)
    # Test-only validator-derived workload helper, never installed. The GUI
    # resource gate uses its compiled limits rather than a parallel schema.
    add_executable(layerc_editor_resource_probe tools/linux-gui/resource_probe.cpp)
    target_include_directories(layerc_editor_resource_probe PRIVATE src/editor/common)
    target_link_libraries(layerc_editor_resource_probe PRIVATE layerc_editor_core)
    layerc_enable_warnings(layerc_editor_resource_probe)

    layerc_add_test(layerc_linux_profile_store_tests
        SOURCES tests/linux_profile_store_tests.cpp
            src/platform/profile_transaction_testing.cpp
        LINK layerc_linux TIMEOUT 30)
    target_include_directories(layerc_linux_profile_store_tests PRIVATE
        src/platform/linux)
    # Test-only fsync fault injection; production binaries use libc directly.
    target_link_options(layerc_linux_profile_store_tests PRIVATE
        "LINKER:--wrap=fsync")

    layerc_add_test(layerc_linux_visual_assets_tests
        SOURCES tests/linux_visual_assets_tests.cpp
            src/platform/profile_transaction_testing.cpp
        LINK layerc_linux TIMEOUT 30)
    target_include_directories(layerc_linux_visual_assets_tests PRIVATE
        src/platform/linux)

    layerc_add_test(layerc_linux_profile_pack_tests
        SOURCES tests/linux_profile_pack_tests.cpp
        LINK layerc_bundle layerc_miniz TIMEOUT 45)

    layerc_add_test(layerc_linux_display_types_tests
        SOURCES tests/linux_display_types_tests.cpp
        LINK layerc_linux TIMEOUT 30)
    target_include_directories(layerc_linux_display_types_tests PRIVATE
        src/platform/linux)

    if(TARGET layerc_wayland_connection)
        layerc_add_test(layerc_linux_wayland_connection_tests
            SOURCES tests/linux_wayland_connection_tests.cpp
            LINK layerc_wayland_connection TIMEOUT 10)
        # Protocol-server fakes are test-only; never link libwayland-server
        # or their fixture threads into the daemon.
        pkg_check_modules(LAYERC_WAYLAND_TEST_SERVER QUIET IMPORTED_TARGET wayland-server)
        if(LAYERC_WAYLAND_TEST_SERVER_FOUND)
            layerc_add_test(layerc_linux_wayland_registry_tests
                SOURCES tests/linux_wayland_registry_tests.cpp
                LINK layerc_daemon_linux PkgConfig::LAYERC_WAYLAND_TEST_SERVER
                TIMEOUT 10)
            if(TARGET layerc_x11)
                layerc_add_test(layerc_linux_wayland_xwayland_loss_tests
                    SOURCES tests/linux_wayland_registry_tests.cpp
                    LINK layerc_daemon_linux PkgConfig::LAYERC_WAYLAND_TEST_SERVER
                    TIMEOUT 10 LABELS required x11 SKIP_RETURN_CODE 77
                    RESOURCE_LOCK layerc_x11_display)
                target_compile_definitions(layerc_linux_wayland_xwayland_loss_tests
                    PRIVATE LAYERC_TEST_XWAYLAND_LOSS)
            endif()
            if(TARGET layerc_wayland)
                layerc_add_test(layerc_linux_wayland_keymap_tests
                    SOURCES tests/linux_wayland_keymap_tests.cpp
                    LINK layerc_wayland PkgConfig::LAYERC_WAYLAND_TEST_SERVER
                    TIMEOUT 10)
            endif()
        endif()
    endif()
    if(TARGET layerc_wayland)
        layerc_add_test(layerc_linux_wayland_paint_tests
            SOURCES tests/linux_wayland_paint_tests.cpp
            LINK layerc_wayland TIMEOUT 30)
    endif()
    if(TARGET layerc_wayland)
        # Screencopy and the virtual pointer are test-only protocols: they
        # read pixels and synthesize input inside the disposable compositor
        # and must never link into layerd.
        set(layerc_wayland_test_protocol_sources)
        layerc_wayland_protocol(layerc_wayland_test_protocol_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wayland-protocols/wlr-screencopy-unstable-v1.xml"
            wlr-screencopy-unstable-v1)
        layerc_wayland_protocol(layerc_wayland_test_protocol_sources
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/wayland-protocols/wlr-virtual-pointer-unstable-v1.xml"
            wlr-virtual-pointer-unstable-v1)
        add_library(layerc_wayland_test_client STATIC
            tests/wayland_test_client.cpp ${layerc_wayland_test_protocol_sources})
        target_include_directories(layerc_wayland_test_client PUBLIC tests)
        target_link_libraries(layerc_wayland_test_client PUBLIC layerc_wayland)
        layerc_enable_warnings(layerc_wayland_test_client)
        set_source_files_properties(${layerc_wayland_test_protocol_sources}
            PROPERTIES COMPILE_OPTIONS "-w")
        layerc_add_test(layerc_linux_wayland_native_tests
            SOURCES tests/linux_wayland_native_tests.cpp
            LINK layerc_wayland layerc_wayland_test_client layerc_daemon_linux NO_REGISTER)
        add_test(NAME layerc_linux_wayland_native_tests
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/wayland-test-session.sh"
                $<TARGET_FILE:layerc_linux_wayland_native_tests>)
        set_tests_properties(layerc_linux_wayland_native_tests PROPERTIES
            LABELS wayland SKIP_RETURN_CODE 77 TIMEOUT 60)
    endif()
    layerc_add_test(layerc_linux_session_engine_tests
        SOURCES tests/linux_session_engine_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
    layerc_add_test(layerc_linux_display_route_tests
        SOURCES tests/linux_display_route_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
    layerc_add_test(layerc_linux_display_route_binding_tests
        SOURCES tests/linux_display_route_binding_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
    layerc_add_test(layerc_linux_display_adapters_tests
        SOURCES tests/linux_display_adapters_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
    if(LAYERC_HAVE_X11)
        layerc_add_test(layerc_linux_display_x11_tests
            SOURCES tests/linux_display_x11_tests.cpp
            LINK layerc_daemon_linux TIMEOUT 30
            LABELS "required;x11" SKIP_RETURN_CODE 77)
    endif()
    layerc_add_test(layerc_linux_event_loop_tests
        SOURCES tests/linux_event_loop_tests.cpp
        LINK layerc_linux layerc_runtime TIMEOUT 30)
    target_include_directories(layerc_linux_event_loop_tests PRIVATE
        src/daemon/linux
        src/platform/linux)

    # Registry admission and automation health use a synthetic scan;
    # keep this suite in the display-free headless gate.
    layerc_add_test(layerc_linux_x11_registry_tests
        SOURCES tests/linux_x11_registry_tests.cpp LINK layerc_daemon_linux
        TIMEOUT 60)

    # X11 tracking probes run against Xvfb; the test skips (77) when no
    # display is available, so non-X builders stay green.
    if(TARGET layerc_x11)
        # Test-only target window for installed-binary capture smoke.
        add_executable(layerc_linux_x11_smoke_fixture
            tools/linux_x11_smoke_fixture.cpp)
        target_link_libraries(layerc_linux_x11_smoke_fixture PRIVATE
            ${X11_LIBRARIES})
        layerc_enable_warnings(layerc_linux_x11_smoke_fixture)

        layerc_add_test(layerc_linux_x11_tests
            SOURCES tests/linux_x11_tests.cpp LINK layerc_x11
            TIMEOUT 60 LABELS required x11 RESOURCE_LOCK layerc_x11_display
            SKIP_RETURN_CODE 77)
        target_include_directories(layerc_linux_x11_tests PRIVATE
            src/platform/linux)

        layerc_add_test(layerc_linux_x11_capture_tests
            SOURCES tests/linux_x11_capture_tests.cpp LINK layerc_x11
            TIMEOUT 60 LABELS required x11 RESOURCE_LOCK layerc_x11_display
            SKIP_RETURN_CODE 77 ARGUMENTS $<TARGET_FILE:layercal>)
        target_include_directories(layerc_linux_x11_capture_tests PRIVATE
            src/platform/linux)
        add_dependencies(layerc_linux_x11_capture_tests layercal)

        layerc_add_test(layerc_linux_x11_visual_tests
            SOURCES tests/linux_x11_visual_tests.cpp LINK layerc_daemon_linux
            TIMEOUT 60 LABELS required x11 RESOURCE_LOCK layerc_x11_display
            SKIP_RETURN_CODE 77)

        # XI2 input + session replay. XTest is a test-only dependency:
        # it is never linked into layerd, which only ever reads input.
        if(X11_Xtst_LIB)
        layerc_add_test(layerc_linux_x11_input_tests
            SOURCES tests/linux_x11_input_tests.cpp
            LINK layerc_daemon_linux ${X11_Xtst_LIB}
            TIMEOUT 60 LABELS required x11 RESOURCE_LOCK layerc_x11_display
            SKIP_RETURN_CODE 77)
        endif()

        # Shape overlays + monitor scale; XTest checks real event routing
        # when available and remains linked only into this test executable.
        layerc_add_test(layerc_linux_x11_overlay_tests
            SOURCES tests/linux_x11_overlay_tests.cpp
                tests/linux_typed_overlay_tests.cpp LINK layerc_daemon_linux
            TIMEOUT 60 LABELS required x11 RESOURCE_LOCK layerc_x11_display
            SKIP_RETURN_CODE 77)
        if(X11_Xtst_LIB)
            target_link_libraries(layerc_linux_x11_overlay_tests PRIVATE
                ${X11_Xtst_LIB})
            target_compile_definitions(layerc_linux_x11_overlay_tests PRIVATE
                LAYERC_TEST_XTEST=1)
        endif()
    endif()

    layerc_add_test(layerc_linux_control_channel_tests
        SOURCES tests/linux_control_channel_tests.cpp
            tests/linux_control_service_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
    target_compile_definitions(layerc_linux_control_channel_tests PRIVATE
        LAYERC_TEST_LAYERCTL_PATH="$<TARGET_FILE:layerctl>")
    add_dependencies(layerc_linux_control_channel_tests layerctl)

    layerc_add_test(layerc_linux_live_draft_tests
        SOURCES tests/linux_live_draft_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)

    layerc_add_test(layerc_linux_lease_worker_tests
        SOURCES tests/linux_lease_worker_tests.cpp
        LINK layerc_editor_linux TIMEOUT 30)

    layerc_add_test(layerc_linux_control_ownership_tests
        SOURCES tests/linux_control_ownership_tests.cpp
        LINK layerc_linux TIMEOUT 30)
    target_include_directories(layerc_linux_control_ownership_tests PRIVATE
        src/platform/linux)
    # Test-only bind failure and descriptor-reuse simulation.
    target_link_options(layerc_linux_control_ownership_tests PRIVATE
        "LINKER:--wrap=bind" "LINKER:--wrap=close")

    layerc_add_test(layerc_linux_editor_daemon_link_tests
        SOURCES tests/linux_editor_daemon_link_tests.cpp
        LINK layerc_editor_linux layerc_daemon_linux TIMEOUT 30)
    target_include_directories(layerc_linux_editor_daemon_link_tests PRIVATE
        src/platform/linux)

    layerc_add_test(layerc_linux_editor_model_tests
        SOURCES tests/linux_editor_model_tests.cpp
        LINK layerc_editor_linux TIMEOUT 30)
    layerc_add_test(layerc_linux_editor_session_tests
        SOURCES tests/linux_editor_session_tests.cpp
        LINK layerc_editor_linux TIMEOUT 30)
    # The toolkit suite is its own gate (label editor), so builds without the
    # editor keep an unchanged x11 gate and editor gates require it by name.
    if(TARGET layerc_editor_fltk)
        layerc_add_test(layerc_linux_editor_widget_tests
            SOURCES tests/linux_editor_widget_tests.cpp
                tests/linux_editor_items_tests.cpp
                tests/linux_editor_dialog_tests.cpp
                tests/linux_editor_accessibility_tests.cpp
                tests/linux_editor_visual_contract_tests.cpp
                tests/linux_editor_binding_capture_tests.cpp
                tests/linux_editor_diagnostics_tests.cpp
                tests/linux_editor_command_registry_tests.cpp
                tests/linux_editor_design_system_paint_tests.cpp
            LINK layerc_editor_fltk TIMEOUT 45 SKIP_RETURN_CODE 77
            LABELS required editor RESOURCE_LOCK layerc_x11_display)
        set_source_files_properties(
            tests/linux_editor_accessibility_tests.cpp tests/linux_editor_diagnostics_tests.cpp
            PROPERTIES COMPILE_OPTIONS "${LAYERC_ATK_INCLUDES};${LAYERC_ATK_CFLAGS_OTHER}")
        layerc_editor_build_runtime(layerc_linux_editor_widget_tests)
        if(X11_Xtst_LIB)
            target_link_libraries(layerc_linux_editor_widget_tests PRIVATE ${X11_Xtst_LIB})
            target_compile_definitions(layerc_linux_editor_widget_tests PRIVATE LAYERC_TEST_XTEST=1)
        endif()
        layerc_add_test(layerc_linux_editor_autosave_tests
            SOURCES tests/linux_editor_autosave_tests.cpp
            LINK layerc_editor_fltk TIMEOUT 45 SKIP_RETURN_CODE 77
            LABELS required editor RESOURCE_LOCK layerc_x11_display)
        layerc_editor_build_runtime(layerc_linux_editor_autosave_tests)
    endif()

    # Wayland-era lane suites (docs/Wayland.md); each module is optional.
    foreach(layerc_linux_module IN LISTS layerc_linux_modules)
        include(cmake/${layerc_linux_module}Tests.cmake OPTIONAL)
    endforeach()
endif()

layerc_add_test(layerc_automation_benchmark
    SOURCES tools/automation_benchmark.cpp LINK layerc_core
    TIMEOUT 30 LABELS required headless benchmark)
set_tests_properties(layerc_automation_benchmark PROPERTIES RUN_SERIAL TRUE)

add_test(NAME layerc_visual_asset_gc_contract_tests
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_asset_gc_contracts.cmake")
set_tests_properties(layerc_visual_asset_gc_contract_tests PROPERTIES
    TIMEOUT 30 LABELS "required;headless")

if(WIN32)
    set(layerc_capture_test_link layerc_capture)
else()
    set(layerc_capture_test_link layerc_capture_common)
endif()
layerc_add_test(layerc_capture_tests
    SOURCES tests/capture_math_tests.cpp
    LINK ${layerc_capture_test_link} layerc_runtime TIMEOUT 30)

if(WIN32)
    if(LAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS)
        layerc_add_test(layerc_capture_integration_tests
            SOURCES tests/capture_integration_tests.cpp LINK layerc_capture
            TIMEOUT 30 LABELS interactive)
    else()
        layerc_add_test(layerc_capture_integration_tests
            SOURCES tests/capture_integration_tests.cpp LINK layerc_capture
            NO_REGISTER)
    endif()

    add_executable(layerc_capture_probe tools/capture_probe.cpp)
    target_link_libraries(layerc_capture_probe PRIVATE layerc_capture)
    layerc_win32_exe(layerc_capture_probe)

    add_executable(lctest WIN32 tools/lctest.cpp)
    layerc_win32_exe(lctest)

    layerc_add_test(layerc_overlay_tests
        SOURCES tests/overlay_window_tests.cpp LINK layerc_overlay
        MINGW_UNICODE TIMEOUT 45 SKIP_RETURN_CODE 77
        ARGUMENTS $<TARGET_FILE:lctest>)
    target_include_directories(layerc_overlay_tests PRIVATE src/daemon)
    add_dependencies(layerc_overlay_tests lctest)

    layerc_add_test(layerc_platform_tests
        SOURCES tests/platform_live_tests.cpp tests/platform_live_scenarios.cpp
            tests/platform_live_test_support.cpp
            src/platform/profile_transaction_testing.cpp
        LINK layerc_core layerc_windows WIN32 TIMEOUT 30)

    layerc_add_test(layerc_daemon_runtime_tests
        SOURCES tests/daemon_runtime_services_tests.cpp
            tests/daemon_owner_tests.cpp
        LINK layerc_daemon_windows WIN32 TIMEOUT 30)
    target_include_directories(layerc_daemon_runtime_tests PRIVATE src/daemon)

    layerc_add_test(layerc_bundle_tests
        SOURCES tests/profile_pack_tests.cpp LINK layerc_bundle layerc_miniz
        WIN32 TIMEOUT 45)

    layerc_add_test(layerc_isolated_visual_tests
        SOURCES tests/isolated_visual_protocol_tests.cpp
            tests/capture_health_diagnostics_tests.cpp
        LINK layerc_isolated layerc_runtime
        WIN32 TIMEOUT 30)
    target_include_directories(layerc_isolated_visual_tests PRIVATE
        src/daemon)

    layerc_add_test(layerc_companion_tests
        SOURCES tests/companion_tests.cpp tests/companion_widget_tests.cpp
            tests/companion_items_tests.cpp
        LINK layerc_gamebar
        WIN32 TIMEOUT 30)
    target_include_directories(layerc_companion_tests PRIVATE src/gamebar)

    layerc_add_test(layerc_editor_tests
        SOURCES tests/editor_dialog_tests.cpp tests/editor_regression_tests.cpp
            tests/editor_main_dialog_tests.cpp
            tests/editor_appearance_dialog_tests.cpp
            tests/editor_tasks_dialog_tests.cpp
            tests/editor_pack_dialog_tests.cpp
            tests/editor_automation_dialog_tests.cpp
            tests/editor_hid_dialog_tests.cpp
            tests/editor_diagnostics_dialog_tests.cpp
            tests/editor_calibration_tests.cpp
            tests/editor_asset_tests.cpp tests/editor_component_tests.cpp
            tests/editor_theme_tests.cpp tests/editor_layout_tests.cpp
            tests/editor_accessibility_tests.cpp
            src/editor/editor_resources.rc
        LINK layerc_editor_ui WIN32 TIMEOUT 30)
    target_include_directories(layerc_editor_tests PRIVATE src/editor)

    # F01: the live-edit controller against a scripted transport and clock.
    layerc_add_test(layerc_editor_live_edit_tests
        SOURCES tests/editor_live_edit_session_tests.cpp
        LINK layerc_editor_ui WIN32 TIMEOUT 30)

    layerc_add_test(layerc_calibration_tests
        SOURCES tests/calibration_window_tests.cpp
            tests/calibration_reference_tests.cpp
            src/calibration/calibration_resources.rc
        LINK layerc_calibration_ui WIN32 TIMEOUT 30)

endif()
