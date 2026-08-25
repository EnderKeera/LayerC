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
- V2.2 adds `DaemonKernel`, `CapabilityDemand`, `IRuntimeService`,
  `RuntimeResourceStats`, first-party platform backend contracts, generic HID,
  and validated profile-bundle interfaces.
- V2.3 adds the typed automation AST, runtime events, transactions, recovery
  modes, cooldown groups, presentation overrides, tracing, and deterministic
  simulation.
- V2.4 adds immutable panel-subscription messages for the optional Game Bar
  companion without moving cooldown authority out of LayerD.

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

### Version 2.1: Template recognition

V2.1 is the focused Windows template-recognition release. The implementation,
automated tests, portable package workflow, and Low/Balanced integrated- and
discrete-GPU benchmark gates are complete. A human visual click-through remains
a release checklist item rather than additional feature scope.

The existing native Win32 LayerD, LayerE, and LayerCal architecture remains.
LayerD continues to be the sole authority for cooldown state and overlays. New
matching code stays platform-neutral so later runtime and Linux backends can
reuse it.

#### V2.1 implementation

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

V2.1 retains the V1.1 active-capture release gate at 15 Hz: at most 35 MiB
private bytes, 50 MiB working set, and 3 percent average CPU on the reference
integrated-GPU and discrete-GPU systems. The worst-case benchmark is four
simultaneously active template states with four references each. It must verify
that LayerD actually activated capture before accepting a result.

### Version 2.2: Modular runtime and extensibility

V2.2 ships as one public `2.2.0` release built through three internal phases:

1. modular daemon kernel;
2. profile packs and generic HID;
3. isolated visual worker and portable platform contracts.

LayerD remains a native, single-process daemon by default. First-party modules
are statically linked but instantiated only when demanded. V2.2 adds no public
DLL/plugin ABI, automation, Game Bar, or Linux runtime. The kernel extraction
must preserve V2.1 behavior exactly before new features are added, and heavy
resources are released immediately when their final consumer disappears.

#### V2.2 modular runtime

Introduce a platform-neutral `layerc_runtime` static library containing
`DaemonKernel`, `CapabilityDemand`, `SessionId`, opaque `WindowId`,
`IRuntimeService`, `RuntimeResourceStats`, and service lifecycle/reconciliation
helpers. Windows implementations remain statically linked into LayerD:

- `InputService` owns Raw Input, global-key polling, XInput, and the lazy HID
  backend;
- `OverlayService` owns session/edit overlay HWNDs, rendering, positioning, and
  capture exclusion;
- `VisualService` owns capture workers, pixel evaluation, and measurement
  delivery;
- `TemplateEvaluator` is a lazy child of a visual worker and exists only while
  template observations are active;
- `LiveEditService` owns authenticated connection buffers, edit leases,
  heartbeats, and live messages.

`DaemonKernel` retains only the message window, tray, window hooks, game scan,
profile and settings reconciliation, reload events, session authority, service
ownership, demand calculation, and shutdown. It calculates one immutable
`CapabilityDemand` after every profile, game-window, focus, setting, or editor
state change.

Each service implements equivalent `reconcile(const CapabilityDemand &)`,
`quiesce() noexcept`, and `stats() const` behavior. An absent service is a null
`unique_ptr` and owns no worker, timer, HWND, capture device, decoded image,
queue, cache, or active IPC connection. Quiescence releases resources in the
same reconciliation cycle; there is no warm-cache grace period.

LayerD keeps one dormant current-user-only named-pipe listener and completion
event so LayerE can connect at any time. The existing 250 ms live-channel poll
timer is replaced by event-driven connection/read notification. The
`LiveEditService` object is constructed only after peer authentication.

Raw Input is registered only while a foreground or global session requires it.
XInput and generic HID activate independently. Visual workers notify the kernel
only when a new bounded snapshot exists; no idle visual polling remains.
Session objects retain authoritative cooldown, input-matcher, and visual-trigger
state. Services provide only normalized input events, measurements, and
presentation.

Activation order is session state, input baseline seeding, overlays, then visual
capture. Teardown stops visual/input producers first, hides overlays, and then
retires session state. Opening LayerE alone activates only `LiveEditService` and
must not construct capture, template, HID, or edit-overlay resources.

The expected demand tiers are:

| Active requirement | Resident capabilities |
| --- | --- |
| No matched game | Kernel, profile metadata, tray, reload events, dormant pipe listener |
| Input-only session | Input backend, cooldown session, visible overlay state |
| Pixel observation | Input tier plus `VisualService` and bounded capture state |
| Template observation | Pixel tier plus lazy evaluator and referenced assets |
| Connected editor | Requested tier plus `LiveEditService` for the connection |
| Generic HID binding | Input tier plus only the configured device backend |
| Isolated visual window | Kernel plus one bounded helper for that HWND |

Increment the editor-daemon live protocol to version 2 and require LayerD,
LayerE, and helper binaries from the same package version. Add one-shot runtime
diagnostic request/reply messages. LayerE exposes File -> Runtime Diagnostics
with Refresh and Copy JSON. Each service reports state, consumer count, owned
threads/timers/handles/HWNDs/queues/capture sessions, decoded assets and cache
bytes, last activation/quiescence, activation/fault count, and last error.
Aggregate diagnostics include private bytes, working set, handle/thread count,
CPU time, and available GPU allocation data. No continuous trace is allocated
while the dialog is closed.

#### V2.2 schema 3 and generic HID

V2.2 reads profile schemas 1 through 3. Any LayerE save upgrades the base
profile to schema 3 after the existing explicit older-build compatibility
warning; autosave and live edit cannot approve that boundary implicitly.
Schema 3 adds generic-HID binding descriptors, `visualRuntime` with values
`inline` or `isolated` and an `inline` default, plus first-party module metadata
required for bundle consistency. Persistence becomes a multi-document
transaction without regressing the existing base/visual recovery guarantees.
Unknown schemas or module kinds are rejected rather than guessed or discarded.

Persisted HID configuration is separate from compact runtime input tokens:

- `HidDeviceSelector` contains VID, PID, device version, top-level usage,
  descriptor hash, optional serial, and stable machine slot;
- `HidControlBinding` contains report ID, usage page, usage, link collection,
  activation mode, normalized threshold, and hysteresis;
- resolved `RuntimeInputToken` contains only backend, resolved device instance,
  and compact control ID.

Support buttons, arrays, hats/discrete values, absolute axes converted to
Boolean press/release events using direction/threshold/hysteresis, and relative
controls converted to momentary directional events. Continuous analog values
remain V2.3 work.

Keyboard- and mouse-class HID controls are hidden by default. An Advanced
per-binding opt-in exposes device-specific keyboard/mouse controls with a
duplicate-event warning. LayerE uses Capture then refine: select a device,
actuate a control, infer its kind/direction, then optionally edit threshold,
hysteresis, stable slot, and system-class opt-in.

Prefer serial identity. Otherwise resolve descriptor fingerprint plus a
persistent machine-local stable slot. Ambiguous identical devices make the
binding unavailable until the user reassigns the slot; LayerC never silently
chooses the first enumerated device. Slot mappings are global machine settings
and are excluded from exported packs.

The daemon enumerates or listens to generic HID only while an active binding
demands it. LayerC installs no driver, requests no administrator permission,
claims no device exclusively, and performs no input synthesis or injection.

#### V2.2 multi-profile packs

Vendor miniz 3.1.2 at its tagged upstream commit, include its MIT license and
source hashes, and compile it only into a separate bundle library used by
LayerE and tests. LayerD does not link the ZIP implementation.

A `.layercprofile` file is a standard ZIP containing `manifest.json`, one or
more base profiles, their known module sidecars, and deduplicated
content-addressed assets. Manifest schema 1 records the pack kind/version,
producing LayerC version, suggested filenames, profile IDs, module paths/types
and schema versions, sizes, and SHA-256 hashes.

Export Pack opens a checklist with the current profile selected by default,
Select All, profile count, and deduplicated asset-size summary. It exports only
current valid primary documents. Backups, removal markers, appearance/options,
HID slot mappings, logs, and runtime state are excluded. Entry order, names,
content, timestamps, and compression settings are deterministic.

Import validates the complete archive before showing choices. Limits are
128 MiB for the archive and total uncompressed data, 256 entries, 64 profiles,
the existing 256 KiB per JSON document, and 4 MiB per asset. Reject encrypted
entries, ZIP64, symlinks/reparse entries, absolute/UNC/drive paths, traversal,
alternate streams, duplicate paths, Windows case-fold collisions, undeclared
files, bad hashes, and unsupported schemas.

The validated import preview offers a profile checklist and, for each ID or
filename conflict, Replace or Import Copy with Copy selected by default. Copy
regenerates the profile ID and dependent module references while preserving
cooldown and panel IDs local to that profile.

Commit all selected profiles and shared assets as one recoverable transaction:
stage and validate every output, write a durable journal, publish immutable
assets, replace profile/module documents with write-through renames, publish a
commit marker, then remove recovery state. Startup rolls back a transaction
without a commit marker and finishes cleanup for a committed transaction.
Checksums provide integrity, not publisher identity; packs are data-only and
unsigned until V3 trust work.

#### V2.2 isolated visual runtime and portability

Add `layerworker.exe`, launched only when an active visual session requests
isolation. The per-profile Advanced option is off by default and warns about
additional process memory.

Isolation is resolved per game HWND. If any matching profile requests
isolation, that window's entire capture/pixel/template evaluation pipeline moves
to one shared helper so LayerC never captures the same HWND twice. LayerD
retains input matching, cooldowns, visual-trigger policy, overlays, profile
reconciliation, and all authoritative state. The helper returns bounded
`ObservationMeasurement` snapshots, never frames or mutable cooldown state.

Launch the same-package sibling executable as the current user in a Windows Job
with active-process limit one, kill-on-job-close, a 256 MiB process-commit cap,
and canonical executable/process validation. This is crash and resource
containment, not a security sandbox.

Use a random per-launch pipe name and nonce, current-user-only ACL, mutual
PID/path/session validation, version handshake, 1 MiB message limit, and 4 MiB
queued-data limit. Sequence numbers and QPC timestamps reject stale,
duplicated, oversized, or out-of-order measurements.

Require a 500 ms heartbeat. After two seconds without progress, terminate the
owned helper and retry once after one second. A second fault quarantines visual
evaluation for that HWND while input cooldowns and overlays continue. Runtime
Diagnostics exposes Retry. Game restart or relevant profile reload clears the
quarantine automatically.

Freeze source-level, not binary, platform contracts for game/window discovery
and focus, normalized input, overlay presentation, capture-session creation,
current-user control transport, and helper lifecycle. Platform-neutral code
uses opaque IDs, rectangles/scaling data, monotonic time, and normalized events;
Win32 handles remain inside Windows adapters. V2.2 does not implement Linux.

#### V2.2 verification and release gates

Internal Phase A extracts services with no profile, UI, overlay, timing, or IPC
behavior change. Every V2.1 test and visual benchmark passes before Phase B
adds schema 3, diagnostics, packs, and HID. Phase C adds the isolated helper and
freezes platform contracts. Only the complete result ships as LayerC 2.2.0.

Required coverage includes:

- the service-demand matrix for no game, foreground/background games, global
  profiles, editor connection/edit mode, input-only, pixel, template, HID, and
  isolation;
- idempotent reconciliation, activation/teardown ordering, failure rollback,
  reentrant notifications, and 100 activation cycles per service;
- independent cooldowns and correct foreground input for two profiles and two
  matching game windows;
- proof that opening/closing LayerE without edit mode creates no visual,
  template, HID, edit-overlay, daemon timer, or daemon worker resource;
- synthetic HID descriptors/reports covering buttons, arrays, hats, centered
  and unipolar axes, relative controls, malformed reports, hot-plug, ambiguous
  devices, slot reassignment, system-class opt-in, chords, and held baselines;
- deterministic pack round trips, shared assets, Replace/Copy mixtures, every
  transaction crash point, corrupt hashes/CRC, ZIP bombs, traversal,
  Unicode/case collisions, unsupported schemas, duplicate IDs, and missing
  modules/assets;
- helper spoofing/version/session failures, oversized/out-of-order messages,
  capture/device loss, crash, hang, retry, quarantine, manual recovery, HWND
  close, daemon shutdown, and several isolated profiles sharing one helper;
- clean MSVC and MinGW builds plus the complete CTest suite.

Resource gates remain:

- no game: at most 12 MiB private, 20 MiB working set, and 0.2 percent average
  CPU;
- existing input-only and in-process visual limits, including the 35 MiB
  private, 50 MiB working-set, and 3 percent Low/Balanced visual gate;
- opening LayerE without edit mode adds no daemon thread/timer and at most
  1 MiB private and 2 MiB working set, returning to the prior plateau on close;
- after 100 service cycles: no thread/GPU-allocation growth, no inactive
  capability handles, at most 1 MiB private-byte growth, and at most two total
  handles for the dormant listener;
- generic HID remains inside the existing input-only budget with no worker
  thread;
- isolated Low/Balanced mode is provisionally capped at 50 MiB combined private
  bytes, 70 MiB combined working set, and 3.5 percent average CPU across LayerD
  and its helper;
- median and p95 input-to-overlay latency regress by no more than 1 ms from the
  recorded V2.1 baseline.

Release artifacts include `layerd.exe`, `layere.exe`, `layercal.exe`,
`layerworker.exe`, third-party notices, the portable ZIP, updated README, and
this canonical plan. V2.2 deliberately exposes no third-party native plugin
ABI; signing, permissions, distribution, and stable external extensions remain
V3 trust work.

### Version 2.3: Automation Graphs

V2.3 is the bounded declarative automation milestone. It is not an embedded
scripting runtime. One profile may own multiple independent automations, and
the Basic editor and Advanced editor compile to the same typed JSON AST. There
is no text programming language, loop, recursion, user-defined function,
filesystem or registry access, process launch, network access, memory access,
injection, native extension loading, or arbitrary code execution.

LayerD constructs `AutomationService` only while at least one matching session
has an enabled automation with rules. Disabling or removing the last graph
releases its AST, variables, timers, trace state, evaluator allocation, and the
service-owned timer. Here a matching session means a live, non-minimized
matched game window, including one temporarily in the background: timers and
non-input rules must continue there, while only input-derived rules pause on
focus loss. Ordinary input, cooldowns, overlays, visual recognition, HID, and
live editing remain separate capabilities in the V2.2 runtime kernel.
Opening LayerE alone does not activate automation.

#### Typed graph and state

Expression values include null, Boolean, signed 64-bit integer, finite number,
duration, confidence, bounded UTF-8 string, bounded heterogeneous list, and
bounded string-keyed map. Record-like data uses maps. Expressions provide
checked arithmetic, comparisons, Boolean operations, indexing, `min`, `max`,
`clamp`, and bounded `any`, `all`, `map`, and `filter` collection operations.
Integer overflow, division by zero, non-finite values, invalid UTF-8, wrong
types, or limit violations fail closed.

Variables are declared with a type and default value. Session variables reset
when the matching game-window session retires. A declaration may opt into
persistence for any bounded value type. Persistent values are shared by all
sessions of the same profile, survive compatible live edits with the same ID
and type, and use a machine-local state file under `profiles\_state`. LayerD
coalesces mutations into an atomic checkpoint no later than one second after
the first unsaved change and performs a final flush on normal teardown;
continuous activity cannot postpone checkpoints indefinitely. Definitions and
defaults may be exported, but machine-local values and traces never enter a
profile pack.

Event sources include input press/release and demanded continuous
controller/HID values, visual observations, cooldown and group transitions,
session start/stop, variable changes, monotonic timers, and event age. Losing
foreground focus pauses only input-derived rules. Cooldowns, visual/session
state, and timers continue, so returning to a game never rewinds automation.
Continuous inputs use profile deadzone/change thresholds and a bounded rate;
relative HID movement is a momentary value event.

Each root event evaluates against one immutable state snapshot. Derived events
are bounded and carry an origin token so overlapping propagation groups cannot
cycle; each rule may fire at most once for one root. All enabled automations in
the session participate in one ordered transaction:

- higher rule priority wins a conflict on the same target;
- earlier automation and rule declaration wins a priority tie;
- non-conflicting actions compose;
- external effects commit only after the complete winning set validates;
- an error rolls back and faults only the offending automation in that session,
  then the remaining automations are re-evaluated from the original snapshot.

Reload, an explicit Retry command, or an edit re-enables a faulted automation.
A live edit takes effect on the next event boundary. Same-ID, same-type variable
values survive; timers and presentation overrides owned by a changed graph are
canceled, while unchanged graph state remains.

Actions may trigger, reset, reduce, or refund cooldowns; trigger or reset
cooldown groups; update variables and bounded collections; create or cancel
one-shot/repeating timers; and temporarily override overlay visibility, text,
colors, dimensions, anchor, layout, text visibility, and hide-ready behavior.
Named presentation overrides are session-local, carry normal conflict ordering,
and last until their optional duration expires or an explicit clear action.
They never rewrite profile appearance. Timers share the window message-loop
scheduler, have a 50 ms minimum, coalesce missed wakeups, and introduce no
fixed-rate worker or busy loop.

The initial hard limits are 64 rules and 64 variables per profile, 256 AST
nodes at depth 16 per rule, 64 entries per list or map, 512 UTF-8 bytes per
string, 32 actions per rule, 32 timers per session, 8,192 node evaluations and
32 derived events per root, a 256 KiB sidecar, a 64 KiB state file, and a 4 MiB
trace. A normal root averages no more than 0.25 ms in the release workload; a
root exceeding the 2 ms hard budget faults its offending automation.

#### Cooldown recovery and groups

Base schema 4 makes cooldown recovery explicit:

- `single` preserves legacy `stackCharges == false` behavior;
- `sequential` preserves legacy `stackCharges == true` behavior;
- `parallel` gives every spent charge its own monotonic recovery deadline.

For parallel recovery, a refund cancels the most recently created outstanding
deadline. Reduction actions explicitly choose the next deadline or every
deadline. Shared-pool groups own duration, charge count, recovery mode, and
retrigger policy, while trigger-propagation groups fan one origin-tagged event
out to otherwise independent cooldowns. A cooldown belongs to at most one
shared pool and may participate in multiple propagation groups. Nested group
references and cyclic propagation are invalid.

#### Native editor, trace, and simulator

LayerE remains native Win32. The Automation Graphs dialog provides multiple
Basic presets, an automation list, node palette, connected zoomable canvas,
clickable rule/action nodes, typed property fields, node deletion, validation,
and an Advanced whole-graph JSON editor. Basic and Advanced modes edit the same
AST. Enable/disable, preset, node, and graph changes join the existing 50-step
bundle undo/redo history and valid saved profiles autosave immediately.

An authenticated live-trace subscription reports bounded source events,
evaluated rules, winners, state changes, and errors. LayerD allocates no trace
buffer without a subscriber. `.layerctrace` import/export is sanitized,
bounded, timestamped, and data-only. Exact replay retains validated typed
source-field values, including user-authored strings and maps, so the editor
warns that trace files may contain sensitive profile variable contents. The deterministic simulator uses the
production evaluator and supports synthetic typed events, recorded traces,
play/pause, step, speed, rule breakpoints, variable/timer state, and assertions.
It requires no game or daemon.

#### Schema, packs, and release gates

The owning base document is schema 4 and declares an automation module at
schema 1. Definitions live at `profiles\_automation\<profile filename>.json`;
state lives separately under `_state`. LayerE requires the existing explicit
compatibility approval before first persistence of a legacy base. Base,
visual, and automation documents commit through one crash-recoverable bundle
transaction. A missing or invalid optional automation sidecar disables only
automation and reports the exact error; it does not block input cooldowns.
Profile packs carry compatible automation definitions/defaults and checksums,
but never `_state` files or traces.

Required release evidence includes all earlier V2.2 gates plus:

- round trips for every value, expression, event, action, recovery mode, group,
  sidecar, state file, trace, and pack module;
- hard-limit, malformed/truncated mutation, transaction rollback, conflict,
  per-automation fault, origin-cycle, timer, focus, reconfiguration, and
  deterministic replay tests;
- native dialog checks for presets, canvas notification, property controls,
  checkbox semantics, Advanced AST editing, validation, and simulator controls;
- a disposable daemon benchmark that proves AutomationService activation,
  proves a persistent checkpoint, compares inactive/active/disabled plateaus,
  and refuses a false pass;
- no automation thread, at most 1 MiB private and 2 MiB working-set incremental
  cost, no more than 0.25 ms average and 2 ms maximum per benchmark root, and
  return to the inactive resource plateau after disabling the last graph;
- clean MinGW and MSVC Release builds, the complete CTest suite, both V2.2 and
  V2.3 resource benchmarks, install staging, and the versioned portable ZIP.

Only the complete result ships as LayerC 2.3.0.
### Version 2.4: Optional Xbox Game Bar companion

The Game Bar companion is an optional follow-up and cannot block V2.1, V2.2,
or V2.3. The portable ZIP continues to contain the normal desktop applications.
A separate full MSIX packages LayerD, LayerE, LayerCal, and a C++/WinRT Xbox
Game Bar companion.

Implementation status (2026-08-25): UWP XAML host (8 panel widgets + settings), `WidgetSnapshotHub`, App Service `LayerC.GameBar.v1`, demand-driven `GameBarTransport` (State/AppServiceClient/RetryPolicy split), bounded 64 KiB snapshots with 2s stale clear, companion sidecar (`_companion` schema 1) via transactional multi-doc save and `.layercprofile` packs, `pack_detail.*`/`SidecarKind` refactor, poll consolidation to single wait-handle drain, full-trust LayerD launch, and unsigned x64 MSIX are implemented. Clean **MSVC (ci preset, sccache+UNITY)** and **MinGW-Ninja** Release builds pass **0 warnings, 10/10 tests**; `benchmark_v23` + `benchmark_v22` + `benchmark_latency` gates pass; `benchmark_v11` Low/Balanced remains borderline (35.3–35.8 MiB vs 35.0 MiB) on integrated GPU. Earlier MSIX was registered and smoke-tested with `lctest` (8 slots + settings); current x64 artifact needs re-registration and the full restart/controller/DPI/click-through/resource/fullscreen matrix. V2.4 remains **WIP** until that matrix and external signing complete.

The package declares eight fixed standard panel-widget extensions plus one
settings widget. Each panel slot subscribes to one panel in the current
foreground LayerC session, and the slot mapping is stored in a separately
versioned companion sidecar. A same-package `LayerC.GameBar.v1` App Service
carries bounded, immutable panel snapshots. LayerD remains authoritative; the
widget performs no capture, input processing, or cooldown mutation.

`GameBarBridge` remains absent when the companion is not installed or connected.
Installing the MSIX adds no gameplay thread or polling loop to the portable
LayerD path. Measurements report the incremental LayerC bridge/widget cost and
the Game Bar host's own memory, CPU, and GPU cost separately; Game Bar overhead
must never be represented as part of the native-overlay budget.

The companion reconnects after LayerD, Game Bar, package, and Explorer
restarts, and hides data that has been stale for more than two seconds. It must
support pinning, click-through where the Game Bar SDK permits it, transparency,
compact presentation, controller navigation, and per-monitor DPI changes.

Game Bar coverage is best-effort. LayerC detects and reports unsupported
fullscreen, graphics-API, driver, or shell combinations instead of claiming
universal exclusive-fullscreen support. CI produces an unsigned test MSIX;
official release packages are signed using an externally supplied certificate
secret.

### Version 2 schemas and compatibility

V2.1 already writes base and visual schema version 2 after a clear warning that
older builds cannot reopen the saved files. Schema-v1 profiles remain readable
and migrate in memory. Base schema v2 preserves the established input/cooldown
model while visual schema v2 adds typed observations, template references,
asset hashes, confidence configuration, and alpha-mask semantics. Primary,
backup, removal-marker, and asset publication retain atomic bundle semantics.

Future milestones must not add fields with semantics that an older build could
silently discard while retaining the same schema number. The owning document
increments its schema whenever backward-safe preservation is impossible.
LayerE shows the compatibility boundary before the first persistent upgrade;
autosave and live edit cannot approve it implicitly. Newer unknown schemas are
rejected rather than guessed or overwritten.

Modular feature data prefers separately versioned sidecars so installing or
disabling one capability does not enlarge every base profile:

- V2.2 defines a versioned bundle manifest and module-sidecar envelope. Generic
  HID bindings and isolated-runtime policy increment the base schema only where
  their core references require it.
- V2.3 stores the typed automation graph in an automation sidecar. Core
  recovery modes and cooldown groups increment the base schema because they
  change authoritative cooldown behavior.
- V2.4 stores widget-slot mappings and companion presentation preferences in a
  companion sidecar rather than making Game Bar fields mandatory in the base
  profile.

The profile store expands its transaction from base-plus-visual into a bounded
set of declared module documents and assets. A commit either publishes a
mutually compatible set or restores the previous valid set. Missing or invalid
optional module data disables only that capability and reports the exact source;
ordinary input cooldowns and overlays remain usable. A module cannot execute
content or reinterpret an unknown node/action/type.

### Cross-version verification and release gates

- V2.1 template tests cover masks, threshold boundaries, pixel-distinct scale
  tolerance, noise, SDR and HDR-tonemapped frames, capture loss, unavailable
  regions, malformed/flat assets, path traversal, content hashes, crash
  consistency, and fail-closed garbage collection.
- V2.1 matcher performance uses the worst-case four-state by four-reference
  workload on integrated and discrete GPUs for Low and Balanced quality and
  refuses an idle-capture false pass.
- V2.2 service tests cover every demand transition, activation/quiescence,
  missing consumers, profile/window churn, resource plateau, bundle rollback,
  HID disconnect/reconnect, helper authentication, and helper crash isolation.
- V2.3 expression tests cover every type/operator, static validation, all hard
  limits, event ordering, priority conflicts, transaction rollback, session
  cleanup, timer monotonicity, deterministic replay, fuzzing, and property
  tests.
- V2.3 cooldown tests cover legacy migration, single, sequential, and parallel
  recovery, refunds, reductions, shared pools, propagation, reconfiguration,
  and clock discontinuities. UI tests prove session-local presentation
  overrides reset correctly and never alter persisted appearance.
- V2.4 Game Bar tests cover all eight panel slots, authenticated reconnection,
  package and Explorer lifecycle, stale-state hiding, controller input,
  click-through, DPI changes, resource accounting, and a documented fullscreen
  compatibility matrix.
- Every earlier parser, runtime, capture, overlay, editor, daemon, packaging,
  and benchmark regression remains mandatory for every later milestone.

V2.x deliberately excludes OpenCV, embedded scripting runtimes, OCR, rotation
or perspective matching, automatic object discovery, cloud services,
telemetry, community distribution, and untrusted native extension execution
unless a later milestone explicitly replaces that exclusion with a threat
model and release gate.

### Version 2.5: Linux foundation and X11

Linux is a sequence of backend releases, not one portability checkbox. V2.5
targets a supportable X11 path while preserving the same platform-neutral core,
profile semantics, resource-demand model, and no-injection/no-root posture.

V2.5 is staged as:

1. **V2.5.0 - Linux core and headless runtime:** compile and run profile,
   cooldown, template, automation, bundle, and deterministic simulation tests;
   add Linux packaging, current-user storage, monotonic timers, and a
   current-user Unix-domain control channel.
2. **V2.5.1 - X11 input-only parity:** add game-window tracking, foreground
   routing, normalized keyboard/mouse/controller input, overlays, DPI/scale
   handling, tray/application lifecycle, and an editor strategy without
   leaking X11 types into core interfaces.
3. **V2.5.2 - X11 visual parity:** add bounded window capture, calibration,
   unavailable/protected handling, template assets, and Low/Balanced benchmark
   evidence where the active X11 stack can support them reliably.

The first Linux release does not claim Wayland, gamescope, universal global
input, exclusive fullscreen, or parity on every desktop environment. Import and
export bundles remain cross-platform, while bindings that reference an
unavailable device/backend are preserved and reported instead of discarded.

### Version 2.6: Gamescope and Steam Deck

V2.6 begins with a feasibility prototype rather than a compatibility promise.
It determines which documented gamescope/SteamOS paths can provide panel
presentation, focus/session identity, controller-first operation, capture, and
safe packaging without injection, privileged helpers, or compositor bypasses.

Supported paths receive explicit capability detection, controller-navigable
configuration, suspend/resume and game-mode lifecycle handling, resource
benchmarks on target hardware, and a published compatibility matrix. Unsupported
graphics/session combinations fail visibly and retain input-only operation when
possible.

### Version 2.7: Wayland

V2.7 uses XDG portals and compositor-approved protocols with an explicit
capability matrix. Global input, window identity, capture, overlay placement,
click-through, and always-visible behavior vary by compositor; LayerC detects
the available contract and never claims capabilities supplied only by X11 or
gamescope.

No design may bypass portal consent, request unnecessary privilege, scrape
another client's private protocol state, or promise universal overlay behavior.
XWayland fallback is reported as X11 compatibility, not native Wayland parity.

### Version 3: Ecosystem and trust

- Community profile distribution only after signing, moderation, update,
  revocation, provenance, and trust requirements are specified and implemented.
- A stable third-party extension ABI is considered only after first-party V2.x
  module contracts survive Windows and Linux backends; it requires capability
  permissions, version negotiation, crash isolation, and resource limits.
- Sandboxed WebAssembly extensions are considered only if real profiles cannot
  be represented by the bounded declarative automation system and a concrete
  threat model justifies the additional runtime.
- V3 is reserved for an actual compatibility, extension, or trust-boundary
  change rather than merely another first-party platform backend.

## Assumptions

- Version 1 prioritizes reliable keyboard/mouse/controller cooldown overlays over fullscreen, visual recognition, automation, sharing, and cross-platform support.
- The portable ZIP remains the default desktop distribution through V2.x; optional MSIX packaging begins with the Game Bar work in V2.4.
- Memory numbers are acceptance budgets to validate with a prototype, not claims that the first build will meet automatically.
- V1.1 means simple numeric pixel predicates; V2.1 means user-provided image/template matching, not OCR.
- V2.2 means first-party modular runtime and extensibility, V2.3 means bounded declarative automation, and V2.4 means the optional Game Bar presentation layer.
- Linux support is intentionally staged across V2.5 X11, V2.6 gamescope, and V2.7 Wayland rather than promised as one port.
- `SolOG.md` and `Luna+.md` remain historical source notes; this file is the canonical merged plan.
