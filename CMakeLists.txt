cmake_minimum_required(VERSION 3.28)

project(LayerC VERSION 0.1.0 LANGUAGES CXX)

option(LAYERC_BUILD_DAEMON "Build the native layerd daemon" ON)
option(LAYERC_BUILD_EDITOR "Build the native LayerE editor" ON)
option(LAYERC_BUILD_TESTS "Build LayerC tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MINGW)
    # Ninja otherwise scales to every logical CPU; MinGW's C++ front end can
    # exhaust memory when several Win32-heavy translation units compile at
    # once. Link and resource jobs remain unconstrained.
    set_property(GLOBAL PROPERTY JOB_POOLS layerc_mingw_compile=2)
    set(CMAKE_JOB_POOL_COMPILE layerc_mingw_compile)
endif()

add_library(layerc_core STATIC
    src/core/cooldown_engine.cpp
    src/core/input_matcher.cpp
    src/core/live_edit.cpp
    src/core/json.cpp
    src/core/json_value.cpp
    src/core/utf8.cpp
    src/core/profile.cpp
    src/core/profile_fields.cpp
    src/core/profile_serialize.cpp
    src/core/runtime_instances.cpp
)
target_include_directories(layerc_core PUBLIC src/core/include)
target_compile_features(layerc_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(layerc_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(layerc_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_library(layerc_windows STATIC
        src/platform/windows.cpp
        src/platform/window_discovery.cpp
        src/platform/text.cpp
        src/platform/profile_store.cpp
        src/platform/live_channel.cpp
    )
    target_include_directories(layerc_windows PUBLIC
        src/platform/include
        src/core/include
    )
    target_link_libraries(layerc_windows PUBLIC
        layerc_core user32 advapi32 shell32 ole32 gdi32 shcore)
    target_compile_definitions(layerc_windows PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    if(MSVC)
        target_compile_options(layerc_windows PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(layerc_windows PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(WIN32 AND LAYERC_BUILD_DAEMON)
    add_executable(layerd WIN32
        src/daemon/main.cpp
        src/daemon/daemon_app.cpp
        src/daemon/daemon_sessions.cpp
        src/daemon/daemon_input.cpp
        src/daemon/daemon_overlays.cpp
        src/daemon/daemon_live_edit.cpp
        src/daemon/overlay_window.cpp
        src/daemon/overlay_render.cpp
    )
    target_include_directories(layerd PRIVATE src/daemon)
    target_link_libraries(layerd PRIVATE
        layerc_core layerc_windows shell32 user32 gdi32 xinput9_1_0)
    target_compile_definitions(layerd PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(layerd PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_link_libraries(layerd PRIVATE delayimp)
        target_link_options(layerd PRIVATE
            /DELAYLOAD:xinput9_1_0.dll
        )
    else()
        target_compile_options(layerd PRIVATE -Wall -Wextra -Wpedantic)
        if(MINGW)
            target_link_options(layerd PRIVATE -municode)
        endif()
    endif()
    set_target_properties(layerd PROPERTIES OUTPUT_NAME "layerd")
endif()

if(WIN32 AND LAYERC_BUILD_EDITOR)
    add_executable(layere WIN32
        src/editor/main.cpp
        src/editor/editor_dialog.cpp
        src/editor/editor_layout.cpp
        src/editor/editor_profiles.cpp
        src/editor/editor_profile_support.cpp
        src/editor/editor_cooldowns.cpp
        src/editor/editor_bars.cpp
        src/editor/editor_tasks.cpp
        src/editor/editor_theme.cpp
        src/editor/editor_draw.cpp
        src/editor/editor_appearance.cpp
        src/editor/editor_color_picker.cpp
        src/editor/editor_controls.cpp
        src/editor/editor_live_edit.cpp
        src/editor/editor_resources.rc
    )
    target_include_directories(layere PRIVATE src/editor)
    target_link_libraries(layere PRIVATE
        layerc_core
        layerc_windows
        comctl32
        comdlg32
        dwmapi
        shell32
        uxtheme
        user32
        gdi32
    )
    target_compile_definitions(layere PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(layere PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(layere PRIVATE -Wall -Wextra -Wpedantic)
        if(MINGW)
            target_link_options(layere PRIVATE -municode)
        endif()
    endif()
    set_target_properties(layere PROPERTIES OUTPUT_NAME "layere")
endif()

if(TARGET layerd)
    install(TARGETS layerd RUNTIME DESTINATION . COMPONENT Runtime)
endif()
if(TARGET layere)
    install(TARGETS layere RUNTIME DESTINATION . COMPONENT Runtime)
endif()

if(LAYERC_BUILD_TESTS)
    enable_testing()
    add_executable(layerc_core_tests
        tests/core_tests.cpp
        tests/test_support.cpp
        tests/cooldown_input_tests.cpp
        tests/profile_tests.cpp
        tests/runtime_json_tests.cpp
        tests/live_edit_tests.cpp
    )
    target_link_libraries(layerc_core_tests PRIVATE layerc_core)
    add_test(NAME layerc_core_tests COMMAND layerc_core_tests)
    set_tests_properties(layerc_core_tests PROPERTIES TIMEOUT 30)

    if(WIN32)
        add_executable(lctest WIN32 tools/lctest.cpp)
        target_compile_definitions(lctest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MINGW)
            target_link_options(lctest PRIVATE -municode)
        endif()
        set_target_properties(lctest PROPERTIES OUTPUT_NAME "lctest")

        add_executable(layerc_overlay_tests
            tests/overlay_window_tests.cpp
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
        if(MSVC)
            target_compile_options(layerc_overlay_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(layerc_overlay_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_dependencies(layerc_overlay_tests lctest)
        add_test(NAME layerc_overlay_tests COMMAND layerc_overlay_tests $<TARGET_FILE:lctest>)
        set_tests_properties(layerc_overlay_tests PROPERTIES
            TIMEOUT 45
            SKIP_RETURN_CODE 77)

        add_executable(layerc_platform_tests tests/platform_live_tests.cpp)
        target_link_libraries(layerc_platform_tests PRIVATE layerc_core layerc_windows)
        target_compile_definitions(layerc_platform_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(layerc_platform_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(layerc_platform_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME layerc_platform_tests COMMAND layerc_platform_tests)
        set_tests_properties(layerc_platform_tests PROPERTIES TIMEOUT 30)

        add_executable(layerc_editor_tests
            tests/editor_dialog_tests.cpp
            src/editor/editor_theme.cpp
            src/editor/editor_resources.rc
        )
        target_include_directories(layerc_editor_tests PRIVATE src/editor)
        target_link_libraries(layerc_editor_tests PRIVATE
            layerc_core layerc_windows comctl32 dwmapi uxtheme user32 gdi32)
        target_compile_definitions(layerc_editor_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(layerc_editor_tests PRIVATE
                /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(layerc_editor_tests PRIVATE
                -Wall -Wextra -Wpedantic)
        endif()
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
                layerc_editor_tests
                PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${LAYERC_MINGW_RUNTIME_DIR}"
            )
        endif()
    endif()
endif()
