# LayerC

LayerC is a lightweight, open-source Windows cooldown overlay. It uses a
dependency-free C++20 core, typed JSON profiles, cooldown/input state machines,
native game matching, Raw Input/XInput handling, click-through buffered-GDI
panels, opt-in visual predicates, a native editor and calibration utility,
tests, and a disposable game harness.

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

The install step stages the three production programs, `layerd.exe`,
`layere.exe`, and `layercal.exe`, in `out/layerc`. Build-tree executables remain under
`build/vs2026/<Configuration>`; `out/layerc` changes only during the install
step or the complete workflow preset.

For the standard CMake configure, Release build, test, and install sequence as
one CMake-native command, run:

```powershell
cmake --workflow --preset vs2026-release
```

This workflow uses CMake's native Workflow and Package Presets. It configures,
builds, tests, installs to `out\layerc`, and emits the versioned portable ZIP
`build\vs2026\LayerC-2.1.0-windows-x64.zip`. A future Linux configure preset can
reuse the same build, test, install, and package structure without requiring a
platform-specific orchestration script. Close LayerE and LayerD first on
Windows so their executable files are not locked during the build or install.

The test targets have separate jobs:

- `layerc_core_tests` covers profile parsing, serialization, cooldown/input behavior, and runtime state without creating windows.
- `layerc_capture_tests` covers normalized fallback bounds and the bounded
  Low/Balanced/Exact sampling contract without requiring a GPU.
- `layerc_capture_probe` is a manual hardware smoke tool used by the V1.1
  benchmark to prove that WGC or Desktop Duplication returned a real frame.
- `lctest` is a tiny disposable fake game window that matches `examples/lctest.json`; press Shift+E while it is foreground to exercise the Dash cooldown.
- `layerc_overlay_tests` launches `lctest` and verifies real HWND attachment, overlay behavior, capture exclusion, and profile matching.
- `layerc_platform_tests` verifies protected last-valid profile backups,
  canonical Windows profile identities, authenticated peers, and framed
  overlapped named-pipe messaging.
- `layerc_editor_tests` verifies native dialog default-button and checkbox
  semantics after dark theming, plus coalesced and bounded undo/redo history.

Each matched game keeps its own non-topmost, game-owned mini overlay. Switching apps leaves the cooldown running behind the foreground app; returning to the game shows its remaining time. Minimizing or closing that game hides its overlay.

## Editor and live updates

Opening the single LayerE instance starts or connects to the single LayerD
instance. Existing
profiles autosave valid text and numeric edits after 400 ms; discrete changes
such as colors, bindings, add/remove actions, and checkboxes save immediately.
A new profile remains a draft until the first explicit Save. Ctrl+Z and Ctrl+Y
provide a 50-transaction in-memory undo/redo history. Atomic replacement keeps
the previous parseable profile beside it as `<profile>.json.bak`.

LayerE and LayerD remain separate native processes. A versioned, length-prefixed
UTF-8 protocol carries profile revisions over a current-user-only named pipe.
Before exchanging profile data, both ends verify the peer process: LayerE
requires the sibling `layerd.exe`, and LayerD requires the sibling
`layere.exe`, under the same user and logon session.
LayerD diffs revisions in place: visual, placement, name, and binding edits keep
active cooldown timelines; only a cooldown whose runtime definition changes is
reset. The old reload event remains as a fallback for manual file edits or an
unavailable live channel. The transport sits behind an interface so a future
Linux build can use a Unix-domain socket without changing the protocol or core
reconciliation logic.

**Edit on Window** selects one exact matching HWND. Dragging the editor preview
moves the temporary overlay on that window, and direct changes are mirrored
back into the editor. **Enable Direct Drag** is opt-in for each edit session; it
removes click-through only from the visible bar windows, keeps them
non-activating, and restores normal behavior on editor disconnect or a
one-second lease timeout. LayerC does not inject code, hook graphics APIs, read
game memory, install a driver, or synthesize game input. That design reduces
risk but cannot guarantee compatibility with every anti-cheat. On-window
editing supports windowed and borderless-windowed targets, not exclusive
fullscreen compositor bypass.

The native editor can also be built with MinGW. Set `MINGW_ROOT` to the
MinGW-w64 installation and use the checked-in preset, with Ninja available on
`PATH`:

```powershell
$env:MINGW_ROOT = "C:/path/to/mingw64"
cmake --workflow --preset mingw-ninja
```

This build needs the matching MinGW GCC runtime DLLs at deployment time unless that toolchain is configured for static runtimes; it does not need Qt.

## Visual recognition (V1.1 and V2.1.0)

Visual recognition is opt-in per cooldown. Choose **Calibrate** in LayerE,
select a matching game window, draw a normalized region, and configure one of
three numeric predicates: average color, brightness above/below a threshold,
or minimum target-color coverage. V2.1 also provides **Template match**: capture
the selected game region or import up to four PNG references, set confidence
and scale tolerance, and use **Mask** to left-drag transparent/ignored pixels or
right-drag them opaque again. The drawn rectangle defines expected size and,
for both capture and import, LayerCal searches a centered area up to twice as
large. Flat references are rejected because they cannot identify a position;
PNG alpha is preserved. Live template confidence is paced to two evaluations
per second so Exact does not block every capture tick. `layercal.exe` is a
short-lived native child of LayerE; LayerD performs recognition during gameplay.

LayerC stores visual data separately at
`profiles\_visual\<profile filename>.json`. V2.1 reads schema-v1 base/visual
files and writes typed schema v2 after an explicit compatibility warning.
Template PNGs are metadata-free, content-addressed files under
`profiles\_visual\assets\<sha256>.png`; their hash is verified before use.
Collection deletes orphans only after every profile, sidecar, and backup has
been parsed successfully. This preserves the base input binding. Pixel-only
mode normally replaces input semantically,
while Confirm Input arms a bounded confirmation window. Disabling the visual
assignment or the global visual-recognition option immediately restores normal
input routing. Both modes can explicitly choose `triggerFromInput` when
capture is unavailable, inactive, or still starting before its first frame.

LayerE commits a base profile and its visual sidecar as one protected bundle:
the replacement order and rollback keep the on-disk pair consistent if either
file cannot be replaced. A small `.state` marker is published before visual
removal and remains authoritative until a later visual bundle is active, so a
crash cannot reactivate stale sidecar bytes or create an ID mismatch. Missing
and removed sidecars remain input-only. Two invalid copies
deliberately block every trigger in that profile as a fail-safe until the
sidecar is repaired or removed; the session and overlays remain available so
the diagnostic is visible.

LayerD starts capture workers only for the foreground HWND when an enabled
visual observation needs them. Profiles sharing an HWND are bucketed by quality, so a
Low or Balanced assignment is never promoted to Exact by another profile. WGC
is primary. Desktop Duplication is a
foreground-only fallback limited to the window portion on its
largest-intersection monitor. Rotated outputs are transformed back into logical
desktop orientation, and rules touching LayerC overlay pixels are marked
unavailable rather than evaluated against masked black. LayerE/LayerD report
that degraded mode.
Runtime recognition has no WARP or GDI capture fallback: without hardware
D3D11, visual state is unavailable and the assignment's configured failure
policy applies. LayerCal alone may take one bounded, foreground screen
snapshot if Desktop Duplication cannot produce its initial frame on a fully
static desktop.

Low and Balanced read back aspect-preserving grids bounded to 64 and 256
pixels respectively. Their CPU-readable D3D texture contains only the sampled
source rows (or columns on rotated outputs), rather than a full-window copy.
WGC keeps one newest-frame buffer because LayerC intentionally drops older
frames between its 5-15 Hz polls. Exact reads every pixel and has no V1.1
performance guarantee. Pixel evaluation runs on its quality-specific
per-window worker, so an Exact scan can reduce visual sample throughput without
blocking LayerD's input and overlay message loop. Capture resizes recreate the
WGC frame pool. An invalid primary sidecar recovers from `.bak` when possible.

Template matching is a bounded platform-neutral coarse-to-fine CPU pass. Low
normally retains at most a 32-pixel reference axis, Balanced at most 64, and
only the best eight structural candidates receive RGB refinement. Sparse masks
may retain the source image while constructing the same bounded sample set so
they do not lose the required 64 opaque pixels. LayerD shares decoded reference
sets and retains only the representation the selected quality can consume;
full-resolution assets remain on disk and in the short-lived calibrator.
Missing or corrupt assets make only that observation unavailable, so ordinary
input and overlays remain usable.

The default `layerc_capture_tests` target contains only deterministic capture
math and runs on headless or locked CI sessions. The one-shot GDI calibration
snapshot test is built separately and is registered only when configuring with
`-DLAYERC_ENABLE_INTERACTIVE_CAPTURE_TESTS=ON` on an unlocked desktop.

Input confirmation accepts only a recent stable measurement. Its freshness
window is three sample intervals (200 ms at 15 Hz, 600 ms at 5 Hz); an older
stable result arms the normal bounded confirmation wait rather than confirming
from stale pixels.

### Visual release benchmark

Build Release with tests enabled, close any existing LayerD, then run both
guaranteed qualities:

```powershell
.\tools\benchmark_v11.ps1 -Quality low
.\tools\benchmark_v11.ps1 -Quality balanced
```

For the V2.1 worst-case workload (four active template states with four
references each), run the same isolated harness with:

```powershell
.\tools\benchmark_v11.ps1 -Workload v21-template -Quality low
.\tools\benchmark_v11.ps1 -Workload v21-template -Quality balanced
```

The isolated workflow creates a temporary lctest profile and first requires a
real hardware capture frame. The V2.1 workload must also observe a successful
template-triggered overlay before it can measure the actual LayerD path at
15 Hz; an idle capture worker cannot pass. It writes JSON evidence under
`out\benchmarks` with adapter/driver data, backend,
frame dimensions, private memory, working set, CPU, threads, and GPU-local
memory when Windows exposes that counter. The gate is 35 MiB private, 50 MiB
working set, and 3% average CPU. Run both qualities on at least one integrated
GPU and one discrete GPU; Exact is intentionally excluded.

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
