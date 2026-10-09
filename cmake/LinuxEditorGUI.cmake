# Initial Linux editor slice. Missing FLTK keeps existing source/headless
# builds working; release/editor CI will explicitly require this target.
find_program(LAYERC_FLTK_CONFIG
    NAMES "${CMAKE_LIBRARY_ARCHITECTURE}-fltk-config" fltk-config)
if(NOT LAYERC_FLTK_CONFIG)
    message(STATUS "Linux LayerE not built: FLTK 1.4 development files unavailable")
    return()
endif()
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --version
    OUTPUT_VARIABLE LAYERC_FLTK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if(LAYERC_FLTK_VERSION VERSION_LESS 1.4)
    message(STATUS "Linux LayerE not built: FLTK 1.4 or later required")
    return()
endif()
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --cxxflags
    OUTPUT_VARIABLE LAYERC_FLTK_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${LAYERC_FLTK_CONFIG}" --ldflags
    OUTPUT_VARIABLE LAYERC_FLTK_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(LAYERC_FLTK_CXXFLAGS UNIX_COMMAND "${LAYERC_FLTK_CXXFLAGS}")
separate_arguments(LAYERC_FLTK_LDFLAGS UNIX_COMMAND "${LAYERC_FLTK_LDFLAGS}")
set(LAYERC_EDITOR_SYSROOT "" CACHE PATH
    "Optional extracted dependency root for Linux editor development")
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
add_executable(layere src/editor/linux/main.cpp)
if(LAYERC_EDITOR_SYSROOT)
    # fltk-config omits /usr/include when it is the compiler's default root.
    target_include_directories(layere SYSTEM PRIVATE
        "${LAYERC_EDITOR_SYSROOT}/usr/include")
endif()
target_compile_options(layere PRIVATE ${LAYERC_FLTK_CXXFLAGS})
target_link_libraries(layere PRIVATE layerc_editor_linux ${LAYERC_FLTK_LDFLAGS})
layerc_enable_warnings(layere)
