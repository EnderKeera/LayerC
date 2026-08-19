# LayerC Game Bar Companion (v2.4)

## Overview
The Game Bar companion is an optional Xbox Game Bar extension (C++/WinRT) that subscribes to LayerC cooldown panel snapshots and displays them in the 8 standard panel slots + 1 settings widget. It is **not** part of the portable ZIP; it ships only in the separate MSIX package.

**Key invariants** (must never be violated):
- LayerD remains the sole source of truth for cooldown state.
- No capture, input processing, or mutation happens in the companion.
- Companion reconnects on LayerD/Game Bar/Explorer restart and hides stale data after 2 seconds.
- All measurements treat companion cost separately from native LayerD budgets.

## Package Structure (new directories)
```
src/gamebar/
├── GameBarBridge.hpp          # WinRT interface + AppService contract
├── GameBarBridge.cpp
├── PanelSlotMapping.hpp       # Versioned sidecar schema
├── PanelSlotMapping.cpp
└── Resources/
    └── GameBarExtension/
        └── Package.appxmanifest
```

## 1. GameBarBridge (Core Bridge)
```cpp
// src/gamebar/GameBarBridge.hpp
#pragma once
#include <winrt/Windows.ApplicationModel.AppService.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

namespace layerc::gamebar {

struct PanelSnapshot {
    std::string panelId;                    // e.g. "cooldown-1"
    int8_t cooldownPercent;                 // 0-100
    std::string remainingText;              // "45s" or "READY"
    std::string color;                      // #RRGGBB
    bool isReady = false;
    bool isVisible = true;
};

class GameBarBridge : public winrt::implements<GameBarBridge, IGameBarBridge> {
public:
    GameBarBridge() = default;
    ~GameBarBridge();

    // Called from LayerD via framed JSON over the same-package AppService
    winrt::fire_and_forget OnLayerDSnapshot(std::string jsonSnapshot);

    // AppService contract methods
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::ApplicationModel::AppService::AppServiceConnectionStatus>
    ConnectAsync(winrt::Windows::ApplicationModel::AppService::AppServiceConnection connection);

private:
    std::mutex m_snapshotMutex;
    std::map<std::string, PanelSnapshot> m_activeSnapshots;
    bool m_reconnected = false;

    void Reconnect();
    void HideStaleData();
};
}
```

## 2. PanelSlotMapping (Versioned Sidecar)
```cpp
// src/gamebar/PanelSlotMapping.hpp
struct PanelSlotMapping {
    std::string panelId;
    std::string widgetId;           // "widget-1", "widget-8", "settings"
    bool isEnabled = true;
    bool isPinned = false;
    bool isTransparent = true;
};

struct CompanionSidecar {
    std::string version = "v1";
    std::vector<PanelSlotMapping> mappings;   // 9 entries total
    std::string lastUpdated;
};
```

## 3. MSIX Packaging Skeleton (new file)
```xml
<!-- src/gamebar/GameBarExtension/Package.appxmanifest -->
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10" 
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10" 
         IgnorableNamespaces="uap">
    <Identity Name="LayerC.GameBar.v1" Version="2.4.0.0" Publisher="CN=LayerC" Architecture="x64" />
    <Properties>
        <DisplayName>LayerC Game Bar Companion</DisplayName>
        <PublisherDisplayName>LayerC</PublisherDisplayName>
        <Description>LayerC cooldown panel companion for Xbox Game Bar</Description>
    </Properties>
    <Resources>
        <Resource Language="en-us" />
    </Resources>
    <Capabilities>
        <Capability Name="internetClient" />
    </Capabilities>
    <Extensions>
        <uap:Extension Category="windows.appService" EntryPoint="LayerC.GameBar.AppService">
            <uap:AppService Name="LayerC.GameBar.v1" />
        </uap:Extension>
        <uap:Extension Category="windows.gameBar" EntryPoint="LayerC.GameBar.Extension">
            <uap:GameBar>
                <uap:GameBarPanelSlot Name="1" />
                <!-- ... slots 2-8 + settings ... -->
            </uap:GameBar>
        </uap:Extension>
    </Extensions>
</Package>
```

## Current Status (audited)
- The typed bridge, bounded snapshot parser, daemon-owned publication transport, sidecar validation/preservation, focused tests, CMake integration, and unsigned manifest skeleton are implemented.
- The transport is a current-user/same-session named-pipe publisher with bounded framed JSON and stale-data suppression in the bridge.
- The compiled C++/WinRT widget host, real AppService client, MSIX assembly/signing, and Game Bar host smoke gates are still outstanding.
- Do not claim V2.4 is a complete shipping companion until the remaining PLAN.md Game Bar gates pass.