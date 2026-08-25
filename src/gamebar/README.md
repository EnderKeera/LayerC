# LayerC Game Bar companion package

LayerD remains authoritative and publishes immutable, bounded JSON snapshots.
The companion performs no capture, gameplay input processing, or cooldown
mutation.

- App Service: `LayerC.GameBar.v1` (same-package UWP provider)
- Snapshot limit: 64 KiB, including final serialized size
- Stale-data cutoff: 2 seconds
- Widget contract: eight standard panel slots plus one settings widget
- Host project: `widget/LayerC.GameBar.vcxproj` (C++/WinRT UWP XAML)
- Full-trust launch: packaged `layerd.exe`, single-instance

`Package.appxmanifest` is the x64 source manifest. `StageGameBarPackage.ps1`
combines the UWP payload (XBF/PRI/WinMD, public and private Xbox Game Bar SDK
metadata/runtime) with LayerD, LayerE, LayerCal, LayerWorker, and the complete
x64 desktop VC runtime DLL set. The explicit CMake target
`layerc_gamebar_msix` produces the unsigned `LayerC.GameBar.v1.msix`; the
portable ZIP remains desktop-only. Official
distribution still requires external certificate signing and the documented
Game Bar lifecycle/resource/fullscreen matrix.
