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

## Version 2 roadmap

### Version 2.1: Windows template recognition and automation

Scope locked on 2026-08-10. Version 2.1 is delivered as three independently
releasable stages so template recognition and automation are not held back by
Game Bar packaging:

1. **V2.1.0 - Template Recognition**
2. **V2.1.1 - Automation Graphs**
3. **V2.1.2 - Optional Game Bar Companion**

The existing native Win32 LayerD, LayerE, and LayerCal architecture remains.
LayerD continues to be the sole authority for cooldown state and overlays. The
portable ZIP remains supported throughout V2.1. Linux remains a V2.2 target, so
new matching, expression, and state-machine code must stay platform-neutral
where practical.

#### V2.1.0 - Template Recognition

Template states extend the V1.1 visual sidecar and contain:

- a normalized search region within the selected client area;
- up to four separately captured or imported PNG references;
- expected normalized reference size;
- confidence threshold, defaulting to `0.90`;
- scale tolerance from 0 to 20 percent, defaulting to plus/minus 5 percent;
- per-channel RGB tolerance, defaulting to 24;
- consecutive stable samples, defaulting to two;
- explicit repeat and unavailable-capture policies.

A profile may have at most four active template states, with at most four
references per state. V2.1 does not include rotation or perspective matching,
whole-window object discovery, or OCR.

LayerCal gains reference capture, PNG import, crop and mask editing, normalized
search-region selection, best-match inspection, and live confidence reporting.
For both capture and import, the drawn rectangle defines expected reference
size and LayerCal centers a documented two-times-larger search region around
it. Expensive template confidence previews are paced to at most two per second
so Exact evaluation cannot monopolize the dialog message loop.
PNG alpha controls the match mask: transparent pixels are ignored. Imported
images are decoded and re-encoded through WIC, metadata is stripped, malformed
or path-traversing inputs are rejected, and each reference is limited to 4 MiB
compressed, 512 by 512 pixels, and at least 64 opaque pixels. Flat or near-flat
references are rejected because they cannot establish a spatial location.

Assets are content-addressed by SHA-256 and stored below
`_visual/assets/<sha256>.png`. Assets are made durable before the referencing
sidecar is published. Garbage collection occurs only after a successful scan
of every base profile and sidecar, so an unreadable profile cannot cause a
still-referenced asset to be deleted.

Matching uses a bounded, platform-neutral, coarse-to-fine CPU implementation:

- masked luminance plus RGB reference data is preprocessed and cached;
- Low quality normally downsamples the longest reference axis to at most 32
  pixels and Balanced to at most 64; a sparse mask may retain its source image
  long enough to build an equivalent bounded sample set without losing all 64
  participating pixels;
- masked normalized correlation produces coarse candidates;
- at most the best eight candidates receive an RGB refinement pass;
- reported confidence weights structure at 70 percent and color coverage at
  30 percent;
- a fixed per-frame work budget increases search stride when necessary instead
  of allowing unbounded work;
- Exact quality is available outside the Low/Balanced release guarantee.

`PixelRuleMeasurement` is generalized into `ObservationMeasurement`, allowing
the visual engine to consume numeric predicates and template-confidence
observations through one ordered interface. Capture workers remain shared per
window and quality level. Disabled assignments, input-only profiles, minimized
targets, and inactive sessions allocate no visual capture or matcher resources.

V2.1.0 retains the V1.1 active-capture release gate at 15 Hz: at most 35 MiB
private bytes, 50 MiB working set, and 3 percent average CPU on the reference
integrated-GPU and discrete-GPU systems. The worst-case benchmark is four
simultaneously active template states with four references each. It must verify
that LayerD actually activated capture before accepting a result.

#### V2.1.1 - Automation Graphs

Automation is a bounded declarative system, not an embedded scripting runtime.
LayerE offers ordinary users presets and a Basic editor, plus an Advanced Graph
editor backed by a typed JSON AST. There is no text programming language,
looping, recursion, user-defined function, filesystem access, process launch,
network access, native extension loading, or arbitrary code execution.

Expression values include null, Boolean, signed 64-bit integer, finite number,
duration, confidence, UTF-8 string, bounded list, and bounded string-keyed map.
Record-like data uses maps in V2.1. Expressions provide arithmetic,
comparisons, Boolean operations, indexing, `min`, `max`, `clamp`, and bounded
collection operations such as `any`, `all`, `map`, and `filter`.

Event sources include input transitions, visual observations, cooldown and
group transitions, session lifecycle, transient variable changes, timers, and
event age. Evaluation is event-driven and uses monotonic timers; it must not
introduce a fixed-rate busy loop. Each root event evaluates against one
immutable state snapshot. Matching rules form one transaction:

- higher priority wins when actions conflict;
- earlier declaration wins a priority tie;
- non-conflicting actions compose;
- actions commit only after the full transaction validates.

Actions may trigger, reset, reduce, or refund cooldowns; trigger or reset
groups; update transient values and collections; create or cancel timers; and
temporarily override overlay visibility, text, colors, dimensions, anchor,
layout, and hide-ready behavior. These presentation overrides are session-local
and are never written back to profile JSON.

The initial hard limits are 64 rules and 64 transient variables per profile,
256 AST nodes at depth 16 per rule, 64 entries per list or map, 512 bytes per
string, 32 actions per rule, 32 timers per session, 8,192 node evaluations per
root event, and 32 derived events per root event. Validation rejects documents
that exceed a limit. A runtime graph error disables automation for that session
and reports the error, while legacy input bindings, cooldowns, and overlays
continue operating.

Cooldown recovery becomes an explicit enum:

- `single` preserves legacy `stackCharges == false` behavior;
- `sequential` preserves legacy `stackCharges == true` behavior;
- `parallel` gives every spent charge its own monotonic recovery deadline.

For parallel recovery, a refund cancels the most recently created outstanding
deadline. Reduction actions explicitly choose the next deadline or every
deadline.

Cooldown groups support two independent modes:

- a shared pool owns duration, charge count, recovery mode, and retrigger
  policy, while members display and consume the same underlying state;
- trigger propagation fans an event out to otherwise independent cooldowns.

A cooldown belongs to at most one shared pool and may participate in multiple
propagation groups. Nested groups and cyclic propagation are invalid.

Transient variables and timers live only for the matching game-window session
and reset when that session retires. LayerE can subscribe to an authenticated
live trace showing source events, evaluated rules, winning actions, state
changes, and failures. Tracing allocates no ongoing buffer when there is no
subscriber. A deterministic simulator replays recorded timestamped events
through the same production evaluator without requiring a live game.

#### V2.1.2 - Optional Game Bar Companion

The Game Bar companion is an optional follow-up and cannot block V2.1.0 or
V2.1.1. The portable ZIP continues to contain the normal desktop applications.
A separate full MSIX packages LayerD, LayerE, LayerCal, and a C++/WinRT Xbox
Game Bar companion.

The package declares eight fixed standard panel-widget extensions plus one
settings widget. Each panel slot subscribes to one panel in the current
foreground LayerC session, and the slot mapping is stored in the schema-v2
profile. A same-package `LayerC.GameBar.v1` App Service carries bounded,
immutable panel snapshots. LayerD remains authoritative; the widget performs
no capture, input processing, or cooldown mutation.

The companion reconnects after LayerD, Game Bar, package, and Explorer
restarts, and hides data that has been stale for more than two seconds. It must
support pinning, click-through where the Game Bar SDK permits it, transparency,
compact presentation, controller navigation, and per-monitor DPI changes.

Game Bar coverage is best-effort. LayerC detects and reports unsupported
fullscreen, graphics-API, driver, or shell combinations instead of claiming
universal exclusive-fullscreen support. CI produces an unsigned test MSIX;
official release packages are signed using an externally supplied certificate
secret.

#### V2.1 schemas and compatibility

Saving any profile in a V2.1 build upgrades its base profile and visual sidecar
to schema version 2 after a clear warning that older builds cannot reopen it.
Schema-v1 profiles remain readable and migrate in memory. Primary and backup
files retain atomic bundle semantics.

Base schema v2 adds recovery modes, cooldown groups, transient-variable
declarations, automation rules and actions, and Game Bar slot mappings. Visual
schema v2 adds observation types, template references, asset hashes, confidence
configuration, and alpha-mask semantics.

The public core abstractions include:

- `ITemplateMatcher`, `TemplateState`, `TemplateReference`, and
  `ObservationMeasurement`;
- `ExpressionValue`, `ExpressionNode`, `RuntimeEvent`, `AutomationRule`,
  `AutomationAction`, and `AutomationTransaction`;
- `CooldownRecoveryMode`, `CooldownGroup`, and parallel recovery deadlines;
- `OverlayPresentationOverrides`;
- Game Bar subscription and immutable snapshot messages.

Missing assets, missing graph references, or invalid automation disable only
the affected visual or automation path; normal input cooldowns remain usable.
Unknown schema versions, expression nodes, and action types are rejected rather
than guessed.

#### V2.1 verification and release gates

- Template tests cover masks, threshold boundaries, scale tolerance, noise,
  SDR and HDR-tonemapped frames, capture loss, unavailable regions, malformed
  assets, path traversal, content hashes, crash consistency, and safe garbage
  collection.
- Matcher performance is measured at the worst-case four-by-four workload on
  integrated and discrete GPUs for both Low and Balanced quality.
- Expression tests cover every type and operator, static validation, all hard
  limits, event ordering, priority conflicts, transaction rollback, session
  cleanup, timer monotonicity, deterministic replay, fuzzing, and property
  tests.
- Cooldown tests cover legacy migration, single, sequential, and parallel
  recovery, refunds, reductions, shared pools, propagation, reconfiguration,
  and clock discontinuities.
- UI tests verify session-local presentation overrides reset correctly and
  never alter persisted profile appearance.
- Game Bar tests cover all eight panel slots, reconnection and package
  lifecycle, stale-state hiding, controller input, click-through, DPI changes,
  and a documented fullscreen compatibility matrix.
- Every V1 and V1.1 parser, runtime, capture, overlay, editor, daemon, and
  benchmark regression remains mandatory.

V2.1 deliberately excludes OpenCV, embedded scripting runtimes, OCR, rotation
or perspective matching, automatic object discovery, cloud services,
telemetry, community distribution, and third-party extension execution.

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
