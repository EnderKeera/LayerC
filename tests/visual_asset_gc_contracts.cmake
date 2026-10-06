cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(implementation "${SOURCE_ROOT}/src/platform/win32/visual_assets.cpp")
set(interface "${SOURCE_ROOT}/src/platform/win32/include/layerc/platform/visual_assets_windows.hpp")
file(GLOB_RECURSE production_sources
    "${SOURCE_ROOT}/src/*.cpp" "${SOURCE_ROOT}/src/*.hpp")
foreach(source IN LISTS production_sources)
    if(source STREQUAL implementation OR source STREQUAL interface)
        continue()
    endif()
    file(READ "${source}" contents)
    if(contents MATCHES "garbage_collect_(unreferenced_)?visual_assets")
        message(FATAL_ERROR
            "Automatic asset maintenance would discard interactive owners: ${source}")
    endif()
endforeach()

file(READ "${interface}" interface_contents)
if(interface_contents MATCHES "garbage_collect_visual_assets[ \t\r\n]*\\(")
    message(FATAL_ERROR "Callers must not supply an externally scanned retention set")
endif()

file(READ "${implementation}" implementation_contents)
string(FIND "${implementation_contents}"
    "VisualAssetGcReport garbage_collect_unreferenced_visual_assets()" entry)
if(entry LESS 0)
    message(FATAL_ERROR "Offline asset-maintenance entry point is missing")
endif()
string(SUBSTRING "${implementation_contents}" ${entry} -1 maintenance)
if(NOT maintenance MATCHES "commit_profile_transaction[ \t\r\n]*\\(")
    message(FATAL_ERROR "Offline collection requires recovered store writer ownership")
endif()
if(NOT maintenance MATCHES "collect_active_asset_references[ \t\r\n]*\\(")
    message(FATAL_ERROR "Offline collection must derive its references inside ownership")
endif()
message(STATUS "Visual asset maintenance is offline-only and owns recovery/scan/deletion")

if(SKIP_NEGATIVE_FIXTURES)
    return()
endif()

# Disposable source-only fixtures prove each guard rejects a reintroduced bug.
# They never compile, launch applications, or touch any profile/settings store.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef fixture_id)
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/layerc-gc-contract-${fixture_id}")
if(EXISTS "${fixture}")
    message(FATAL_ERROR "Refusing to overwrite an existing negative fixture")
endif()
set(fixture_implementation "${fixture}/src/platform/win32/visual_assets.cpp")
set(fixture_interface "${fixture}/src/platform/win32/include/layerc/platform/visual_assets_windows.hpp")
file(MAKE_DIRECTORY "${fixture}/src/editor"
    "${fixture}/src/platform/include/layerc/platform")
file(WRITE "${fixture_implementation}" "${implementation_contents}")
file(WRITE "${fixture_interface}" "${interface_contents}")

function(expect_rejected expected)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${fixture}" -DSKIP_NEGATIVE_FIXTURES=ON
        -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT error MATCHES "${expected}")
        file(REMOVE_RECURSE "${fixture}")
        message(FATAL_ERROR "Negative fixture was not rejected as expected: ${output}${error}")
    endif()
endfunction()

file(WRITE "${fixture}/src/editor/eager.cpp"
    "void eager_collection() { garbage_collect_unreferenced_visual_assets(); }\n")
expect_rejected("Automatic asset maintenance")
file(REMOVE "${fixture}/src/editor/eager.cpp")

string(REPLACE "commit_profile_transaction(" "unowned_collection("
    unowned "${implementation_contents}")
file(WRITE "${fixture_implementation}" "${unowned}")
expect_rejected("Offline collection requires recovered store writer ownership")
file(WRITE "${fixture_implementation}" "${implementation_contents}")

file(APPEND "${fixture_interface}"
    "\nnamespace layerc::platform { VisualAssetGcReport garbage_collect_visual_assets(std::span<const std::string>); }\n")
expect_rejected("Callers must not supply an externally scanned retention set")
file(REMOVE_RECURSE "${fixture}")
message(STATUS "Rejected eager collection, unowned collection and stale retention-set API fixtures")
