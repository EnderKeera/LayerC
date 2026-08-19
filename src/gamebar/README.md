# LayerC Game Bar companion package

The daemon remains authoritative and publishes immutable framed JSON snapshots.
The companion performs no capture, gameplay input processing, or cooldown mutation.

- App Service: LayerC.GameBar.v1
- Local endpoint: \\\\.\pipe\LayerC.GameBar.v1
- Snapshot limit: 64 KiB
- Stale-data cutoff: 2 seconds
- Widget contract: eight panel slots plus one settings slot; Game Bar widget registration is supplied by the Xbox Game Bar SDK/host manifest extension, not the generic Windows 10 AppX schema.

Package.appxmanifest is the source manifest; MSIX staging renames it to AppxManifest.xml. The C++/WinRT host builds as layerc-gamebar.exe and the optional CMake target layerc_gamebar_msix produces an unsigned MSIX. Official distribution still requires signing and Game Bar host smoke coverage on a supported Windows image.
