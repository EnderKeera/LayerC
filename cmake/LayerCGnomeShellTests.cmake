# gjs fixtures for the GNOME Shell extension (CTest label gjs). Pure modules
# run directly; the glue runs against fake Shell objects on a private
# dbus-run-session bus. GIO_USE_VFS=local keeps gvfs off that bus.
add_test(NAME layerc_linux_gnome_extension_report_tests
    COMMAND bash "${PROJECT_SOURCE_DIR}/tools/gjs-test-session.sh"
        "${PROJECT_SOURCE_DIR}/tests/gjs/test_report.js")
add_test(NAME layerc_linux_gnome_extension_overlay_tests
    COMMAND bash "${PROJECT_SOURCE_DIR}/tools/gjs-test-session.sh"
        "${PROJECT_SOURCE_DIR}/tests/gjs/test_overlay.js")
add_test(NAME layerc_linux_gnome_extension_network_tests
    COMMAND bash "${PROJECT_SOURCE_DIR}/tools/gjs-test-session.sh"
        "${PROJECT_SOURCE_DIR}/tests/gjs/test_no_network.js")
add_test(NAME layerc_linux_gnome_extension_shell_tests
    COMMAND bash "${PROJECT_SOURCE_DIR}/tools/dbus-test-session.sh"
        bash "${PROJECT_SOURCE_DIR}/tools/gjs-test-session.sh"
        "${PROJECT_SOURCE_DIR}/tests/gjs/test_extension.js")
set_tests_properties(
    layerc_linux_gnome_extension_report_tests
    layerc_linux_gnome_extension_overlay_tests
    layerc_linux_gnome_extension_network_tests
    layerc_linux_gnome_extension_shell_tests
    PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "gjs" ENVIRONMENT "GIO_USE_VFS=local")

# Installs the extension component into a disposable prefix and checks the
# datadir layout; never touches the system data directory.
if(LAYERC_INSTALL_GNOME_EXTENSION)
    add_test(NAME layerc_linux_gnome_extension_install_tests
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DPREFIX=${PROJECT_BINARY_DIR}/gnome-extension-install-test"
            "-DDATADIR=${CMAKE_INSTALL_DATADIR}"
            "-DUUID=${LAYERC_GNOME_EXTENSION_UUID}"
            -P "${PROJECT_SOURCE_DIR}/tests/gjs/check_install.cmake")
    set_tests_properties(layerc_linux_gnome_extension_install_tests PROPERTIES
        TIMEOUT 30 LABELS "gjs")
endif()
