# Game Bar Companion Audit

## Current status

V2.4 now has a real end-to-end Xbox Game Bar implementation rather than the
former Win32 pipe-consumer skeleton:

- `src/gamebar/widget/LayerC.GameBar.vcxproj` builds a C++/WinRT UWP XAML host
  against the pinned `Microsoft.Gaming.XboxGameBar` and
  `Microsoft.Windows.CppWinRT` NuGet packages.
- `App::OnActivated` handles `ms-gamebarwidget:` protocol activation, creates and
  retains `XboxGameBarWidget` instances, and routes the eight `PanelSlotN`
  extension IDs and the `Settings` extension ID to their XAML pages.
- `App::OnBackgroundActivated` hosts the same-package `LayerC.GameBar.v1` App
  Service. LayerD is the client and remains authoritative.
- `PanelWidgetPage` renders cooldown name, progress, remaining time, and charges;
  it responds to Game Bar theme, requested opacity, compact mode,
  click-through, and settings events.
- `SettingsWidgetPage` exposes all eight mappings with panel choice,
  click-through, and opacity controls. It sends a bounded full companion
  preference document to LayerD; LayerD validates it and commits the base,
  visual, automation, and companion documents through one profile transaction.
- `WidgetSnapshotHub` validates snapshot JSON, fans snapshots out to widget
  windows, and clears stale data after two seconds using a one-shot timer.
- `Package.appxmanifest` registers eight standard widgets, one settings widget,
  the in-process App Service, the full-trust LayerD process, and the SDK proxy
  stub. The final package includes both public and private Xbox Game Bar WinMDs;
  omitting the private WinMD was reproduced as an activation crash and fixed.
- `GameBarTransport` is active only for the expected full-MSIX package identity,
  uses asynchronous App Service operations plus a wait event and bounded
  reconnect backoff, and remains unavailable in the portable/unpackaged path.
- Companion sidecars now round-trip through profile-pack export/import, including
  copy-ID rewriting and removal of stale undeclared companion data.

## Verification completed through 2026-08-24

- Visual Studio 2026 UWP C++/XAML tools are installed and detected by CMake.
- A fresh MSVC Release build and its complete CTest matrix passed with **0
  warnings and 9/9 tests**. Install staging produced
  `build/vs2026-v24-clean/LayerC-2.4.0-windows-x64.zip`.
- A clean MinGW Release workflow passed with **0 warnings and 9/9 tests**, then
  completed install staging and produced
  `build/mingw-preset/LayerC-2.4.0-windows-x64.zip`.
- The fresh `layerc_gamebar_msix` Release target built the UWP host with **0
  warnings and 0 errors**. `makeappx` created
  `build/vs2026-v24-clean/gamebar-msix/Release/LayerC.GameBar.v1.msix` as an
  x64 package containing the widget/XBF/PRI/WinMD payload, public and private
  Game Bar SDK metadata/runtime, all four desktop executables, and the x64
  desktop VC runtime DLL set.
- Automated companion tests cover all eight slots in one snapshot, sidecar
  persistence, catalog/preferences bounds, stale expiry, package identity, and
  the unpackaged no-resource path. Bundle tests cover companion export/import,
  copy-ID rewriting, and stale undeclared-sidecar removal.
- The earlier loose Release package was registered for the current user with
  `Add-AppxPackage -Register` on 2026-08-21.
- Xbox Game Bar launched `LayerC Panel 1` as a real widget and launched the
  packaged `layerc-gamebar.exe` and full-trust `layerd.exe` processes.
- A disposable enabled `lctest` profile was created without altering existing
  profiles, then removed after the smoke. The user observed **Main/Dash panel
  data** and the **settings widget with all eight slot controls**.
- Runtime Diagnostics reports `gameBarTransport.connected` and
  `gameBarTransport.lastError` to make future package/App Service failures
  diagnosable without tracing.
- The V2.2 modular-runtime benchmark passed: 2.160 MiB private, 11.539 MiB
  working set, and 0% CPU at baseline; opening LayerE added 0 MiB private,
  0.250 MiB working set, and no threads.
- The V2.3 automation benchmark passed activation and persistent-checkpoint
  verification. The evaluator measured 16.3502 us average, 23.6 us p95, and
  1277.3 us maximum.
- The 40-sample same-machine input-to-overlay latency gate passed against the
  checked-in MinGW baseline: +0.510 ms median and -1.394 ms p95 regression.
- The V1.1 Low and Balanced visual resource runs both activated real WGC capture
  and stayed below the working-set and CPU limits, but **failed** the 35 MiB
  private-byte gate at 35.867 MiB and 35.172 MiB respectively. This gate is not
  waived; discrete-GPU coverage is also still required.

## Remaining release gates

V2.4 remains work in progress until all of the following are recorded. The
fresh x64 package has not been registered over the earlier loose package, so the
2026-08-21 smoke does not close the current-artifact runtime gates:

- fresh-package registration plus the package, Game Bar, Explorer, and LayerD
  restart/reconnection matrix;
- all eight slots under simultaneous real-widget use;
- settings **save-and-reload persistence** through the companion sidecar and
  stale-state hiding on the real widget after connection loss;
- controller navigation, pinning/click-through, compact mode, transparency, and
  per-monitor DPI behavior;
- separate LayerC bridge/widget versus Game Bar host CPU, memory, and GPU
  accounting;
- documented fullscreen/graphics API/driver compatibility matrix;
- passing V1.1 Low and Balanced resource gates on integrated and discrete GPUs;
- official MSIX signing with the externally supplied certificate.

Until those gates pass, V2.4 must not be described as a complete signed release.
