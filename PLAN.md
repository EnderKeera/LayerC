# LayerC Implementation Plan

## Summary

LayerC is an open-source C++20 cooldown overlay for Windows 10 22H2 and Windows 11 x64. Version 1 will do one job well: detect configured keyboard, mouse, or controller combinations while a game is foreground and display lightweight cooldown panels attached to that game window.

Version 1 consists of:

- `layere.exe`: a native Win32 resource-based editor, opened only while creating or changing profiles.
- `layerd.exe`: one native Win32 process containing the tray menu, game detection, input handling, cooldown state, and all desktop overlay windows.
- Local, versioned JSON profiles stored per game.

Layerd is one process, not a parent that launches one child runtime per game. Each detected game has an independent logical session inside the daemon. LayerC uses documented Windows APIs and does not inject into games, read game memory, install drivers, require administrator access, evade anti-cheat, conceal itself, or synthesize gameplay input.

## Version 1

### Daemon and game lifecycle

- Offer an opt-in per-user startup entry; otherwise layerd runs only when launched manually.
- Provide tray actions for opening the editor, enabling/disabling overlays, reloading profiles, and exiting completely.
- Match a game using its executable path, with optional executable name, window class, and title filters for launcher or multi-window cases.
- Select the largest visible matching top-level window by default and allow an explicit window selector in the editor.
- Track the game client rectangle, foreground state, DPI, monitor, resize, minimize, and process exit using out-of-process WinEvents with low-frequency validation.
- Create an independent `GameSession` for every detected profile, but evaluate input only for the foreground game.
- Hide the overlay when its game is minimized or closed. While a matched game is backgrounded, retain its game-owned mini overlay directly above that game in normal z-order; it remains below the foreground application.
- Keep a disposable development profile with `"game": { "global": true }` for free-floating smoke tests; it follows the foreground client/work area and is not enabled by default for game profiles.

### Input and cooldown behavior

- Use buffered Raw Input for keyboard scan codes, modifiers, mouse buttons, and wheel directions.
- Use XInput for up to four Xbox-compatible controllers, including buttons and analog trigger thresholds.
- Record combinations in the editor instead of requiring users to type key identifiers.
- Treat input passively: never block, modify, or replay user input.
- Each cooldown item supports:
  - One keyboard, mouse, or XInput combination.
  - A fixed cooldown duration.
  - A single active timer by default; sequential multi-charge stacking requires explicit stackCharges.
  - Retrigger policy: ignore while cooling down or restart the current timer.
  - Ready, cooling-down, and disabled visual states.
- Use a monotonic clock so wall-clock and timezone changes cannot alter timers.

### Desktop overlay

- Render click-through, non-activating non-topmost Win32 windows with buffered GDI. Each process overlay is an owned popup of its matched game window, so it remains above that game but never globally above unrelated applications.
- Officially support windowed and borderless-windowed games in v1. True exclusive fullscreen is not supported in v1.
- Allow multiple panels per profile. A panel is a movable container with row, column, or grid flow; cooldown items remain aligned inside it.
- Anchor panel positions to normalized game-client coordinates so they survive resolution, DPI, and monitor changes.
- Display an icon, remaining time, charge count, and configurable radial sweep or bar.
- Make the panel visual configurable per profile with style: "card" or style: "bar"; a single-cooldown bar contracts as one colored shape, while multi-cooldown panels use horizontal fill tracks.
- A bar begins full on a matched combo, shrinks with remaining time, and can hide at ready through its hideWhenReady setting.
- Render only while visible state is changing; remain event-driven while idle.

### Editor

- Show profiles and panels on the left, a preview canvas in the center, and selected properties on the right.
- Support creating, duplicating, renaming, enabling, and deleting game profiles, panels, and cooldown items.
- Let the user browse for a game executable or bind to a currently running game window.
- Provide panel drag/resize, item ordering, input-combination recording, cooldown/charge configuration, and a simulated timer preview.
- Include a small diagnostics view showing game match, selected window, foreground state, last input match, and overlay status.
- Use a rugged dark theme: charcoal rather than black, compact typography, restrained corner radii, strong borders, and cyan/amber status accents.

### Profiles and communication

- Store one UTF-8 JSON file per game with `schemaVersion`, profile identity, game matcher, panels, cooldown items, input combinations, and visual settings.
- Parse profiles into typed C++ structures and reject missing identifiers, duplicate identifiers, invalid durations, unsupported input codes, or invalid layout values.
- Save through a temporary file followed by atomic replacement and retain one last-known-good backup.
- Use a versioned current-user-only named pipe for editor-to-daemon reload, live preview, diagnostics, and graceful shutdown messages.
- Watch the profile directory as a fallback so manually copied valid JSON files are reloaded.
- Include no network access, telemetry, automatic downloads, executable profile content, scripting, database, or cloud account in v1.

## Public interfaces

Keep platform-independent behavior behind these boundaries:

- `GameProfile`, `GameMatch`, `Panel`, `CooldownDefinition`, `InputChord`, `GameSession`, and `CooldownState` are the shared data types.
- `IProfileStore` loads, validates, saves, and migrates profiles.
- `IGameTracker` reports matched processes and window/client state.
- `IInputSource` emits normalized keyboard, mouse, and controller events.
- `ICooldownEngine` consumes input events and produces immutable cooldown-state snapshots.
- `IRenderBackend` receives panels plus state snapshots without knowing how input was captured.

The interfaces must not expose Win32 handles to the cooldown engine or editor UI types to the daemon. This keeps the editor replaceable and preserves a path to later Linux backends.

Versioned extensions add new sources without changing the cooldown engine contract:

- V1.1 adds `IPixelSource` and `PixelPredicate`, which emit the same normalized trigger events as `IInputSource`.
- V2.1 adds `ITemplateMatcher`, which evaluates preprocessed reference images and reports confidence through `IPixelSource`.

## Memory and performance targets

The editor footprint is not part of gameplay targets because the editor is normally closed while playing.

Provisional v1 daemon budgets on the reference machine:

| Scenario | Private bytes | Working set | CPU |
| --- | ---: | ---: | ---: |
| Layerd running, no game | at most 12 MB | at most 20 MB | under 0.2% average |
| Foreground game with one profile | at most 20 MB | at most 32 MB | under 0.5% average |
| Eight animated panels | at most 28 MB | at most 45 MB | under 2% average |

Implementation rules supporting these targets:

- Keep editor UI code out of the daemon process.
- Use one Win32 message thread; add no worker thread until measurement proves it is necessary.
- Load icons lazily, decode them once at display size, and release assets for inactive profiles.
- Cache text formats and brushes.
- Avoid busy loops and stop controller polling when no configured game is foreground.
- Do not initialize Windows Graphics Capture or allocate capture textures in v1.

Measure private bytes, working set, thread count, thread cycle time, CPU, input-to-render latency, GPU-engine utilization, and VRAM allocation rather than relying on estimated process size alone. Run the benchmark on at least one integrated-GPU system and one discrete-GPU system.

## Test and acceptance plan

- Unit-test JSON parsing, validation, atomic recovery, input-chord matching, retrigger policies, charge recovery, monotonic timing, and session independence.
- Fuzz profile loading with malformed JSON, extreme values, duplicate IDs, invalid input codes, and interrupted saves.
- Build a small Direct3D test-game harness supporting windowed/borderless modes, resize, minimize, focus changes, DPI changes, and movement across mixed-DPI monitors.
- Verify keyboard, mouse, wheel, and four XInput controllers while foreground and confirm that unrelated foreground applications never trigger cooldowns.
- Test multiple matching processes, launcher-to-game transitions, game restarts, daemon restarts, profile reloads, and corrupted last-written profiles.
- Confirm overlays never take focus, intercept clicks, or remain visible after the game is hidden or closed.
- Accept v1 when:
  - A matched game receives its overlay within two seconds of its usable window appearing.
  - An input match updates the overlay by the next rendered frame.
  - Panels remain aligned within two physical pixels during resize, movement, and DPI transitions.
  - Idle and active memory/CPU remain within the stated budgets on the reference machine.
  - A corrupt profile cannot crash the daemon or replace the last-known-good profile.

## Version 1.1: simple pixel predicates

V1.1 is a focused opt-in update after the input-only v1 meets its acceptance criteria. It adds visual confirmation without expanding into general image recognition.

Implementation status: the native V1.1 path is implemented. LayerE launches
the short-lived `layercal.exe` calibration utility, while LayerD owns runtime
capture and recognition. The checked-in benchmark workflow is the release
evidence mechanism; integrated- and discrete-GPU result files are still
machine-specific release artifacts rather than source-controlled claims.

- Capture only the active foreground game HWND using `Windows.Graphics.Capture`; windowed and borderless modes remain the supported targets.
- Allow at most four normalized regions per profile and sample at 5, 10, or 15 Hz.
- Support average RGB, brightness threshold, and percentage of pixels near a selected color.
- Support pixel-only triggers and input-plus-pixel confirmation with stable-sample debounce.
- Add an editor calibration view for drawing a region, sampling a color, selecting tolerance/rate, and previewing predicate state.
- Start capture only when an active profile contains enabled pixel rules; input-only profiles allocate no capture device, textures, or capture thread.
- Use a one-buffer newest-frame WGC pool because older frames are intentionally
  dropped between 5-15 Hz polls. Low and Balanced keep CPU readback and
  evaluation bounded to aspect-preserving 64- and 256-pixel grids, with compact
  sampled-row/column staging instead of a full-window CPU texture; Exact reads
  every captured pixel and carries no performance guarantee. GPU allocation is
  recorded separately. Shared HWNDs use independent quality buckets, and the
  Desktop Duplication staging copy is limited to the window intersection,
  transforms rotated outputs, and marks LayerC-overlay intersections
  unavailable instead of sampling masked pixels.
- Recreate capture after game resize, window replacement, device loss, or capture-session failure.
- Treat black/protected frames and unavailable capture as an explicit unavailable state, never as a successful trigger.
- Exclude OCR, template matching, motion detection, automatic region discovery, and exclusive-fullscreen support.

Provisional V1.1 active-capture budget at 15 Hz is at most 35 MB private bytes, 50 MB working set, and 3% average CPU on the reference machine; GPU memory is recorded separately because full-window capture surfaces scale with game resolution.

V1.1 tests add synthetic SDR and HDR-tonemapped frames, threshold boundaries, noisy regions, stable-sample debounce, input-confirmation timeouts, resize/recreation, black frames, protected content, and 5/10/15 Hz performance. Acceptance requires capture failure to leave input triggers and overlays functioning normally.
Live-desktop GDI snapshot coverage is an explicit interactive integration test;
the default capture-math suite remains deterministic on headless CI.

Run `tools\benchmark_v11.ps1` for `low` and `balanced` on an integrated-GPU
system and a discrete-GPU system. The script refuses a false pass unless the
hardware capture probe first receives a real frame, and records its evidence
as JSON under `out\benchmarks`.

## Deferred roadmap

### Version 2.1: Windows detection and fullscreen expansion

- Add user-captured reference images and normalized template matching with configurable confidence, scale tolerance, and color tolerance.
- Allow multiple reference templates for one visual state and require configurable consecutive matches before emitting a trigger.
- Preprocess and cache templates, expose live confidence diagnostics in the editor, and retain the V1.1 rule that inactive/input-only profiles allocate no capture resources.
- Exclude OCR and automatic object discovery from V2.1.
- Actual Xbox Game Bar C++/WinRT MSIX companion for exclusive-fullscreen coverage, with up to eight independently pinned panel widgets and App Service communication.
- Declarative `all`/`any`/`not` condition graphs, shared cooldown groups, refunds, resets, conditional reductions, and independent charge recovery.

### Version 2.2: Portability and sharing

- Validated `.layercprofile` import/export bundles with assets, manifest, and checksums.
- Generic HID/controller support beyond XInput.
- Optional isolated per-game runtime for profiles that demonstrate stability problems.
- Linux editor/daemon port for X11, followed by a gamescope feasibility implementation.

### Version 3

- Wayland support through XDG portals with an explicit compositor capability matrix.
- Community profile distribution only after signing, moderation, update, and trust requirements are specified.
- Sandboxed WebAssembly extensions only if real profiles cannot be represented by the declarative v2 rule system.

## Assumptions

- Version 1 prioritizes reliable keyboard/mouse/controller cooldown overlays over fullscreen, visual recognition, automation, sharing, and cross-platform support.
- The portable ZIP is the only v1 distribution; MSIX packaging begins with the Game Bar work in v2.1.
- Memory numbers are acceptance budgets to validate with a prototype, not claims that the first build will meet automatically.
- V1.1 means simple numeric pixel predicates; V2.1 means user-provided image/template matching, not OCR.
- `SolOG.md` and `Luna+.md` remain historical source notes; this file is the canonical merged plan.
