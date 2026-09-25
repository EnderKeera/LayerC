# Game Bar UWP widget host — SDK discovery + UWP build orchestration.
# Included only when WIN32 AND MSVC AND LAYERC_BUILD_GAMEBAR_HOST (see CMakeLists.txt).
# Per layer22 4.4, this file isolates the SDK spelunking, MSBuild, and PowerShell
# custom commands so the top-level CMakeLists stays about targets.

# Discover the Windows SDK instead of pinning one version (layer21-F5):
# prefer the VS UCRT environment, then the newest installed SDK. When the
# SDK/C++-WinRT headers are missing the host is skipped gracefully so the
# default build never fails on a machine without the exact SDK.
set(LAYERC_CPPWINRT_DIR)
set(LAYERC_WINSDK_ROOT)
set(LAYERC_WINSDK_VER)
if(DEFINED ENV{UniversalCRTSdkDir} AND DEFINED ENV{UCRTVersion})
    set(_cppwinrt_candidate "$ENV{UniversalCRTSdkDir}/Include/$ENV{UCRTVersion}/cppwinrt")
    if(EXISTS "${_cppwinrt_candidate}")
        set(LAYERC_CPPWINRT_DIR "${_cppwinrt_candidate}")
        set(LAYERC_WINSDK_ROOT "$ENV{UniversalCRTSdkDir}")
        set(LAYERC_WINSDK_VER "$ENV{UCRTVersion}")
    endif()
endif()
if(NOT LAYERC_CPPWINRT_DIR)
    foreach(_root "C:/Program Files (x86)/Windows Kits/10" "C:/Program Files/Windows Kits/10")
        if(EXISTS "${_root}/Include")
            file(GLOB _cppwinrt RELATIVE "${_root}/Include" "${_root}/Include/*/cppwinrt")
            list(SORT _cppwinrt ORDER DESCENDING)
            if(_cppwinrt)
                list(GET _cppwinrt 0 _top)
                set(LAYERC_CPPWINRT_DIR "${_root}/Include/${_top}")
                get_filename_component(LAYERC_WINSDK_VER "${_top}" PATH)
                set(LAYERC_WINSDK_ROOT "${_root}")
                break()
            endif()
        endif()
    endforeach()
endif()
file(GLOB LAYERC_UWP_TOOLSET_TARGETS
    "C:/Program Files/Microsoft Visual Studio/*/*/MSBuild/Microsoft/VC/*/Application Type/Windows Store/10.0/Platforms/x64/PlatformToolsets/*/Toolset.targets"
    "C:/Program Files (x86)/Microsoft Visual Studio/*/*/MSBuild/Microsoft/VC/*/Application Type/Windows Store/10.0/Platforms/x64/PlatformToolsets/*/Toolset.targets")
find_program(LAYERC_POWERSHELL NAMES powershell pwsh)
if(NOT LAYERC_CPPWINRT_DIR)
    message(STATUS "Game Bar host disabled: Windows SDK C++/WinRT headers not found")
elseif(NOT LAYERC_UWP_TOOLSET_TARGETS)
    message(STATUS "Game Bar host disabled: Visual Studio UWP C++/XAML tools are not installed")
elseif(NOT CMAKE_VS_MSBUILD_COMMAND OR NOT LAYERC_POWERSHELL)
    message(STATUS "Game Bar host disabled: MSBuild or PowerShell is unavailable")
else()
    message(STATUS "Game Bar UWP widget host enabled")
    set(LAYERC_GAMEBAR_WIDGET_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/gamebar-widget")
    set(LAYERC_GAMEBAR_MANIFEST
        "${LAYERC_GAMEBAR_WIDGET_ROOT}/Package.appxmanifest")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/Package.appxmanifest"
        "${LAYERC_GAMEBAR_MANIFEST}" @ONLY)
    set(LAYERC_GAMEBAR_NUGET_ROOT
        "${LAYERC_GAMEBAR_WIDGET_ROOT}/nuget")
    set(LAYERC_GAMEBAR_NUGET_PACKAGES
        "${LAYERC_GAMEBAR_NUGET_ROOT}/packages")
    set(LAYERC_GAMEBAR_WIDGET_OUTPUT_DIR
        "${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/output")
    set(LAYERC_GAMEBAR_WIDGET_MARKER
        "${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/widget-host.stamp")
    set(LAYERC_GAMEBAR_WIDGET_ASSET
        "${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/Assets/StoreLogo.png")
    set(LAYERC_GAMEBAR_WIDGET_PROJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/widget/LayerC.GameBar.vcxproj")
    # The MSBuild project owns the shared core sources it compiles; derive the
    # dependency list from it. A missing source must fail configuration, not
    # just a future widget link.
    file(STRINGS "${LAYERC_GAMEBAR_WIDGET_PROJECT}" _widget_core_items
        REGEX "ClCompile Include=.*core")
    set(LAYERC_GAMEBAR_WIDGET_CORE_SOURCES)
    foreach(_item IN LISTS _widget_core_items)
        string(REPLACE "\\" "/" _item "${_item}")
        if(_item MATCHES "core/([a-z_0-9]+[.]cpp)")
            set(_source "src/core/${CMAKE_MATCH_1}")
            if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
                message(FATAL_ERROR "${LAYERC_GAMEBAR_WIDGET_PROJECT} compiles missing ${_source}")
            endif()
            list(APPEND LAYERC_GAMEBAR_WIDGET_CORE_SOURCES "${_source}")
        endif()
    endforeach()
    set(LAYERC_GAMEBAR_WIDGET_SOURCES
        src/gamebar/widget/App.cpp
        src/gamebar/widget/App.h
        src/gamebar/widget/App.idl
        src/gamebar/widget/App.xaml
        src/gamebar/widget/PanelWidgetPage.cpp
        src/gamebar/widget/PanelWidgetPage.h
        src/gamebar/widget/PanelWidgetPage.idl
        src/gamebar/widget/PanelWidgetPage.xaml
        src/gamebar/widget/SettingsWidgetPage.cpp
        src/gamebar/widget/SettingsWidgetPage.h
        src/gamebar/widget/SettingsWidgetPage.idl
        src/gamebar/widget/SettingsWidgetPage.xaml
        src/gamebar/widget/WidgetSnapshotHub.cpp
        src/gamebar/widget/WidgetSnapshotHub.hpp
        src/gamebar/widget/pch.cpp
        src/gamebar/widget/pch.h
        src/gamebar/widget/packages.config
        src/gamebar/Package.appxmanifest
        src/gamebar/DecodeBase64Asset.ps1
        src/gamebar/Assets/StoreLogo.png.base64
        ${LAYERC_GAMEBAR_WIDGET_CORE_SOURCES})
    add_custom_command(
        OUTPUT "${LAYERC_GAMEBAR_WIDGET_MARKER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${LAYERC_GAMEBAR_NUGET_ROOT}"
            "${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/Assets"
            "${LAYERC_GAMEBAR_WIDGET_OUTPUT_DIR}"
            "${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/obj"
        COMMAND "${LAYERC_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/DecodeBase64Asset.ps1"
            -Source "${CMAKE_CURRENT_SOURCE_DIR}/src/gamebar/Assets/StoreLogo.png.base64"
            -Destination "${LAYERC_GAMEBAR_WIDGET_ASSET}"
        COMMAND "${CMAKE_VS_MSBUILD_COMMAND}"
            "${LAYERC_GAMEBAR_WIDGET_PROJECT}" /t:Restore
            /p:RestorePackagesConfig=true
            "/p:WindowsTargetPlatformVersion=${LAYERC_WINSDK_VER}"
            "/p:SolutionDir=${LAYERC_GAMEBAR_NUGET_ROOT}/"
            "/p:RestorePackagesPath=${LAYERC_GAMEBAR_NUGET_PACKAGES}"
            "/p:LayerCGameBarManifest=${LAYERC_GAMEBAR_MANIFEST}"
            "/p:LayerCGeneratedInclude=${LAYERC_GENERATED_INCLUDE_DIR}"
        COMMAND "${CMAKE_VS_MSBUILD_COMMAND}"
            "${LAYERC_GAMEBAR_WIDGET_PROJECT}" /t:Build
            "/p:Configuration=$<CONFIG>" /p:Platform=x64
            "/p:WindowsTargetPlatformVersion=${LAYERC_WINSDK_VER}"
            "/p:LayerCNuGetRoot=${LAYERC_GAMEBAR_NUGET_PACKAGES}"
            "/p:LayerCGameBarAsset=${LAYERC_GAMEBAR_WIDGET_ASSET}"
            "/p:LayerCGameBarManifest=${LAYERC_GAMEBAR_MANIFEST}"
            "/p:LayerCGeneratedInclude=${LAYERC_GENERATED_INCLUDE_DIR}"
            "/p:OutDir=${LAYERC_GAMEBAR_WIDGET_OUTPUT_DIR}/"
            "/p:IntDir=${LAYERC_GAMEBAR_WIDGET_ROOT}/$<CONFIG>/obj/"
            /p:GenerateAppxPackageOnBuild=false
        COMMAND ${CMAKE_COMMAND} -E touch
            "${LAYERC_GAMEBAR_WIDGET_MARKER}"
        DEPENDS "${LAYERC_GAMEBAR_WIDGET_PROJECT}"
            ${LAYERC_GAMEBAR_WIDGET_SOURCES}
        VERBATIM)
    add_custom_target(layerc_gamebar_host
        DEPENDS "${LAYERC_GAMEBAR_WIDGET_MARKER}")
endif()
