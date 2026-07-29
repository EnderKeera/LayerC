# LayerC

LayerC is a lightweight, open-source Windows cooldown overlay. The current implementation is the first functional v1 slice: a dependency-free C++20 core, typed JSON profiles, cooldown/input state machines, native game matching, Raw Input/XInput handling, click-through buffered-GDI panels, tests, and a disposable game harness.

See [PLAN.md](PLAN.md) for the product and release plan.

## Build

Requirements:

- Windows 10 22H2 or Windows 11
- Visual Studio 2026 with the Desktop C++ workload
- CMake 3.28 or newer

LayerD and LayerE are both native Win32 C++ applications. LayerE has no Qt or
managed-runtime dependency and is included in the normal MSVC build.

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug
cmake --build --preset vs2026-release
ctest --preset vs2026-release
cmake --install build/vs2026 --config Release --prefix out/layerc
```

The install step stages only the two production programs, `layerd.exe` and `layere.exe`, in `out/layerc`.

For the standard CMake configure, Release build, test, and install sequence as
one CMake-native command, run:

```powershell
cmake --workflow --preset vs2026-release
```

This workflow uses CMake's native Workflow Presets. A future Linux configure
preset can reuse the same build, test, and install structure without requiring
a platform-specific orchestration script. Close LayerE and LayerD first on
Windows so their executable files are not locked during the build or install.

The test targets have separate jobs:

- `layerc_core_tests` covers profile parsing, serialization, cooldown/input behavior, and runtime state without creating windows.
- `lctest` is a tiny disposable fake game window that matches `examples/lctest.json`; press Shift+E while it is foreground to exercise the Dash cooldown.
- `layerc_overlay_tests` launches `lctest` and verifies real HWND attachment, overlay behavior, capture exclusion, and profile matching.

Each matched game keeps its own non-topmost, game-owned mini overlay. Switching apps leaves the cooldown running behind the foreground app; returning to the game shows its remaining time. Minimizing or closing that game hides its overlay.

The native editor can also be built with MinGW. Keep the C++ and resource
compilers from the same toolchain:

```powershell
$mingw = "C:/path/to/mingw64"
$ninja = "C:/path/to/ninja.exe"
cmake -S . -B build/mingw -G Ninja `
  -DCMAKE_MAKE_PROGRAM="$ninja" `
  -DCMAKE_CXX_COMPILER="$mingw/bin/g++.exe" `
  -DCMAKE_RC_COMPILER="$mingw/bin/windres.exe"
cmake --build build/mingw --target layere
```

This build needs the matching MinGW GCC runtime DLLs at deployment time unless that toolchain is configured for static runtimes; it does not need Qt.

During development, set `LAYERC_PROFILE_DIR` to load profiles from a disposable directory instead of `%LOCALAPPDATA%\\LayerC\\profiles`. For example:

```powershell
$env:LAYERC_PROFILE_DIR = "$PWD\\examples"
& .\\build\\vs2026\\Debug\\layerd.exe
```
For a free-floating smoke test, keep smoke.json in the same
profile directory. It uses "game": { "global": true }, so layerd does not
attach to an executable: the panel follows the current foreground window (or
the desktop work area) and the Raw Input binding works while another app is
focused. Use the launch command above with the example directory.

Press Shift+E over any app. The lctest.json entry still exercises
the normal process-attached path when lctest.exe is foreground.

Profile notes:

- maxCharges defaults to one active timer. Set stackCharges: true only for sequential charge stacking.
- Set a panel style to bar for a horizontal progress track, or omit it/use card for the original card layout.

### Bar configuration

A bar is configured inside its panel. It starts full when triggered, shrinks
with the remaining cooldown, and can hide when ready.

~~~json
{
  "style": "bar",
  "bar": {
    "width": 360,
    "height": 30,
    "fillColor": "#50BAC8",
    "backgroundColor": "#202A31",
    "borderColor": "#80D8E8",
    "textColor": "#F1F5F7",
    "hideWhenReady": true
  }
}
~~~

Set hideWhenReady to false to keep a ready bar visible. Dimensions are
validated to 40–1200 px wide and 16–240 px high. Colors use #RRGGBB.