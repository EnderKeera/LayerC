# Installs only the GnomeShellExtension component into a fresh disposable
# prefix and verifies the extension lands under <datadir>/gnome-shell/extensions.
foreach(required BUILD_DIR PREFIX DATADIR UUID)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check_install.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
        --component GnomeShellExtension --prefix "${PREFIX}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "component install failed:\n${output}")
endif()

set(root "${PREFIX}/${DATADIR}/gnome-shell/extensions/${UUID}")
set(expected
    metadata.json extension.js
    lib/report.js lib/frame.js lib/overlay.js lib/service.js)
foreach(file IN LISTS expected)
    if(NOT EXISTS "${root}/${file}")
        message(FATAL_ERROR "missing installed file: ${root}/${file}")
    endif()
endforeach()

# Nothing outside the extension directory and no extra files.
file(GLOB_RECURSE installed RELATIVE "${PREFIX}" "${PREFIX}/*")
list(LENGTH installed count)
list(LENGTH expected wanted)
if(NOT count EQUAL wanted)
    message(FATAL_ERROR "unexpected install set (${count} files): ${installed}")
endif()
file(READ "${root}/metadata.json" metadata)
string(JSON uuid GET "${metadata}" uuid)
if(NOT uuid STREQUAL UUID)
    message(FATAL_ERROR "metadata uuid ${uuid} does not match ${UUID}")
endif()
file(REMOVE_RECURSE "${PREFIX}")
message(STATUS "GNOME Shell extension installs to ${DATADIR}/gnome-shell/extensions/${UUID}")
