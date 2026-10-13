# Lexical tripwire for passive production input. Test event writers live under
# tests/, never in this source glob or any product target.
if(NOT DEFINED LAYERC_SOURCE_DIR)
    message(FATAL_ERROR "LAYERC_SOURCE_DIR is required")
endif()
file(GLOB sources
    "${LAYERC_SOURCE_DIR}/src/platform/linux/evdev_*.hpp"
    "${LAYERC_SOURCE_DIR}/src/platform/linux/evdev_*.cpp")
if(NOT sources)
    message(FATAL_ERROR "No evdev sources found")
endif()
foreach(source IN LISTS sources)
    file(READ "${source}" contents)
    foreach(forbidden IN ITEMS EVIOCGRAB O_WRONLY O_RDWR uinput EVIOCS)
        if(contents MATCHES "${forbidden}")
            message(FATAL_ERROR "Passive evdev boundary: ${source} contains ${forbidden}")
        endif()
    endforeach()
    if(contents MATCHES "(^|[^A-Za-z0-9_])write[ \t\r\n]*\\(")
        message(FATAL_ERROR "Passive evdev boundary: ${source} contains a write call")
    endif()
endforeach()
message(STATUS "Passive evdev boundary verified")
