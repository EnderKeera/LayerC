layerc_add_test(layerc_linux_bus_model_tests
    SOURCES tests/linux_bus_model_tests.cpp
    LINK layerc_bus_model TIMEOUT 30)

add_test(NAME layerc_linux_kwin_script_tests
    COMMAND bash "${PROJECT_SOURCE_DIR}/tools/gjs-test-session.sh"
        "${PROJECT_SOURCE_DIR}/tests/linux_kwin_script_tests.js"
        "${PROJECT_SOURCE_DIR}/src/platform/linux/kwin_window_script.cpp")
set_tests_properties(layerc_linux_kwin_script_tests PROPERTIES
    TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "gjs")

if(LAYERC_HAVE_X11)
    # The visual service's native (portal) capture seam with a fake factory;
    # display- and bus-free, so it runs in every graphical build.
    layerc_add_test(layerc_linux_native_visual_tests
        SOURCES tests/linux_native_visual_tests.cpp
        LINK layerc_daemon_linux TIMEOUT 30)
endif()

if(TARGET layerc_bus)
    layerc_add_test(layerc_linux_dbus_service_tests
        SOURCES tests/linux_dbus_service_tests.cpp
        LINK layerc_bus NO_REGISTER)
    add_test(NAME layerc_linux_dbus_service_tests
        COMMAND bash "${PROJECT_SOURCE_DIR}/tools/dbus-test-session.sh"
            $<TARGET_FILE:layerc_linux_dbus_service_tests>)
    set_tests_properties(layerc_linux_dbus_service_tests PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "dbus")
    layerc_add_test(layerc_linux_gnome_bridge_tests
        SOURCES tests/linux_gnome_bridge_tests.cpp
        LINK layerc_bus NO_REGISTER)
    add_test(NAME layerc_linux_gnome_bridge_tests
        COMMAND bash "${PROJECT_SOURCE_DIR}/tools/dbus-test-session.sh"
            $<TARGET_FILE:layerc_linux_gnome_bridge_tests>)
    set_tests_properties(layerc_linux_gnome_bridge_tests PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "dbus")
    layerc_add_test(layerc_linux_portal_screencast_tests
        SOURCES tests/linux_portal_screencast_tests.cpp
        LINK layerc_bus NO_REGISTER)
    add_test(NAME layerc_linux_portal_screencast_tests
        COMMAND bash "${PROJECT_SOURCE_DIR}/tools/dbus-test-session.sh"
            $<TARGET_FILE:layerc_linux_portal_screencast_tests>)
    set_tests_properties(layerc_linux_portal_screencast_tests PROPERTIES
        TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "dbus")
    # BusProviders is compiled into the daemon only with the X11 session engine.
    if(LAYERC_HAVE_X11)
        layerc_add_test(layerc_linux_bus_providers_tests
            SOURCES tests/linux_bus_providers_tests.cpp
            LINK layerc_daemon_linux layerc_bus NO_REGISTER)
        add_test(NAME layerc_linux_bus_providers_tests
            COMMAND bash "${PROJECT_SOURCE_DIR}/tools/dbus-test-session.sh"
                $<TARGET_FILE:layerc_linux_bus_providers_tests>)
        set_tests_properties(layerc_linux_bus_providers_tests PROPERTIES
            TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "dbus")
        if(TARGET layerc_pipewire)
            target_compile_definitions(layerc_linux_bus_providers_tests PRIVATE
                LAYERC_TEST_PIPEWIRE=1)
        endif()
    endif()
endif()


if(TARGET layerc_pipewire)
    layerc_add_test(layerc_linux_pipewire_capture_tests
        SOURCES tests/linux_pipewire_capture_tests.cpp
        LINK layerc_pipewire LABELS dbus TIMEOUT 30)
endif()
