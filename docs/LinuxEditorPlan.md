# Linux LayerE and GUI Testing Plan

## Status of this document

This is a historical implementation plan and dated local GUI/resource evidence
log. The current [Linux editor resource contract](Linux.md#linux-editor-resource-targets)
lives in the tracked Linux behavior document. Dated evidence/qualification
subsections describe only their stated environments and source snapshots; this
plan establishes no CI, physical-desktop, certification, or release claim.

**Status:** Changes 1–8 are implemented locally on `v27/linux-editor`, including lossless unknown-profile-field preservation, cooldown-group editing, per-profile export selection and the resource-audit repairs. Windows editor features are implemented on X11 or explicitly unavailable where Linux lacks the underlying service; unavailable data is preserved. FLTK 1.4 with the ATK/AT-SPI bridge is the selected toolkit; phase 1 of the [unification plan](EditorUnificationPlan.md) replaces that bridge with the editor's own AT-SPI adaptor on sd-bus, and this record keeps its dated ATK evidence. The X11 editor targets unpublished v2.6c (package version 2.6.2); Wayland remains V2.7 in development. The [rendered repairs and current local qualification](#rendered-audit-repairs-and-current-local-qualification-2026-10-21) bind directly inspected rendered states and fresh active-operation, validator-boundary, empty/representative-idle, 100-cycle and walkthrough evidence to the repaired candidate. Fresh Windows/Linux/package results and the final documentation-only delta are recorded separately; rejected earlier rendered acceptance is not carried forward. All evidence is automated (Xvfb/WSL/containers/private sessions); no VM, physical-desktop or real KWin qualification is claimed. On 2026-10-28 the branch landed on `main`, and the `v2.6.2` release published the gate-built `.deb` and Flatpak after a green `ci.yml` run; the dated records below predate that and are unchanged. On the `trial/fltk-standardization` branch, the frontend moved from `src/editor/linux` to `src/editor/fltk` ([EditorUnificationPlan.md](EditorUnificationPlan.md), phase 0); dated records keep the paths of their snapshots. [Wayland.md](Wayland.md#compositor-evidence-and-gaps) records compositor qualification gaps.

**Date:** 2026-10-05 (Asia/Bangkok).

**Approved placement:** Linux editor work targets v2.6c on X11, with first-release and advanced parity tracked separately. Wayland integration is V2.7 and may start only after `v27/wayland` merges into `main`.

## 1. Proposal

Build a Linux `layere` frontend around shared C++ editor behavior, and develop its automated and agent-driven GUI testing at the same time. Start against the existing X11 daemon. Integrate native Wayland daemon behavior when the V2.7 backend is ready.

Strict low memory is a design requirement. The representative comparison selected FLTK 1.4 with the ATK/AT-SPI bridge (§6), within the approved resource targets. It becomes an editor-only production dependency in Change 3; prototype evidence does not establish production feature parity or release accessibility.

The ordinary editor window and the daemon's game overlays are separate surfaces. A toolkit's ability to display an editor on Wayland does not establish native Wayland input, overlay, or capture support in `layerd`.

## 2. Planning baseline (2026-10-05)

| Surface | Observed position |
| --- | --- |
| Windows LayerE | C++20, Win32/Common Controls, and GDI custom painting; no Qt dependency. See [CMake](../CMakeLists.txt) and [editor drawing](../src/editor/editor_draw.cpp). |
| Shared editor behavior | Document, history, validation, and core simulation are reuse candidates. Some implementation helpers and the commit/live-edit orchestration still depend on the Windows frontend; they need focused extraction. |
| Linux V2.6 | `layerd`, `layerctl`, and X11 `layercal` exist. Linux LayerE and graphical calibration were deferred when this plan started (the v2.6c editor status is recorded below). [Linux.md](Linux.md) owns current support claims. |
| Automated Linux checks | [run-linux-tests.sh](../tools/run-linux-tests.sh) covered headless, five required X11 suites under Xvfb, installed-program smokes, and optional/required Flatpak stages; Change 6 adds the editor and package stages. |
| Existing GUI recipe | A local Xvfb/Openbox recipe describes test input, screenshots, and overlay pixel checks. It is a recipe, not a complete walkthrough runner; its old keyboard-binding example needs correction against current source. |
| Local tools | Debian WSL2 has Xvfb and build/X11 development dependencies. Openbox, xdotool, x11-apps, and ImageMagick were absent during inspection. Windows `cua-driver` is installed; a Linux computer-use bridge has not been verified. |
| Resource evidence | No running Windows LayerE was available for a current memory measurement. The historical plan excludes the editor from gameplay memory budgets, so daemon budgets cannot be presented as editor measurements. |

This draft was prepared against local `main` at `74fb105` with unrelated uncommitted work present. The local Wayland contract is on `v27/wayland`, introduced at `2ad59d4`, in `docs/Wayland.md`. That branch targets Wayland as V2.7 and leaves Gamescope unsupported; historical PLAN numbering differs. Re-resolve source and branch status before implementation.

## 3. Roadmap choice

| Option | Consequence |
| --- | --- |
| A: full editor plus backend in V2.7 | Enlarges one release to include a frontend port, new compositor adapters, capture, input, and their combined validation. |
| **B: usable editor first** | Gives X11 users a GUI and establishes user-interaction tests before backend integration. Recommended, with an explicitly bounded first release. |
| C: editor after V2.7 | Preserves backend focus but leaves Linux CLI-first and delays editor usability coverage. |

The first editor milestone should not require every Windows dialog to be ported. Full parity remains a separate deliverable, tracked by a feature matrix. Existing Wayland worktrees must be preserved and incorporated through their own reviewable changes; this plan does not authorize restarting them or launching workers.

## 4. Scope and parity

### First usable editor

- List, create, edit, enable/disable, and delete profiles through the existing store semantics.
- Edit cooldowns, durations, recovery modes, keyboard/mouse bindings, panel appearance, and placement using a local preview.
- Import/export validated JSON and `.layercprofile` bundles; show conflicts, validation errors, and unavailable platform features.
- Provide undo/redo, explicit Save, unsaved-change handling, and visible save/reload outcomes.
- Show daemon connection status and capability/fault reasons. Support offline editing.
- Preserve automation, visual, companion, and unsupported binding data according to the existing parser/store contracts, even when this frontend cannot edit them yet.

Initial placement is authored in the editor's preview. Positioning against a live game window belongs to a later integration phase with explicit backend capabilities.

### Later parity phases

- Automation rules/actions, presets, trace, and deterministic simulator UI, reusing the core evaluator.
- Visual/template editing and graphical calibration, including backend-specific window selection and portal consent where applicable.
- Live draft synchronization, autosave behavior, diagnostics, device-specific binding capture, advanced appearance, and live placement.
- Tray/application lifecycle UI only after its behavior and resource cost are separately scoped.

Maintain a matrix with `implemented`, `preserved but not editable`, `unavailable on this platform`, and `deferred` for each Windows editor feature. Do not label the first release full parity. Unsupported or unreadable data must never be silently discarded to make a save succeed.

### Feature matrix (first release, 2026-10-09)

Status against the Windows editor (`src/editor/*.cpp`, `editor_resources.rc`). *Implemented* entries are covered by widget/session tests, the AT-SPI check or the scripted walkthrough. Change 5 shipped the first-release subset; Changes 8a–8d and the 2026-10-15 audit repairs completed the remaining rows. No row is deferred or preserved-but-not-editable: each Windows feature is implemented on X11 or is unavailable on this platform because Linux lacks the underlying service, and that data is preserved.

| Area | Windows feature | Linux `layere` | Notes |
| --- | --- | --- | --- |
| Profiles | List, open on selection | implemented | A store that cannot be read is reported in the status. |
| Profiles | New profile (Ctrl+N), Duplicate | implemented | Each opens an unsaved draft; Save writes it. Toolbar, menu and context menu. |
| Profiles | Rename | implemented | Through the Profile name field; no separate Rename command. |
| Profiles | Turn on/off (saved enabled flag) | implemented | Toggle saved enabled, plus the document's Profile enabled box. |
| Profiles | Delete with confirmation | implemented | Removes modules and recovery copies through the store. |
| Profiles | Save (Ctrl+S); unsaved-change prompt | implemented | Cancel/Discard/Save on selection change and close. |
| Profiles | Save As (Ctrl+Shift+S) | implemented | Change 8d: saves the open profile, with its modules, as a new named profile. |
| Profiles | Open Profiles Folder | implemented | Change 8d: File > Open profiles folder starts `xdg-open` in its own session; without it the status names the folder path. |
| Profiles | Game match fields (executable, path, window class/title) | implemented | |
| Profiles | Legacy-format upgrade confirmation | implemented | |
| Profiles | Detected Tasks picker | implemented | Change 8d: Tools > Detected tasks (Ctrl+T) lists EWMH top-level windows (never LayerE) with executable, title and PID from a demand-opened X11 connection, scanned on open and Refresh only. Use selected task sets the executable matcher (optionally the exact path), falling back to the window class when the process is not readable. Pick Window is not offered: X11 grants no safe passive pointer pick. |
| Edit | Undo/redo (Ctrl+Z, Ctrl+Y) | implemented | Also Ctrl+Shift+Z. Document shortcuts take precedence over a focused field's local undo. |
| Cooldowns | Add, remove, name, duration, charges, recovery, retrigger | implemented | Invalid values are reported and never written. |
| Cooldowns | Duplicate, Rename (F2) | implemented | Change 8d: Cooldowns > Duplicate cooldown copies the timing without bindings (as on Windows); F2 focuses the cooldown or profile name. Ctrl+D duplicates the profile on Linux. |
| Cooldowns | Cooldown groups | implemented | Cooldowns > Cooldown groups edits propagation/shared-pool mode, members, timing, charges, recovery and retrigger in one validated, undoable transaction; Cancel is inert. Widget/session tests and the required `cooldown-groups` GUI case pass. |
| Bindings | Keyboard (scan code), mouse and gamepad bindings | implemented | Add, remove and capture within the focused editor dialog; Change 8d captures mouse buttons and XI2 gamepad-class buttons from focused input only. |
| Bindings | Generic HID capture | unavailable on this platform | Existing HID bindings are preserved; LayerD reports `genericHid` `platform_unsupported`. |
| Panels | Add/remove, name, layout, style, anchor, bar size, colors, text and hide-when-ready, membership | implemented | Colors are `#RRGGBB` fields. |
| Panels | Local preview placement by dragging; live placement on a game window | implemented | Change 8d: Position on window maps the preview to the matching (or selected) game window using the overlay's centred-anchor rule, and each drag is published through the live draft so LayerD's overlay follows. Direct Drag is unavailable on X11: overlays stay click-through. |
| Panels | Color picker dialog; Split Grouped Bars; Advanced Settings view | implemented | Change 8d: Panels > Choose colours uses FLTK's colour chooser per bar colour; Tools > Split grouped bars uses the shared `split_grouped_bars`; every advanced field is always shown on Linux. |
| Exchange | Import pack/JSON with collision review | implemented | Per-profile Include/Skip and Copy/Replace, then a reviewed summary. |
| Exchange | Export pack; export base JSON | implemented | Pack export offers individual saved-profile checkboxes, Select all and Select none; empty selection is refused and Cancel writes nothing. Widget/session tests and the required `export-profile-checklist` GUI case pass. |
| Runtime | Daemon status, capability faults, reload acknowledgement, offline editing | implemented | |
| Runtime | Live draft sync and autosave | implemented | Change 8c: autosave of stored profiles (text 400 ms, other edits immediately); a leased in-memory base-profile draft in LayerD restores the committed profile on end or expiry. |
| Runtime | Runtime Diagnostics dialog | implemented | Change 8d: View > Runtime diagnostics shows the shared summary plus process, X11, capability, service, session and automation-fault detail, with Refresh and Copy raw JSON. Linux has no isolated visual worker, so there is no Retry. |
| Advanced | Automation, presets, simulator, trace | implemented | Change 8a: Basic, Simulator, Advanced JSON and authenticated Live trace views on the shared graph/simulator units; trace files load/save; an unreadable module stays preserved and blocked. |
| Advanced | Visual templates and calibration | implemented | Change 8b: the Visual tab assigns, updates, clones and removes visual triggers, with shared-observation prompts. Modal X11 calibration has a capture notice, live preview, region drag, color sampling, four predicates, up to four captured/imported PNG references and an alpha mask. Profile, sidecars and assets publish in one durable transaction. The Windows isolated-runtime setting has no Linux counterpart (there is no isolated visual worker); its value is preserved. |
| Advanced | Game Bar companion | unavailable on this platform | Companion data is preserved. |
| Advanced | Options (capture exclusion, visual recognition); editor Appearance | implemented | Change 8d: Options (Ctrl+,) states that capture exclusion and isolated recognition are unavailable on X11. Tools > Appearance offers the Windows presets, accent and per-role fill/text/selection colours, previewed live and stored durably in `<config>/layerc/editor-appearance.json`. |
| Interface | Text size 100/115/130 % | implemented | FLTK scaling with Ctrl+Plus/Minus/0. |
| Interface | Keyboard Shortcuts help (F1); About | implemented | Change 8d: the F1 list is generated from the menu definitions; About has Copy details. |
| Interface | Screen-reader access | implemented | ATK and its AT-SPI adaptor (≥ 2.62), without GTK widgets. |

## 5. Architecture

### Shared behavior, separate frontends

Extract the smallest portable units from [ProfileDocument](../src/editor/common/profile_document.hpp), history, and document mutation/commit behavior. Move only code required by both frontends; avoid a generic cross-platform widget or application framework.

Keep Win32 handles, toolkit widgets, display handles, and platform headers out of shared editor interfaces. Windows keeps its native frontend. Linux gets a focused frontend and a small store/control adapter. Introduce a common adapter interface only where both implementations need it.

Use existing core JSON, UTF-8, validation, hashing, schema migration, bundle handling, and durable store APIs. Do not create a Linux serializer or a second automation engine. Toolkit dependencies belong to `layere`; the core, daemon, and CLI targets retain their existing dependency boundaries.

### Saving and daemon synchronization

The first milestone uses this path:

```text
UI draft -> shared validation/normalization -> durable stage/commit
         -> existing authenticated Linux reload request -> acknowledged result
```

Retain one-writer ownership, backups, journal recovery, sidecar write intent, and schema-upgrade confirmation. Profile operations must use the existing transactional APIs; do not implement deletion or replacement with standalone filesystem calls.

A successful disk save and a successful runtime reload are distinct outcomes. A disconnected daemon permits offline saving and leaves runtime application visibly pending. A failed save retains the draft and recovery evidence. A failed reload after a successful commit reports both results accurately.

Change 8c adds the live draft protocol: a typed, versioned `draft` payload (`src/core/live_draft_control.*`) on the authenticated control socket. LayerD applies one editor's unsaved base profile in memory under a renewed monotonic lease and restores the committed profile when the lease ends or lapses; the store is never written. Stored profiles autosave through the durable path above; new profiles need one explicit Save, and legacy formats keep explicit upgrade confirmation.

### Input and rendering

Binding capture in the editor is focused application input. It must not require global keyboard access. Daemon passive-input capability remains a separate concern, including the proposed Wayland evdev opt-in.

Create advanced dialogs on demand. Bound undo history, decoded images, preview buffers, and simulator/trace storage using current core limits plus explicit editor cache budgets. Avoid continuous redraw when the UI is unchanged; animation deadlines stop when their view closes or becomes inactive.

The editor retains the project's passive-input, no-elevation, no-telemetry, and no-network boundaries. Test input synthesis is confined to disposable test environments and test executables.

## 6. Toolkit selection gate

Evaluate at most two small prototypes initially. FLTK 1.4 is a candidate because it has documented X11 and Wayland backends; this is not a claim about its memory use or accessibility. A second candidate can be a standard accessible toolkit, selected after checking current packaging and build requirements. Qt Widgets is an optional comparator, not the approved default.

Both prototypes must exercise the same representative UI: profile list, text/numeric fields, bindings, tabs, context menu, modal validation dialog, resizable layout, and custom panel preview. Use identical data, fonts, build settings, and scenarios. An empty-window benchmark is insufficient.

Record startup and retained memory, idle CPU, dependency/package size, X11/Wayland behavior, HiDPI readability, keyboard navigation, accessibility tree quality, and the work required for the remaining editor features. Standard controls need usable accessible names and roles. If a candidate needs an accessibility bridge, include that implementation and its cost in the comparison.

Direct Xlib would constrain the frontend to X11; building a bespoke widget system would also create substantial menu, text-input, layout, and accessibility work. Neither is the default shortcut for a low-memory requirement.

Select a toolkit only after a candidate passes the resource and interaction gates. If none passes, document the measured shortfall and revise the scope or budget explicitly; do not quietly raise a limit.

References: [FLTK platform behavior](https://www.fltk.org/doc-1.4/osissues.html), [FLTK overview](https://www.fltk.org/doc-1.4/intro.html), and [Qt accessibility](https://doc.qt.io/qt-6/qaccessible.html).

### Measured comparison (2026-10-07)

The historical standalone `tools/editor-prototypes` experiments used the shared editor document/store behavior. The obsolete experiment sources have since been retired; live process/readiness helpers are maintained in [`tools/linux-gui`](../tools/linux-gui/). Both experiments contained a profile list, fields, tabs, context menu, modal validation surface and panel preview. Measurements used Release builds with GCC 14.2, FLTK 1.4.3 and GTK 4.18.6 on Debian WSL2 with a private Xvfb display. The populated fixture is 20 profiles × 10 cooldowns × 2 panels, with the first profile selected.

Evidence: local ignored `out/linux-gui/toolkit-20261007T185856Z/` (JSON samples, screenshots, AT-SPI dumps, environment, executable hashes, Windows baseline and cleanup snapshot). These are one-environment prototype results, not production-editor release gates or physical-desktop evidence.

| Candidate / scenario | USS MiB | PSS MiB | RSS MiB | Sampled peak USS MiB |
| --- | ---: | ---: | ---: | ---: |
| FLTK, empty | 3.98 | 6.05 | 12.96 | 3.98 |
| FLTK, representative | 7.60 | 10.93 | 19.70 | 7.91 |
| GTK default renderer, empty | 142.13 | 165.82 | 196.31 | 142.63 |
| GTK default renderer, representative | 143.28 | 166.99 | 197.51 | 143.36 |
| GTK `GSK_RENDERER=cairo`, empty | 65.30 | 89.00 | 119.48 | 65.41 |
| GTK `GSK_RENDERER=cairo`, representative | 66.27 | 89.98 | 120.48 | 66.34 |

All recorded Linux scenarios had zero swap and 0.000% sampled idle CPU over 60 seconds (tick-resolution measurement, not proof of zero work). Warmed-to-final USS drift after 100 internal profile/dialog cycles was 0.000 MiB for FLTK and −0.074 MiB for both GTK renderers. These cycles do not prove user-driven modal interaction. The peak sampler begins after the application announces readiness, so it does **not** capture the full startup peak required for final acceptance.

FLTK's executable is 1,409,608 bytes; its additional `ldd` shared-library closure relative to the daemon is 15,913,568 bytes (40 libraries). GTK's executable is 1,408,960 bytes; its additional closure is 31,267,304 bytes (54 libraries). These are file-size estimates, not native/Flatpak package sizes, and omit dynamically loaded renderer modules and an accessibility bridge's cost.

Keyboard Tab sequences, Alt+S Save, context menus and 2× scaling were exercised with captured desktop images. This is partial interaction evidence: both 2× windows outgrow the 1600×1000 test desktop; minimum-size layout, resize paths, focus restoration and complete shortcut coverage remain unverified. GTK exposed a 70-node AT-SPI tree with 39 named nodes and standard roles. FLTK exposed no application tree on the same isolated accessibility bus. GTK's tree is not, by itself, proof of complete screen-reader usability.

The Windows Release editor baseline recorded empty/representative private bytes of 2.68/2.77 MiB, working set of 19.02/18.45 MiB, and private working set of 2.07/2.11 MiB. Idle CPU samples were 0.078%/0.208% over 60 seconds. Windows private commit and Linux USS remain different metrics; the slightly higher populated Windows CPU sample is not a Linux resource-gate pass.

**Decision status:** GTK exceeds the 20/32 MiB USS targets even with Cairo. The user chose to **prototype an FLTK accessibility bridge**, retaining the existing targets and accessibility requirement. The final isolated resource and native layout/HiDPI probes below support FLTK 1.4 with the ATK/AT-SPI bridge as the leading candidate. The first independent source review found nine issues and its three follow-up rounds found three, five, then one remaining high-severity issue (`review/layer1.review-2026-10-08.md`). All fixes are implemented and remeasured below. The fifth round left no unresolved high-severity finding, so the decision below is recorded. No production toolkit dependency is added and no screen-reader support is silently deferred.

### FLTK accessibility extension (2026-10-07)

The standalone `layere_proto_fltk_a11y` extends the same FLTK UI with ATK semantics and the AT-SPI adaptor, without GTK widgets. GLib readiness descriptors and monotonic deadlines are integrated into FLTK's blocking loop. The verified run uses FLTK 1.4.3, GLib 2.84.4 and ATK/AT-SPI 2.62.0. Debian `2.62.0-2~bpo13+1` dependencies were extracted into a separate development prefix; installed system packages and the original comparison prefix were not changed.

The initially tested AT-SPI 2.56.2 backend crashed during graceful close. Its cache cleanup passes null hash values to `g_object_weak_unref` and leaves stale callbacks. This is an upstream defect, not an accepted recovery path ([cleanup fix](https://github.com/GNOME/at-spi2-core/commit/c4ddb104fb25ba9b1f839cea28f280bb3a4146ba), [duplicate-reference fix](https://github.com/GNOME/at-spi2-core/commit/8db3f580bd024095ddd86981b07f975575992c5d)). The experiment requires ATK/AT-SPI ≥2.62, the corrected version actually verified; this is not yet a final production dependency policy.

| Bridge scenario, live accessibility client | USS MiB | PSS MiB | RSS MiB | Sampled peak USS MiB |
| --- | ---: | ---: | ---: | ---: |
| Empty | 4.60 | 6.81 | 15.48 | 4.60 |
| Representative | 7.15 | 10.62 | 20.79 | 8.48 |
| After 100 internal cycles | 8.34 | 11.81 | 22.04 | 8.34 |

All three 60-second idle samples were 0.000% of one logical CPU at tick resolution, with no swap. Warmed-to-final USS drift was 0.000 MiB. The executable is 1,452,664 bytes; its additional linked-library closure relative to the daemon is 24,050,824 bytes across 56 libraries. These are application-process measurements and a dependency-file estimate, not native/Flatpak package sizes or an aggregate desktop-harness measurement. The subscribed test client and session accessibility services are harness overhead, not editor-owned helper processes.

The semantic probe passed 14 checks: application/list export, readable fields, focus and keyboard-Tab reflection, profile selection and refreshed text, Unicode edits/character offsets/caret, accessible Save with disposable-file verification, readable status, and focus/text/selection events. Both normal close and the explicitly disabled-bridge path exited successfully with `G_DEBUG=fatal-warnings`; the disabled path retained a visible fault. A post-run snapshot found no prototype processes. This does not prove complete socket/helper/capture ownership cleanup.

Evidence: ignored `out/linux-gui/fltk-a11y-backport-20261007T210321Z/` (semantic action/event report, screenshots, close/fault logs) and `out/linux-gui/fltk-a11y-backport-resource-20261007T205753Z/` (three resource scenarios and screenshots). Both include base revision, working-tree source/executable/linked-library hashes and a cleanup snapshot. They are automated WSL/Xvfb evidence, not VM, physical-desktop or release evidence.

### Corrected sampling and comparison bundles (2026-10-07)

`measure.sh` now establishes the sampler before exec, samples every 5 ms through readiness/initial settling, and synchronizes a warmed baseline before releasing the 100 internal cycles. The FLTK and GTK prototypes now explicitly use the same DejaVu Sans font and size. This repairs the earlier missing startup interval and unsynchronized cycle baseline; sampled peaks still cannot prove sub-interval maxima.

| Candidate / scenario | USS MiB | PSS MiB | RSS MiB | Sampled startup peak USS MiB |
| --- | ---: | ---: | ---: | ---: |
| FLTK + bridge, empty | 4.23 | 5.80 | 15.70 | 4.66 |
| FLTK + bridge, representative | 7.18 | 9.77 | 22.22 | 8.47 |
| FLTK + bridge, after cycles | 7.23 | 9.83 | 22.36 | 8.55 |
| GTK Cairo, empty | 66.50 | 82.83 | 120.90 | 69.45 |
| GTK Cairo, representative | 67.50 | 83.77 | 121.67 | 66.88 |
| GTK Cairo, settled after cycles | 70.70 | 93.84 | 124.03 | 67.13 |

All six 60-second idle samples recorded 0.000% of one logical CPU at tick resolution and no swap. FLTK's synchronized warmed-to-final drift was +0.012 MiB. GTK's immediate post-cycle drift was +0.414 MiB, increasing to +3.570 MiB in the final settled sample. The candidates ran concurrently in separate displays/sessions; peer exits can change shared/private page accounting, so this exploratory comparison is not an isolated leak test or final resource acceptance. The startup column covers the startup interval, not the maximum over the entire later scenario. Evidence: ignored `out/linux-gui/toolkit-corrected-20261007T213030Z/`. The bridge semantic/close/fault probe also passed again against the current binary in `out/linux-gui/fltk-a11y-backport-20261007T213832Z/`.

`comparison_bundle.py` produces deterministic, checksum-verified gzip/tar comparison archives containing the stripped prototype executable and its linked-library closure. The generated archives are 10,501,217 bytes for FLTK, 10,773,646 bytes for FLTK + bridge, and 16,157,170 bytes for GTK. They are **same-host prototype bundles, not distributable LayerC/native/Flatpak packages**: they omit the host ELF interpreter, fonts, dynamically loaded assets/plugins and desktop session services. Each archive records these limitations and the exact library checksums. Package/install policy and cross-distribution portability remain separate release work.

### Isolated resource and native interaction checks (2026-10-08, after four review rounds)

The candidates were measured **sequentially**, each on an authenticated private Xvfb display and D-Bus session, with disposable HOME/XDG data and a live subscribed accessibility client. Release binaries use the same DejaVu Sans font/size and representative fixture.

- **Fixture isolation:** each executable refuses to start unless it finds the harness's private fixture marker. It then pins the fixture root by descriptor and confines itself with Landlock ABI 3 or later, so truncation is controlled, before reading any profile:
  - Writes are allowed only beneath the fixture, plus write access to `/dev/null`.
  - Reads are allowed only beneath the fixture, system trees, the canonical directories of loaded libraries, and four device nodes.
  - Directory names remain listable for the durable store's ancestor fsync barrier.
  - No grant may include the real home or overlap its configuration, data, state, cache or runtime trees, nor the original XDG locations the harness records before isolating the environment.
  - Each document is loaded only if its opened inode is a single-link regular file, both before and after the pathname load.

  The executable reports its resolved store and loaded counts. The harness rejects a scenario unless they are exactly 0 profiles (empty), or 20 profiles with a 10-cooldown/2-panel document (representative). The guard prevents accidental production access; it does not claim protection against a concurrent same-user adversary.
- **Process ownership:** every long-lived harness process runs under a supervisor that keeps it unreaped, defers stop requests until its child is owned, and always stops and reaps it, including on internal failure. A process group is signalled only while its leader is unreaped, and detached descendants are adopted and stopped through pidfds. Stop requests arrive through a control pipe whose read end is the supervisor's standard input. No script signals a numeric PID.
- **Sampling:** the pre-exec sampler alone owns the application. It publishes the PID only after observing the prototype image, and binds its per-run report to a nonce plus the application's PID, start time and executable.
- **Measurement integrity:** memory samples fail closed when a field is missing. Idle CPU is divided by the observed monotonic interval. A run is accepted only if the application exits cleanly or is terminated by the harness's own SIGTERM.
- **Cycle workload:** each measured cycle switches profile and opens the toolkit's own Save-validation dialog. A cycle counts only after that specific dialog has rendered, using FLTK `draw()` or GTK frame-clock paint, and has then closed. All 100 measured cycles completed for both toolkits.

These results supersede the earlier tables; sampled peaks still cannot prove sub-interval maxima.

| Candidate / scenario | USS MiB | PSS MiB | RSS MiB | Sampled startup peak USS MiB |
| --- | ---: | ---: | ---: | ---: |
| FLTK + bridge, empty | 6.90 | 10.45 | 20.98 | 6.90 |
| FLTK + bridge, representative | 7.45 | 10.99 | 21.50 | 7.45 |
| FLTK + bridge, after cycles | 7.49 | 11.04 | 21.51 | 7.48 |
| GTK Cairo, empty | 64.56 | 88.16 | 118.97 | 65.01 |
| GTK Cairo, representative | 54.75 | 78.36 | 109.14 | 56.68 |
| GTK Cairo, after cycles | 56.77 | 80.37 | 111.14 | 55.88 |

All six idle samples recorded 0.000% of one logical CPU over observed 60.3-second monotonic windows at tick resolution, with no swap. Warmed-to-final USS drift across the 100 rendered validation-dialog cycles was +0.016 MiB for FLTK and +0.883 MiB for GTK. FLTK used five threads and GTK eighteen.

FLTK's executable is 1,507,384 bytes, with 24,050,824 additional linked-library bytes across 56 libraries relative to the daemon. GTK's is 1,444,160 bytes, with 38,769,104 additional bytes across 68 libraries. The bridge links ATK, its AT-SPI adaptor and libatspi by path from one version-checked 2.62 installation. Configuration fails if any of them is missing there. At startup the bridge verifies that the loaded copies share one directory and that ATK is at least 2.62; otherwise it reports a visible fault. The test client, Xvfb, window manager and session services are harness overhead, not editor-owned helpers; the table is application-process evidence, not a measured total for the testing desktop.

On a 2880×1800 Xvfb desktop, both candidates passed the native layout and keyboard probe at 1× and 2× scaling: 214 assertions per FLTK run and 208 per GTK run.

- **Window sizing:** each requested native size, including the scaled 760×400 minimum, must be achieved within 4 px.
- **Geometry:** the probe checks window-relative visible bounds, role-appropriate readable sizes and non-overlap for fields, lists and Save. GTK 4 reports a zero desktop origin for every X11 control, so desktop extents would have made those checks vacuous.
- **Native editing:**
  - Typing, deletion and Home/End caret movement in a focused field are reflected in accessible text.
  - The unchanged document is first saved to its canonical form. That baseline Save is checked independently: it may only add the document's `.bak` with the exact original bytes, preserve every original value apart from a schema-version upgrade, remove nothing, and leave every other store entry identical. After the edit, Save must rewrite exactly that document with only its name changed, with the same backup and identity rules. Store inventories are recursive and include every entry type.
- **Key capture:** capture is explicitly armed and cancellable. Escape cancels and keeps focus, Tab cancels and moves on, and bounded reverse Tab returns. Two distinct keys must each change the same binding label to their exact key symbol: `Key 107` for k and `Key 109` for m. No other non-status label may change.

The stricter probes exposed and fixed three FLTK prototype defects: a Save button that shrank below a readable size at the minimum, an invisible zero-size popup focus stop, and reverse Tab skipping the tab page's controls. These automated assertions do not establish physical-display or observed screen-reader usability.

The FLTK bridge's semantic/event checks, fatal-warning normal close and disabled-bridge visible-fault path passed under the shared authenticated fixture.

- **Direct-launch refusals:** all eleven variants were refused before any profile was read: ambient environment, wrong token, HOME escape, symlink, hard link, store mismatch, non-private root, and production roots overlapping a loaded-library directory, a system tree, or a library directory through a colon-containing path, plus malformed production-root metadata.
- **Confinement probe:** each of these failed with `EACCES`, and the outside sentinel stayed intact:
  - reading, truncating or creating a file outside the fixture;
  - reading or creating a `/dev/shm` file.

  Creating a file in the store, reading a system file, and a durable Save under confinement all succeeded.
- **Supervisor negatives:** after an injected failure following spawn, the child was stopped and reaped. A detached `setsid` descendant was stopped with its group. With an injected pidfd-acquisition failure, a TERM-ignoring detached descendant was still stopped, and the supervisor reported a visible failure.
- **Failure paths:** a killed application failed its scenario immediately, with every owned process, control directory and fixture cleaned up. The harness rejected a private DISPLAY override and failed closed when its Xvfb could not start.

The checksum/readback-verified comparison archives are 10,520,719 bytes (FLTK), 10,792,210 bytes (FLTK + bridge), and 16,173,104 bytes (GTK). As above, these are same-host prototype bundles, not distributable/native/Flatpak packages, and their instructions require the private fixture harness.

Evidence (ignored):

- `out/linux-gui/toolkit-isolated-20261008T213829Z/`: resource reports and compiled-source/executable provenance.
- `out/linux-gui/toolkit-final-20261008T213644Z/`: layout, interaction, semantic/close/fault, failure-path and bundle reports.
- `out/linux-gui/c1-smoke-20261008T213534Z/`: direct-launch refusals and confinement probes.
- `out/linux-gui/c1-f9-negative-20261008T213639Z/`: unexpected-exit failure.

Provenance identifies base revision `5a47cf6` plus dirty-source hashes, not a tested release commit. CI was not run.

### Toolkit decision (2026-10-09)

**Decision:** the Linux `layere` frontend uses **FLTK 1.4 with the project's ATK/AT-SPI bridge**. It is the only candidate that passed both the resource and the interaction gates:

- **Memory:** USS 6.90/7.45/7.49 MiB (empty / representative / after 100 cycles), against the ≤20 MiB idle and ≤32 MiB representative targets.
- **Drift and idle CPU:** +0.016 MiB drift, within the ≤2 MiB target, and 0.000% idle CPU.
- **Interaction:** native 1×/2× layout, keyboard and editing probes passed, and the semantic accessibility-tree probe passed.

GTK 4 is rejected. With Cairo it used 54.75–64.56 MiB USS, about 142 MiB with its default renderer, so it exceeds both memory targets. No limit in §7 was raised. They are frozen, as measured targets, for Change 3 onward. The ≤48 MiB advanced-view peak target remains to be measured when such views exist.

Consequences carried forward:

- **Dependency:** FLTK 1.4 becomes a `layere`-only dependency in Change 3; Debian 13 provides `libfltk1.4-dev` 1.4.3. Core, daemon and CLI dependencies are unchanged.
- **Accessibility runtime:** the bridge requires ATK/at-spi2-core ≥ 2.62. Debian 13 currently ships 2.56.2, whose adaptor has the recorded shutdown defect. Against an older or mixed adaptor installation, the bridge reports accessibility as unavailable through a visible fault rather than crashing. Change 6 decided how releases provide a fixed adaptor: the Debian package depends on the 2.62 libraries (Debian 13 `trixie-backports`), and the Flatpak builds at-spi2-core 2.62.0 because the 25.08 runtime ships 2.54. Screen-reader support is not silently deferred.
- **Review:** six independent static-review rounds are recorded in `review/layer1.review-2026-10-08.md`. Round 6 confirmed the three remaining medium fixes and found no new defect; unresolved high-severity findings: zero. Automated verification remains the primary agent's evidence, not independently executed reviewer tests.

The selected bridge still lacks:

- popup/modal export;
- dynamic list-child reconciliation;
- precise row/tab geometry;
- complete editable-text/selection interfaces;
- an observed Orca workflow.

Main-window semantics establish viability, not full editor accessibility or completed feature-parity walkthroughs. These gaps remain required production work for Change 5, not deferred release accessibility.

### Linux vertical-slice evidence (2026-10-09)

Change 3 now has a working FLTK source target and a toolkit-free controller:
UI edit -> shared durable stage/commit -> authenticated daemon reload -> a
changed live X11 overlay. The private exploratory bundle
`out/linux-gui/slice-probe-20261009T000727Z/` contains before/after/offline
profiles, daemon status, screenshots, checksums, resource reports and
joined-child cleanup records. All four recorded cases passed: the measured
overlay moved 550 pixels; invalid `nan` input left the base file unchanged;
an offline Save committed successfully; a forced storage failure remained
visibly unsaved. Reload refusal, lost acknowledgement, malformed control
paths and a saturated connection queue have focused regression coverage.
A saved profile and an acknowledged reload are distinct outcomes: loss of
the acknowledgement reports an unconfirmed reload, not that it never ran.

For this one-profile slice, the pre-exec-gated 5 ms startup sampler observed
6.91 MiB USS / 10.02 MiB PSS / 19.30 MiB RSS. Settled samples reported
6.93 MiB USS and zero swap; idle CPU was 0.0% of one logical CPU over
60.18 seconds. The full samples are retained in the same bundle. These
observations are below
the approved ceilings, not acceptance of empty/representative/advanced/cycle/
worst-case workloads. Harness/X server buffer accounting and the production
accessibility bridge remain later gates. This exploratory bundle is not the
Change 4 runner.

MSVC rebuilt the Windows editor and passed both affected editor/document
suites; Debian WSL passed all 17 headless and five Xvfb X11 suites through
`run-linux-tests.sh` (`out/linux-automation/slice-20261009T000339Z/`).
The final attachment-only reviewer confirmed all six findings resolved,
with no new actionable or unresolved high-severity finding; it did not
independently execute tests. CI was not run; no physical desktop or Flatpak
editor coverage is claimed. The released version remains 2.6.0; `README.md`
and `Linux.md` describe the source slice as experimental.

## 7. Approved resource targets

The current authoritative contract is [Linux editor resource targets](Linux.md#linux-editor-resource-targets). The historical target text follows unchanged.

**These are approved implementation targets, not release acceptance claims.** Change 1 established prototype feasibility and the Windows baseline; the ceilings are unchanged for Change 3 onward. If a target is missed, consult the user rather than raising it. The [2026-10-21 source-bound audit](#rendered-audit-repairs-and-current-local-qualification-2026-10-21) verifies active operations and the selected validator-derived boundary/exhaustion envelope, with the limits of sampled measurements stated explicitly; it is not proof for every combination of valid profile data.

Use Linux USS (private resident pages) as the primary editor memory metric, and report PSS, RSS, swap, and any GPU/buffer allocations alongside it. Include every editor-owned helper process in aggregate results. Record Windows private bytes and working set separately; private commit and Linux USS are not equivalent measurements.

| Scenario | Approved Linux USS ceiling |
| --- | ---: |
| Settled idle editor, no profile selected | 20 MiB |
| Representative store: 20 profiles, 10 cooldowns each, two panels each; one selected | 32 MiB |
| Active advanced view on a representative automation/visual fixture | 48 MiB peak |
| After 100 repeated dialog/profile-switch cycles | Within 2 MiB of the warmed baseline, with no continuing upward trend |

Capture the full startup peak even if the idle gate begins after settling. Once functionality is available, measure worst-case valid documents derived from the current validators. Before release, establish their explicit peak bound and resource-exhaustion behavior; the representative fixture is not proof for every valid profile.

Additional approved targets:

- Idle CPU at most 0.2% of one logical CPU averaged over 60 seconds when no animation or operation is active.
- Closing the editor leaves no editor/helper processes, owned sockets, or active capture resources.
- Opening/closing the editor must not weaken existing daemon memory/resource gates. Reuse the daemon benchmark contracts and add standalone editor measurement rather than interpreting daemon deltas as editor memory.
- Cache/history limits preserve authored data. A resource-limited preview or operation reports its reason; it does not silently alter a valid profile.

Keep the desktop, driver, video recorder, and agent/model overhead in a separate harness measurement. Application-only numbers must not conceal the total cost of running the testing environment.

## 8. Testing method

### Deterministic checks

Run shared document/history/validation tests on Windows and Linux. Cover normal edits, rejected candidates, schema upgrades, unreadable modules, unsupported bindings, save failure, recovery, and bundle round trips. Add widget tests for repeatable field, menu, shortcut, and dialog behavior.

Toolkit-internal events establish widget behavior; they do not prove window-manager resizing, native focus delivery, popup capture, or daemon passive input. Those need the process/desktop paths below. [Qt Test documents this distinction](https://doc.qt.io/qt-6/qttestlib-tutorial3-example.html).

### Agent walkthroughs

Run a single computer-use agent against a disposable Linux desktop. The user chooses the harness; do not start multi-agent teams by default. The Linux screenshot/input driver must run inside that graphical session with the appropriate accessibility bus. Expose it to the Windows-side agent through a local CLI/MCP bridge. Installing the Windows driver alone does not establish this connection.

Follow the same discipline as the existing Windows LayerE skill:

1. Capture fresh desktop/window state and accessibility information.
2. Select a current semantic target, or use current screenshot coordinates for a custom surface.
3. Perform one action and capture the result.
4. Verify the visual outcome and, for durable/runtime effects, the saved document and daemon state.
5. Record findings, tested coverage, blocked paths, and cleanup.

Use whole-desktop PNG captures when inspecting popups, context menus, or separate dialogs. An application-window screenshot alone may omit them. Save before/after images; optional video supplements the images and action log. Prefer the driver's capture API; a verified X11 root capture is a fallback for the Xvfb environment. [Cua documents local Linux desktop sandboxes](https://cua.ai/docs/cua/reference/desktop-sandbox/linux-container).

Use actual edge/corner dragging for user resize paths. Directly setting geometry is useful for repeatable layout checks and is recorded as a separate action type. Build the menu coverage table from the implemented UI: right-click selected items and empty areas, inspect disabled/enabled entries, exercise each relevant command, and verify its result. A menu being visible is not evidence that its commands work.

### Required walkthrough cases

| Area | Cases |
| --- | --- |
| Window/layout | Minimum size, narrow/wide windows, repeated resize drags, maximize/restore, scrolling, font scaling, clipping and overlap. |
| Navigation | Tab/Shift-Tab order, shortcuts, Enter/Escape, focus restoration, modal behavior, accessible labels and selected states. |
| Menus | Top-level and context menus; selected versus empty rows; disabled actions; cancel and confirmed outcomes. |
| Profiles | Empty/populated store, create/rename/enable/delete, dirty selection changes, undo/redo, invalid values. |
| Persistence | Save/reopen, import/export conflicts, invalid or recovered sidecars, failed commit, retained unknown/platform-specific data. |
| Runtime | Daemon absent, connect/reconnect, successful reload, reload failure after save, visible capability faults. |
| Overlay integration | Test binding triggers an overlay on the controlled target; target resize/focus changes; click-through and cleanup. |
| Automation (Change 8a) | Presets, palette, rename, cancelled/confirmed removal; malformed JSON with location and no write; undo/redo; saved sidecar content; simulator refusal, Step, Play/Pause, breakpoint and assertion; live trace record, view-departure stop, save/load/replay, daemon outage; 60 s per-view resources. |
| Visual (Change 8b) | Unacknowledged capture refused; X11 fixture capture, region drag and sampled color; invalid input and cancelled Exact quality without writes; durable pixel trigger; template reference capture, malformed/oversized/duplicate PNG refusal, mask painting, four-reference bound, cancel without writes, canonical assets; shared observation cancel/clone/update/remove with undo/redo and sibling preservation; unavailable isolated runtime preserved; stopped calibration view resources. |
| Live draft and autosave | Autosave with focus and undo/redo; invalid text never written; a leased in-memory draft moves the real overlay while the store is blocked and Undo restores it; legacy upgrade consent; offline autosave and reconnect (Change 8c). |
| Diagnostics and tools (Change 8d) | Runtime Diagnostics through a LayerD stop and restart; F1/About/F2 and Duplicate cooldown with undo; focused key and mouse capture, cancel without writes and the gamepad/HID limits; Options, Save As and the profiles-folder fallback; Split grouped bars with exact undo; Appearance preview, Cancel and durable Apply; Detected Tasks matcher and Position on window moving the real overlay. |
| Portal consent (V2.7) | Real GNOME source picker deny/cancel/revoke against the product `PortalScreencast` in an owned private headless session (`tools/linux-portal-consent-tests.sh`). |

Agent exploration finds user-facing defects; reproducible failures should receive the smallest meaningful regression check. Automated interaction and synthetic clicks do not establish physical keyboard, monitor, or GPU evidence.

### Environment matrix

| Environment | Purpose and evidence boundary |
| --- | --- |
| Debian WSL2 + Xvfb/Openbox | Routine editor interactions, reproducible layouts, process-level X11 behavior, screenshots and agent walkthroughs. |
| Isolated GNOME Xorg / KDE X11 guest sessions | Real desktop shell, focus, dialogs, compositor behavior, and scaling; VM results remain VM evidence. |
| Headless/nested Wayland compositor | Focused protocol tests and normal editor-window compatibility; no claim of full GNOME/KDE behavior. |
| GNOME/KDE native Wayland guest sessions | Editor plus V2.7 provider/capture/consent integration after those backends land. Driver actions must use admitted compositor mechanisms. |
| Physical Linux desktop | Separate release checklist for actual input, monitors, drivers, hotplug, and observed consent behavior. |

## 9. Runner and evidence

Proposed entry point: `tools/test-linux-gui.ps1`. It invokes the existing Debian WSL environment, uses the repository as source, builds on Linux storage, and collects results back into a unique ignored `out/linux-gui/<run>/` directory. It must not start a new model/provider service automatically.

A focused Linux helper, provisionally `tools/linux-editor-gui-tests.sh`, owns display/window-manager readiness, fixture startup, private XDG directories, tests, and cleanup. Reuse [linux-lib.sh](../tools/linux-lib.sh), the existing test report machinery, and gate conventions where they fit. Keep setup/dependency installation separate from an ordinary test run.

Use unique displays, explicit process ownership, and monotonic readiness deadlines. Do not stop the user's daemon or address the production control socket. Preserve failure evidence, clean test resources on interruption, and restore any environment started solely for the run when doing so will not disrupt user work.

Each result bundle contains:

- Exact source revision, dirty state, a digest of tested source changes, executable checksums, configuration, and tool/environment versions.
- Build/test registration, machine-readable results, logs, screenshots, optional video, and agent action/coverage records.
- Separate editor/daemon/harness memory and CPU samples, declared metric definitions, and fixture identity.
- Expected versus observed behavior, reproduction steps, skips/unavailable capabilities, and cleanup result.

Missing required dependencies or skipped required suites fail the selected GUI gate. An exploratory report can mark a path blocked or untested, but cannot convert it to pass. Preserve the existing headless/X11 gate and add explicit editor gates; do not silently shrink coverage on machines without the new toolkit.

### Runner and bridge status (2026-10-09)

Change 4 implements the runner as specified above. `tools/test-linux-gui.ps1` records the revision, mirrors and builds in Debian WSL, and invokes `tools/linux-editor-gui-tests.sh`. Python helpers live in `tools/linux-gui/`, and process ownership reuses `tools/linux-gui/owned.sh`. [Linux.md](Linux.md) documents usage, bridge operations and bundle contents.

- **Scripted walkthrough** (`out/linux-gui/walkthrough-20261009T021721Z-ddad907d/`, final code; earlier passing runs `walkthrough-20261009T020451Z-ff6db568` and `walkthrough-20261009T014535Z`): all seven required slice cases passed. They covered bridge guards; startup; invalid input left unwritten; durable save, acknowledged reload and an overlay moving from mean x 335 to 885; frame-corner resize from 900x765 to 1020x845 and a clamp back to 900x765; offline save; and a visible store failure with the original bytes restored. Each Save outcome is checked as a status change across the Save itself. The run produced 23 screenshots. Idle LayerE measured 6.86 MiB USS with 0.0% CPU over 60.2 s, and the startup peak was 6.83 MiB USS. Harness processes are reported separately; Xvfb is about 62 MiB USS. Every expected owned process stopped or exited with code 0. Cleanup left no processes, display locks or sandbox behind.
- **Agent sessions:** the Windows-side agent drove the slice through `-Mode Call`. On the final code, `session-20261009T021902Z-7a7492e6` passed: concurrent Calls, a duration edit to 6.5 s saved durably with LayerD reachable, and a drag. In `session-20261009T014716Z` it edited the duration to 2.5 s and saved; the durable profile showed 2500 ms and LayerD was reachable. In `session-20261009T014159Z`, concurrent Calls returned their own responses, and leading-dash text was typed literally. Stale-screenshot actions were rejected. `-Mode Stop` joined both sessions cleanly.
- **Display-loss fault injection** (`session-20261009T022016Z-fb443612` on the final code; refusal checks in `session-20261009T020806Z-d160bf85`): killing the run's own Xvfb ended the session with reason `display-lost`, and the run failed with accurate per-process reasons. Cleanup was still complete, and the stale lock our killed server left was removed and recorded. In the same session, a second Session was refused, and a concurrent walkthrough was refused by the mirror/build lock.
- **Review:** the first static review found two High issues: mirror pruning into an unowned directory, and display ownership checked only at bridge start. It also found thirteen Medium and two Low issues. The confirmation review found no unresolved High. Its remaining Medium items (per-command display confirmation, evidence-shape validation, operation deadlines, argument quoting, failure cleanup, exact mirroring, run isolation, bounded supervisor waits and runner identity) were then addressed. A final confirmation also found no High. Its remaining Medium items (enumeration confirmation, deadline coverage, drag release, kill join, build-to-run handoff and queued-client timeout) were addressed: build stamps are verified under shared mirror/build locks, and a drag button that cannot be released ends the session. Regression tests are in `tools/tests/test_linux_gui_{report,bridge}.py` (17/17 in WSL). A narrow race remains between each display confirmation and that X command's own connection; it requires a same-user server racing into the freed display number.
- **Change 4 scope:** the slice walkthrough deferred menus, CRUD, import/export, keyboard/AT-SPI navigation, layout and reconnect to Change 5; they are now covered below. Change 6 wires the editor's CTest gate and an installed-editor smoke into `run-linux-tests.sh` and CI; the scripted walkthrough remains a local runner (it needs Openbox and the agent bridge).

### First-release editor evidence (Change 5, 2026-10-09)

The production editor is split into `src/editor/linux/editor_{app,forms,menus,import,preview,geometry,accessibility}.cpp` and `binding_capture.cpp`, over the toolkit-free `EditorSession`. Builds with GCC 14.2 and FLTK 1.4.3 are warning-free. Evidence is automated Debian WSL/Xvfb only.

- **Accessibility:** `editor_accessibility.cpp` exports the editor's windows, menus, lists, tabs, fields, choices, check boxes, buttons and modal dialogs through ATK. The tree is reconciled from FLTK's own loop, without a thread or polling timer. Actions that run application callbacks are deferred to that loop, so a modal dialog never runs inside an AT-SPI call. The build requires ATK/AT-SPI ≥ 2.62 and checks at run time that the loaded libraries form one coherent ≥ 2.62 set. Without a usable bus or library set, the window shows the reason and the editor keeps working. `tools/linux-gui/a11y_check.py` passes 26 semantic checks through AT-SPI alone, as the walkthrough's accessibility case. Text changes are reported as the changed span with the removed text. Accessible edits never run application callbacks inside the AT-SPI call.
- **Scripted walkthrough** (`walkthrough-20261009T141735Z-a952ce2b`, after both review rounds; first pass `walkthrough-20261009T064704Z-31dbb1a0`): all 12 required cases passed. They cover bridge guards, startup, the AT-SPI check, keyboard navigation, top-level and context menus, profile create/rename/enable/delete with undo/redo and dirty-switch prompts, invalid input, save/reload with an overlay moving from mean x 335 to 885, export/import with a collision and a cancelled import, resizing to the 640×480 minimum with scroll bars, maximize/restore and scaling, daemon stop/offline save/restart, and a visible store failure. Targets are found by accessible role and name; actions are real X input. The import chooser also lists a non-UTF-8 file name, which must reach AT-SPI as valid UTF-8 (U+FFFD) without stopping the editor. Idle USS 8.25 MiB, startup peak 8.25 MiB and 0.0% idle CPU over 60 s. Cleanup was complete.
- **Representative store** (`resources-20261009T140604Z-2a2834a2`, 20 profiles × 10 cooldowns × 2 panels, first selected; first pass `resources-20261009T065342Z-f21c70bd`): startup peak 8.33 MiB USS, idle 8.33 MiB USS and 0.0% CPU over 60 s, against 32 MiB and 0.2% targets. After the cycles, PSS was 12.19 MiB, RSS 23.00 MiB and swap 0. Over 100 measured profile-switch/Delete-prompt cycles after a 10-cycle warm-up, USS went from 8.789 to 8.793 MiB (+0.004 MiB), with no rising trend.
- **Defects found by the walkthrough and fixed:**
  - A focused text field consumed Ctrl+Z as local undo; document shortcuts are now matched first through `Fl::event_dispatch`, and a widget test sends a real key event.
  - The file chooser started in the process's working directory; it now starts in the home directory or the last chosen one.
  - The bridge's screenshot path checked an expired deadline.
- **Review:** a static review of the accessibility adapter found no confirmed High. Its Medium findings are fixed:
  - Accessible text edits ran change callbacks inside the AT-SPI call, then again when deferred.
  - Every document shortcut pre-empted a focused field; now only undo/redo do.
  - A missing bus was reported as an active bridge.
  - Text events lacked the removed text.
  - Link and version checks ignored package directories and the sysroot.
  - The editor could be skipped silently; `LAYERC_REQUIRE_EDITOR` now makes that an error.
  - The bridge's daemon-start wait was unbounded, and the accessibility check ran without display confirmation.

  Low findings on child ordering, single-instance cleanup, toggle buttons and system headers are fixed too.

  A confirmation review, which also covered the parts of the adapter the first review never saw, found no unresolved High. Its fixes:
  - Exported strings are made valid UTF-8, because libdbus aborts on invalid UTF-8.
  - Name queries no longer hide name changes.
  - Accessible focus requests are deferred like other actions.
  - Visibility, resizability, row focus and duplicate-library detection are corrected.
  - The GObject type names are prefixed.
  - The sysroot pkg-config search is scoped to the version check.

  The sysroot may still take GLib headers from the host.
- **Not covered:** physical desktops, screen readers other than the AT-SPI client used here, and the overlay-target resize/focus cases (runtime suites).

### Automation parity evidence (Change 8a, 2026-10-12)

- **Shared units:** `src/editor/common/automation_graph.*` (labels, presets, palette, node rows/fields, validation, JSON error location) and `automation_simulator.*` (deterministic simulator, trace files, bounded live log) are Win32-free; the Windows `AutomationEditor` now uses them, with removed duplicates. Removals may repair an already-invalid graph; an unreadable sidecar blocks editing on both platforms.
- **Linux UI:** the Automation page builds lazily and offers Basic, Simulator, Advanced JSON and Live trace views. Edits go through `EditorSession::edit_automation` with one history transaction each. The JSON view strips the writer's CR, so FLTK shows no `^M`. A read-only Profile ID field distinguishes same-name imports.
- **Live trace:** LayerD serves an authenticated, versioned `trace` control command with per-client leases (16 clients, 2.5 s lease, 32-entry/128 KiB batches). Tracing is enabled only while a lease exists, and the 2048-entry runtime buffer is released when the last lease ends. Cursor/generation checks refuse stale replays; a lost reply can be fetched again; an oversized entry is skipped as a gap. The editor's client is an owned worker joined on view departure; lapsed leases resubscribe with a visible gap. No input is synthesized.
- **Tests:** `layerc_editor_automation_tests` (codec, simulator bounds), registry trace lifetime (subscription without evaluation, lease expiry, gap, generation reset, retry, overflow refusals), FLTK widget tests (history, invalid properties, JSON, simulator lifetime, trace disconnect), harness unit tests (29 on Linux, none skipped).
- **Results (current tree):**
  - GUI walkthrough `out/linux-gui/walkthrough-20261012T001358Z-081ecc37`: 15/15 required cases passed. The baseline is 8.23 MiB USS at 0.0% CPU over 60.2 s. Advanced views (60 s each): JSON 10.14 MiB USS, simulator 10.23 MiB, stopped trace 10.29 MiB, all at 0.0% CPU, within the 48 MiB / 0.2% targets. These are observed idle peaks. The report requires at least 200 samples covering the window and binds them to the owned editor's PID, start time and nonce.
  - Debian WSL `run-linux-tests.sh --flatpak required --editor required` (`out/linux-automation/c8a-20261012T002232Z`): overall pass. Build has 0 warnings; 19 headless, 5 X11 and 1 editor CTest suites passed; native and Flatpak smokes passed. The native editor used 13.0 MiB USS at 0.0% CPU. The package stage was off.
  - Windows MSVC Debug rebuilt with 0 warnings; 21/21 CTest suites passed.
- **Review:** three reviewer passes (trace protocol, editor/shared units, GUI harness), then a follow-up pass of each, found no unresolved High. The first harness pass's High (a short or forged idle window could pass) is fixed. Medium fixes:
  - Trace: skip oversized entries, retryable cursors, client resubscription.
  - Editor: Windows CRLF trace and blank JSON, the invalid-sidecar guard, repair removals, simulator progress kept across unrelated refreshes, a fresh log per recording, no empty undo steps.
  - Harness: pidfd before identity, deadline-bounded sampling, private-root checks, saved-sidecar assertions, view-departure and replay-count checks.
- **Remaining Medium findings:**
  - Sessions sharing a profile ID trace the first match.
  - Trace faults other than a lapsed lease end the session after three retries, with the reason shown.
  - The walkthrough cannot prove Ctrl+S ran when the refusal text repeats the validation message; the no-write check still holds.
  - The simulator's fixed runtime clock relies on event timestamps.
- **Not covered:** physical desktops, CI (not pushed), and a live trace with two matched sessions for one profile.

### Visual parity evidence (Change 8b, 2026-10-13)

- **Shared units:** `src/editor/common/visual_assignment.*` (assignment lookup, shared-observation update/clone/removal and the complete calibration merge) and `visual_calibration.*` (calibration model, controls, predicates, template references and masks) are Win32-free. The Windows visual editor uses the shared merge; `layerc_editor_visual_tests` runs on both platforms.
- **Linux UI:** the Visual tab builds lazily. Enablement, removal and the shared-observation choice are validated history edits. The calibration dialog is modal and owns all capture resources and preview timers, with no worker thread. Capture starts only after the notice is acknowledged, uses the bounded X11 path, and stops (requiring renewed consent) when the target's identity changes. Accessible actions on controls under the modal are refused.
- **Storage:** template assets are content-addressed canonical PNGs in the profile's XDG data namespace. Apply validates the complete merge, the shared-observation choice and the template bytes inside the dialog; a refusal keeps the dialog and its draft. The session owns new and duplicated template bytes until a Save publishes them with the profile and sidecars in one durable transaction. Bytes still reachable from undo/redo history stay owned; unreachable ones are released. A Save whose final durability barrier fails reports "Save unconfirmed", not "Not saved".
- **Harness:** the AT-SPI check walks the tree iteratively; Python 3.13 crashed when an AT-SPI callback ran while a nested walk generator was being finalized. Opening a modal disables every main-window control, so the dialog's creation event is observed while the dialog is open. The pixel case checks the dragged rectangle and the sampled color.
- **Results (current tree):**
  - GUI walkthrough `out/linux-gui/walkthrough-20261012T230828Z-c854e4a3`: 19/19 required cases passed. Idle LayerE used 8.55 MiB USS (startup peak 8.56 MiB) at 0.0% CPU over 60 s. Each advanced view, sampled for 60 s at 0.0% CPU, stayed within the 48 MiB / 0.2% targets: JSON 10.32 MiB USS, simulator 10.48, stopped trace 10.60, stopped calibration 12.24 (16.42 PSS, 28.26 RSS). Cleanup was complete.
  - Debian WSL `run-linux-tests.sh --flatpak required --editor required` (`out/linux-automation/c8a-20261012T232152Z`): overall pass. The build had no warnings; 21 headless, 5 X11 and 1 editor CTest suites passed with none skipped; the native and Flatpak smokes passed. The native editor used 13.23 MiB USS and the Flatpak editor 14.43 MiB, both at 0.0% CPU. The package stage was off.
  - Windows MSVC Debug rebuilt without warnings; the 8 affected CTest suites passed.
- **Review:** the final reviewer pass found two High issues. Calibration assets were published in their own transaction before the bundle Save. A duplicate's first Save dropped assets that Undo could restore. It also found two Medium issues: a merge refusal discarded the draft, and an unconfirmed durable write was reported as "Not saved". All four are fixed with regression tests. The confirmation pass found no High. Its two Medium issues (a reopened dialog could not read unpublished owned bytes; abandoned bytes accumulated) and one Low (the walkthrough assumed Cancel closed the dialog) are fixed. A second confirmation found no new findings.
- **Remaining:** no GUI case deliberately attempts an accessible mutation of an underlying control while calibration is open; the gate is verified by inspection. The later ownership split is recorded in the 2026-10-18 audit qualification below.
- **Not covered:** physical desktops and compositors, real games, CI (not pushed), and the Windows-only isolated visual runtime (preserved, reported unavailable).

### Live draft and autosave evidence (Change 8c, 2026-10-13)

- **Protocol:** `src/core/live_draft_control.*` is a platform-free, typed and versioned payload (`draftVersion` 1; publish/renew/end; replies applied/duplicate/renewed/ended). It enforces exact fields, bounded client tokens, a 256 KiB profile body and revisions within the signed 64-bit range. It travels as the `draft` command of the authenticated `LayerC.Control` v1 socket. The shared `layerc_core_tests` cover it on Windows and Linux.
- **Daemon:** `LiveDraftHost` (`src/daemon/linux/live_draft.*`) holds at most one editor's draft for a stored profile. Ownership is per client, revisions are monotonic, a duplicate renews, and a 2.5 s lease runs on the monotonic clock. The draft replaces only the base profile and only in memory; ending it, letting the lease lapse, or removing the stored profile restores the committed profile, and the daemon loop wakes at the lease deadline. Changes caused by an expiry are reported even when the request that triggered the expiry is refused.
- **Editor:** stored profiles autosave through the shared durable transaction, text after 400 ms and other edits immediately. Only lists and preview refresh, so focus and the text buffer stay intact. Invalid raw text is reported and never written. A new profile needs one Save; a legacy format pauses autosave and live publication until an explicit Save approves the upgrade. One long-lived worker per editor publishes accepted base-profile edits, renews each second and serializes End before another profile's publish. It wakes the FLTK loop through an eventfd only for publish replies and failures; results carry a publish sequence, so stale ones are ignored. `end()` only queues a request; only shutdown joins.
- **Results (current tree):**
  - GUI walkthrough `out/linux-gui/walkthrough-20261013T050633Z-c0184420`: 23/23 required cases passed. The four 8c cases are autosave-history-invalid, live-draft-lease, autosave-legacy-consent and autosave-offline-recovery. With the store blocked, the real LayerD acknowledged the draft and the overlay moved; it was still applied after the 60 s sample (24 lease periods); Undo restored it; the profile bytes were unchanged. Idle LayerE used 8.65 MiB USS (startup peak 8.65 MiB) at 0.0% CPU. Advanced views, each sampled for 60 s: JSON 10.55 MiB, simulator 10.64, stopped trace 10.79, stopped calibration 12.41, stopped autosave 12.41 (all at 0.0%), and an active live draft 12.41 MiB at 0.033% CPU, within the 48 MiB / 0.2% targets. An earlier run with a 100 ms UI poll measured 0.38% CPU with a draft active; the eventfd wakeup replaced that poll.
  - Debian WSL `run-linux-tests.sh --flatpak required --editor required` (`out/linux-automation/c8a-20261013T050025Z`): overall pass. The build had no warnings; 22 headless, 5 X11 and 2 editor CTest suites and 75 tool tests passed; the native editor used 13.24 MiB and the Flatpak editor 14.35 MiB, both at 0.0% CPU. The package stage was off. It ran before the final walkthrough-only comparison fix (no product code changed).
  - Windows MSVC Debug (`c8c-win-20261013T040752Z`): built without warnings; 5 affected CTest suites passed, including `layerc_core_tests` with the draft codec.
- **Review:** three part reviews found no High. Fixed Medium issues: routine autosave joined the draft worker on the UI thread; a stale failure could leave a newer lease unended; a hidden window left the eventfd readable; harness acknowledgement checks were not part of the poll; the live-draft case did not prove renewal; the legacy case compared too little; the documentation omitted the legacy publication pause and failed-save prompts. Low items were also fixed: an eventfd leak on constructor failure, stale status after a no-op flush, a publish after End at shutdown, and the missing regression for expiry during a refused request. The confirmation pass found no High and no new Medium.
- **Audit repair (2026-10-15):** the shared base-profile parser/serializer retain opaque top-level and nested object fields with their owning entities, including module descriptors, bindings, HID controls and chord tokens. Numeric JSON source digits, including full-width integers, long decimals and `-0`, are preserved rather than rounded through `double`; known integer readers retain their behavior. Invalid opaque values fail before the durable transaction; the failed draft and existing disk bytes remain intact. Focused `layerc_core_tests` and `layerc_editor_document_tests` pass on Windows MSVC Debug and Debian WSL GCC without build warnings, including repeated durable saves, recovery-copy checks and the review-driven negative-zero regression. The independent static confirmation found no substantiated issues. Core repair commit: `13f31d0`; regression evidence below is explicitly working-tree evidence, not a final-package qualification.
- **Remaining:** reply revisions above the signed 64-bit range are not representable; the daemon never produces them.
- **Not covered:** physical desktops, real games, CI (not pushed).

### Diagnostics, device capture, appearance and placement evidence (Change 8d, 2026-10-13)

- **Diagnostics and help:** `src/editor/common/runtime_diagnostics_view.*` formats the daemon status for both editors (Windows `editor_diagnostics.cpp` and the Linux window); View > Runtime diagnostics refreshes on request and copies the raw JSON. F1 lists shortcuts generated from the menu definitions; About has Copy details; F2 focuses the cooldown or profile name; Duplicate cooldown copies timing without bindings.
- **Device capture:** the focused capture dialog records keys, mouse buttons and wheel, and XI2 gamepad-class buttons selected only while the dialog is open. Generic HID capture is stated as unavailable.
- **Tools:** Options states the X11 capture-exclusion and isolation limits. Save As writes the open profile and its modules under a new name. Open profiles folder starts `xdg-open` in its own session with default signals and only stdio inherited, or names the path. Split grouped bars uses the shared `split_grouped_bars` (also used by Windows). Appearance previews presets, an accent and per-role colours, and Apply stores them durably in `<config>/layerc/editor-appearance.json` (empty overrides are omitted).
- **Detected Tasks and placement:** a short-lived X11 connection scans EWMH top-level windows on open and Refresh only, never listing LayerE; tabs and line breaks in window text cannot add columns. Use selected task goes through the normal edit path (undo, autosave, live draft). Position on window maps the preview to the game window with the overlay's centred-anchor rule, and each drag publishes through the live draft.
- **Results (current tree):**
  - GUI walkthrough `out/linux-gui/walkthrough-20261013T190715Z-c6c944e1`: 30/30 required cases passed, session exit 0, clean cleanup. The 8d cases are runtime-diagnostics (33 lines; LayerD stop and restart reported by Refresh), help-and-commands, device-capture (Q + mouse button 2 saved and undone), tools-options-save-as, split-grouped-bars (anchorY 0.6682/0.8318, undone exactly), editor-appearance (neutral window pixels 85 → 406,406 Teal → 103 preview → 406,772 after Cancel → 78 System) and detected-tasks-placement (anchorX 0.750 → 0.349; the real overlay bar moved from x≈885 to ≈446). Editor idle USS 9.08 MiB, idle CPU 0.0% over 60 s.
  - Debian WSL `run-linux-tests.sh --flatpak required --editor required` (`out/linux-automation/c8a-20261013T192915Z`): overall pass, no build warnings; 23 headless, 5 X11 and 2 editor CTest suites and 75 tool tests passed. The native editor smoke used 13.65 MiB USS at 0.05% CPU and the Flatpak editor 14.71 MiB at 0.0% (targets 20 MiB, 0.2%). The package stage was off.
  - Windows MSVC Debug (`c8d-win-20261013T192807Z`): built without warnings; 6 affected CTest suites passed, including `layerc_editor_diagnostics_view_tests`.
- **Review:** six part reviews found no High. Fixed: Duplicate cooldown reported success without a selection; panel-colour labels outlived their storage; Save As lacked a document guard; `xdg-open` inherited descriptors and signal state; an appearance role lookup could dereference null; Detected Tasks actions held references into a list they could refresh; window titles could add browser columns; Use selected task skipped autosave and the live draft; a duplicated cooldown was assumed to be appended; a `noexcept` device refresh could terminate on allocation failure; flaky harness waits (LayerD restart, cancelled capture). The zero-size placement and menu-array findings were already guarded.
- **Remaining (Low):** a running `xdg-open` that never exits blocks a second Open profiles folder; diagnostics show a background session's profile when none is in the foreground. Without a gamepad device, gamepad capture is covered only by unit tests. The explicit-`#000000` accent defect was fixed in the [2026-10-21 rendered audit repairs](#rendered-audit-repairs-and-current-local-qualification-2026-10-21); it is not a remaining limitation.
- **Not covered:** physical desktops, a real gamepad, a desktop with `xdg-open`, real games, CI (not pushed).

### Completion-audit repairs (2026-10-15)

- **Data preservation:** shared-core repair `13f31d0` retains unknown base-profile fields by value with their owning objects. Module-descriptor persistence, HID compaction/remapping, cooldown duplication, durable saves and recovery copies retain the extensions. Known-field collisions, invalid UTF-8 and non-finite opaque values are refused instead of discarded.
- **Editor parity:** Cooldowns > Cooldown groups edits propagation/shared-pool groups as a validated history transaction. Pack export offers per-profile checkboxes, all/none controls, empty-selection refusal and inert Cancel. These replace the two previously missing editor capabilities; platform-unavailable Game Bar and Generic HID remain explicitly unavailable.
- **GUI evidence:** `out/linux-gui/walkthrough-20261015T132458Z-02868c87` passed all 32 required cases, including `cooldown-groups` and `export-profile-checklist`, with session exit 0, no failures and all owned processes joined. Idle USS was 8.91 MiB and CPU 0.0% over 60.24 s. The seven advanced-view samples stayed below 12.63 MiB USS and 0.017% CPU. This is an automated private-Xvfb session on WSL, not physical-desktop evidence.
- **Regression evidence:** the warning-free MSVC Debug build passed 23 CTest suites, with a subsequent focused core/document rerun after the negative-zero review fix. The Linux automation run `out/linux-automation/audit-20261015T131713Z` passed its 57 CTest suites, 88 tooling tests, native/Flatpak installed smokes and editor gates; its package policy was **off**. A separate normalized disposable-source tooling run passed all 90 tests after adding the parity-pack assertions. These runs record their working-tree revisions/deltas; do not relabel them as clean final-commit package evidence.
- **Review:** two initial reviewer dispatches could not inspect source and are not counted as reviews. The subsequent inline-source review found a Medium negative-zero spelling loss, which was fixed and regression-tested; the independent JSON-source confirmation found no substantiated issues. Neither review ran tests itself.
- **Portal consent:** the real GNOME portal returns `session_handle` as a string, which the product rejected; commit `33ee040` accepts `s` or `o` with the same exact-path identity, and `25985aa` tests a successful lifecycle with each representation. `tools/linux-portal-consent-tests.sh` runs GNOME Shell 48 headless, PipeWire, the real xdg-desktop-portal and xdg-desktop-portal-gnome in a bubblewrap user/pid/net namespace with an allowlisted environment and private runtime, bus, home, `/run` and `/tmp` (its `--session` stage refuses to run outside that sandbox), links the probe against product archives built fresh from the tree, and drives the real source picker against the product client: deny (native Cancel → `portal_denied`), cancel (client stop dismisses the picker without a grant) and revoke (fixture window selected and shared → PipeWire remote ready → Shell *Stop Screen Sharing* → `portal_session_closed`). The final bundle `out/linux-consent/consent-20261015T155656Z-973Jfm` (clean `eb09a59`, revision stamp with an empty code diff) passed all three with no leftover processes, verified private namespaces, archive hashes and checksums; all seven runs since the pointer-homing fix (homing at the top-left opened GNOME's hot-corner Overview under the Stop click) have passed, including clean `33ee040` and `25985aa`; the three revoke failures before it are kept in `out/linux-consent/`. Test input is synthesized only inside that owned compositor. This is automated private-session evidence, not physical, VM or KWin evidence.
- **Package:** the `tools/ci-linux.sh` container invocation (run from Windows with host paths) with the digest-pinned Debian image and `--editor required --package required --wayland required` passed every stage on clean commit `25985aa` (`out/linux-ci/debian-stable-20261015T153559Z`; also on `33ee040`, `debian-stable-20261015T150957Z`): build, headless/X11/editor/Wayland/D-Bus/GJS suites, CPack `.deb` check, apt install, installed headless/X11/editor smokes (editor 10.89 MiB USS, 0.0% CPU), apt remove and the post-removal file check. The Ubuntu 24.04 matrix row (`--editor off --package off --wayland required`) passed on clean `25985aa` (`out/linux-ci/ubuntu-2404-20261015T155010Z`). Commit `eb09a59` changes only the consent runner. CI was not run (no push); no release, signing, physical or VM qualification is claimed.
- **Debian WSL:** `run-linux-tests.sh --flatpak required --editor required --wayland required --package off` passed at `eb09a59` (`out/linux-automation/audit-20261015T160533Z`; tree dirty only in documentation): 36 headless, 7 X11, 2 editor, 1 Wayland, 5 D-Bus and 6 GJS suites, none skipped, 90 tooling tests, and native/Flatpak installed smokes. Native/Flatpak editor idle USS was 13.24/13.11 MiB, both 0.0% CPU over 20 s. Package coverage comes from the container run above.
- **Windows:** the full warning-free MSVC Debug build passed 23/23 CTest suites at `25985aa` (worktree differed only in documentation).
- **Consent review:** the first review of `33ee040` found one High (an inherited AT-SPI bus address could reach a host accessibility bus) and three Medium items (direct `--session` runs, an untested successful object-path handle, unverified archive provenance), plus a Low mount-order issue. `25985aa` and `eb09a59` fixed them; two confirmation reviews marked every finding resolved with no new issue. Reviews were by inspection of inline diffs; they ran no tests.

### Rendered audit repairs and current local qualification (2026-10-21)

- **Why this supersedes the rendered verdict:** the completion audit correctly rejected calibration overlap, clipped action/region text and the explicit-black accent defect. The 2026-10-20 numerical/build evidence remains historical; it does not qualify the repaired source or excuse the earlier incomplete pixel review.
- **User-facing repairs:** calibration uses two full-width region rows, separated full-label actions, a dedicated template-reference row and a fixed-size canvas within its scroll. Region precision and canonical template data are unchanged. Appearance distinguishes an absent accent (use the preset) from an explicitly present `#000000`; version-1 settings remain compatible. Apply/reopen, Cancel and Reset all have durable/regression evidence. Multiline fault text, dialog sizing and Visual/Automation tab spacing were repaired. A module-free profile seeds its simulator with the base profile ID; untouched Apply is a no-op. An explicitly loaded empty trace remains empty across reset/rewind/play, whereas a fresh or cleared session retains its normal session-start seed.
- **Rendered acceptance:** the lead opened 19 actual retained whole-desktop PNGs, including eight from the current r16 ELF, and separately opened the current zero-profile PNG. Checks include calibration, Automation, diagnostics/failure states, explicit black, Cancel/reset, minimum viewport and scaling. Default, 110% and native 133⅓% calibration views and four-edge scrolling are covered; native 133⅓% is not described as exactly 130%. Small/high-scale content deliberately scrolls, so controls outside the viewport are not claimed simultaneously visible. Reused r6 calibration/appearance images are limited to byte-unchanged surfaces, supported by the 810-member source-delta proof; current automation/failure images cover the changed path. The detailed observations are in `final-closeout-r16-r1/rendered-acceptance-r1.json` (SHA256 `3afc1b0eac7eae75bad28cfc42a67fb090ab9dc829d6d55ee4da36af6b6dc183`), with the supplementary empty-state acceptance under `final-empty-r16-r1/empty-idle-r1/`. Font/geometry/semantic checks supplement, rather than replace, these pixel observations.
- **Candidate and review binding:** HEAD `6dbb81e91d415580637fe174aeeb55fb2d77b829` plus the inventoried working tree, not a clean-commit qualification. The 63-file reviewed manifest is `code-review-r15/manifest.json`, SHA256 `5c9720795654921f700616e5422f7fafded3c0e06af6b72656ba1ad2077c28ea`. Incremental peer reviews have no unresolved High; the read-only reviewer disclosed earlier implementation involvement and is not represented as an independent third-party execution of tests. The lead reviewed the changes and owned rendered acceptance. The fresh r16 source-change digest is `35a824035cde07556e3415f6e8143f1360a4dde1eb755afe0207c72fcf7059bc`, build stamp `3cd811f18506d644e9f2c865746b64735752b0eba392e9a725fadf95c81a0075`, and complete 810-file final snapshot `95b5abe21a28933c991676f71fec12ccab9bafc42e3d0883e4ad7f068037a595`.
- **Fresh resource evidence:** all 19 frozen validator-derived boundary cases, representative active-operation windows, representative idle/100 cycles and the original 32 walkthrough cases passed. That walkthrough omitted a required controlled-target group and did not close Change 5; the [36-case successor](#required-overlay-walkthrough-audit-repair-2026-10-21) below closes that gap. The zero-profile scenario was separately reacquired on the current binaries, not copied from r12. Failed r13–r15 attempts are retained, with no rows reused. The r16 storage adapter records evidence on native Linux storage and publishes immutable Windows copies; fixtures, samplers, evaluators, CPU windows and the 20/32/48 MiB, 2 MiB/100-cycle and 0.2% targets were not relaxed.

| Current r16 scenario | USS MiB | PSS MiB | RSS MiB | Swap MiB |
| --- | ---: | ---: | ---: | ---: |
| Empty, zero profiles | 7.15 | 10.76 | 21.35 | 0.00 |
| Representative idle | 8.43 | 11.77 | 22.65 | 0.00 |
| Representative active operations | 11.83 | 16.00 | 28.07 | 0.00 |
| Selected boundary maximum: `profiles:panels-byte-limit` | 45.06 | 48.37 | 59.35 | 0.00 |

- **Resource receipts and limits:** values are rounded, component-wise retained sampled maxima; harness processes are separate. Empty and representative idle CPU were 0% over 60.0194 s and 60.228 s. Sampled empty startup USS was 7.15 MiB. After 100 cycles, drift was 12,288 bytes (0.01171875 MiB), with no continuing upward trend. Accepted reports below `.pi/audit-fixes-20261017/resource-audit/` are `post-bounded-save-matrix-r16/resource-verification-r1.json` (SHA256 `ae10d301f6701f4c3f0843facee4f1664e8a176fa5ee0c6077677ae5ad565b80`), `ancillary-verification-r1.json` in the same directory (SHA256 `97d7d2182bd6498f0faeebf7fa7d312df9cea5469691859bf2ac0d7e0d442b03`), and `final-empty-r16-r1/empty-idle-r1/lead-verification-r1.json` (SHA256 `c0b42a72b08e9443c2400ff9029f1fb1d7ffb7ab70b8b2e2bc72b0292e698e2f`). Presentation inputs are frozen in `final-closeout-r16-r2/frozen-resource-summary-r1.json`, SHA256 `8fcd97dbaaee78d7689703b8d07117c3f72e19216e3248e9ec275b37f9e207c8`. Startup reports a 5 ms sampled maximum without retaining individual startup readings; cycle medians lack constituent readings, and advanced-idle CPU lacks raw tick endpoints. No unsampled peak, universal valid-document, GPU-memory or physical-desktop bound is inferred.
- **Broad local qualification:** the fresh Windows MSVC Debug build passed all 22 registered CTest suites, with no skipped tests. Fresh Debian WSL editor/Wayland/Flatpak gates and the pinned Debian container editor/Wayland/package gates each passed 60 suites (39 headless, 7 X11, 2 editor, 1 Wayland, 5 D-Bus, 6 GJS), with no required skips. Native and Flatpak installed smokes passed; the native package was built, checked, installed, smoke-tested and removed. LayerC targets are warning-free; pinned upstream dependency diagnostics are inventoried separately in the qualification receipt, not suppressed. The original final attempt failed when Docker was unavailable; its successful Windows/Linux stages and failed completion remain intact. The successful package-only retry used the same pinned image and logged owned `Xvfb -noreset` fixture, not changed source or weaker gates.
- **Final source reconciliation and scope:** source-bound acceptance before documentation is `final-closeout-r16-r2/qualified-before-docs.json`, SHA256 `3c18be57718dfed5ab2d837d7c9c5b677e74462576bcee8807d291b68890f7e1`. The final reconciliation in that directory permits exactly `README.md`, `docs/Linux.md`, `docs/LinuxEditorPlan.md` and a local test recipe, with no runtime/test/build delta. Raw automated evidence remains under `out/linux-gui/`, `out/linux-automation/resource-r16-final-r1/` and `out/linux-ci/resource-r16-final-r2/`.
- **MinGW follow-up:** a first clean MinGW-w64 UCRT64 GCC 15.2.0 Release build passed all 22 registered tests but emitted 19 warnings, so it was not accepted. Explicit default initializers in two Windows-only production translation units and six Windows-only test fixtures repaired them. Windows matching and the unsupported Windows PipeWire-backend result are unchanged. No assertions, CMake rules or warning flags changed. A fresh clean MinGW Release build then had zero configure/build warnings and passed all 22 tests: `mingw-r16-r4/lead-verification-r3.json`, SHA256 `e75c6d8c5569e141ba5949fc52791697c73537a35772018fdca122bc8dd441a5`. Two earlier verifier-script failures are retained. MSVC Debug rebuilt the six affected suites without warnings, and all six passed (`msvc-warning-followup-r16-r1/lead-verification-r1.json`); this was incremental, not a fresh full MSVC build. Linux runtime, headers, GUI fixtures and resource protocols are byte-unchanged, so the r16 Linux, rendered and package evidence above was not rerun. `mingw-closeout-r16-r1/` contains the source proof, which permits only this document to change after qualification. No push, PR, tag, CI run, publication, signing, physical/VM, real-gamepad, real KWin/GNOME or screen-reader-user acceptance is claimed. v2.6c remains unpublished, X11-only; Wayland remains V2.7 development and Gamescope unsupported.

### Required overlay walkthrough audit repair (2026-10-21)

- **Required, not deferred:** the full X11 walkthrough now includes `overlay-focus-changes`, `overlay-target-resize`, `overlay-click-through` and `overlay-cleanup`. The previous 32-case pass omitted this required group; lower-level runtime tests did not replace it. The fresh complete bundle `out/linux-gui/walkthrough-20261021T185536Z-9b356eac` passed all **36 required cases**, with no required skips. Wayland/provider/portal cases remain in their separately required runners, not silently counted as X11 coverage.
- **Observed process/desktop outcomes:** the binding painted one overlay on the focused fixture; focusing LayerE hid the external overlay while retaining the session, and refocusing restored it. Real corner drags changed the target client from 1100×650 to 700×400 and back, moving the 388×44 overlay from `(692,566)` to `(392,378)` and back. Actual `xev` ButtonPress/ButtonRelease observations confirmed both a control click beside the overlay and a click on its painted `#50bac8` pixel at root `(886,588)`, delivered to target-local `(825,488)`; the overlay remained mapped. Disabling/re-enabling the profile removed/restored its session and overlay; stopping/restarting owned LayerD removed/restored the overlay. Final cleanup joined all owned processes, removed the private display locks and sandbox, and reported no leftovers.
- **Real preconditions and recovery retained:** the appended cases disable only conflicting fixture copies through the real editor, verify read-only profile IDs, and restore the selected fixture after the existing failed-save test. They do not delete profiles, bypass autosave or touch user state. Earlier failed attempts remain intact. A Windows-host attempt to import the Linux evaluator failed at `os.sysconf`; the byte-identical verifier succeeded inside Debian WSL, without changing the evaluator or targets.
- **Evidence and review:** source snapshot `a8b7502290865aad034509962a3e1fc470f79ce50555f246f48d0987704ab759` contains 812 files. Under `.pi/audit-fixes-20261017/resource-audit/overlay-walkthrough-r4/`, `lead-verification-r1.json` checks the source/manifest, actions, dynamic outcomes, original evaluator and 12 rejecting negative controls; `peer-review-r1.json` records the read-only source review; `rendered-acceptance-r1.json` records the lead's direct inspection of eleven original whole-desktop PNGs. The near-expiry narrow-fill frame has partly clipped cooldown text, disclosed in that scoped state review rather than claimed universally unclipped. This review establishes the required state changes, not universal typography or physical-desktop acceptance.
- **Source bridge and scope:** `qualified-before-docs.json` and `final-source-reconciliation.json` bind this harness-only successor to `completion-evidence-r16-r2.json`. Three GUI-harness files changed and two harness/test files were added; compiled product sources, CMake, package payload/manifest and resource samplers/evaluators did not change. The earlier compiler/resource/package results are retained, not relabeled as rerun. Only `README.md`, `docs/Linux.md`, this plan and a local test recipe change after the fresh walkthrough qualification. All results are automated WSL/private-Xvfb evidence; CI, physical/VM, real-gamepad and publication remain unclaimed.

<a id="resource-audit-open-2026-10-19"></a>
<a id="resource-audit-and-current-local-qualification-2026-10-20"></a>

### Historical resource qualification (2026-10-20)

**Superseded rendered verdict:** this r12 snapshot retains its original numerical/build evidence, but its rendered acceptance was rejected. Use the [2026-10-21 repairs and fresh qualification](#rendered-audit-repairs-and-current-local-qualification-2026-10-21) for the current tree; do not relabel the older rows as measurements of repaired source.

- **Auditor objections addressed:** the rejected 2026-10-18 advanced-view observations were idle/stopped views, not operation peaks, and lacked validator-derived worst-case/exhaustion coverage. Fresh r12 evidence measures explicit simulator-play, trace-recording, X11 capture, template-processing and template-matching windows, and all 19 validator-derived boundary cases. No resource ceiling, idle-CPU window or required case was relaxed. The earlier idle-view table below remains historical and is not used as active-peak proof.
- **Repaired source:** bounded saves retain pretty output when it fits, otherwise use lossless compact output, and refuse over-budget output with typed `resource_limit` before durable effects. Shared immutable JSON/history payloads reduce copying; copy/move assignment snapshots a self-descendant before replacing its owner. AT-SPI uses a per-pass menu-size cache and exports browser rows only when visible. Every unavailable feature and unsupported field remains preserved. The 52-file repair manifest is SHA256 `fbb729762a1227b31a765bfef555fd57b5c95315e03b22c920c2f075bd8d3ca6`; accepted incremental reviewer receipts cover that scope, with no unresolved High or Medium. Focused Debug, sanitized/negative-control and MSVC evidence predates and complements, rather than replaces, the fresh broad gates.
- **Candidate binding:** HEAD is `6dbb81e91d415580637fe174aeeb55fb2d77b829` plus the explicitly inventoried worktree, not a clean-commit claim. The r12 acquisition source-change digest is `4e14b6146d6d2bb607f1c02f91ddb4650f8ff115c8d6432de3d00fd0f49190d2`, build-stamp SHA256 `bd9fc30317caa570c359c32e6b9a39c8e5946e92eac28c7073eaf6b65351ccf7`, and full 809-file snapshot SHA256 `18ee0fe4f307ec55a7d2160ba8c060379ac510af59e0c41d754daf7c2d2ee807`. r1–r11 are retained attempts, not reused rows; in particular r11's post-run source guard failed after a documentation edit, so every r12 row was acquired fresh. Frozen fixtures were restored and checksum-verified, not regenerated.
- **Lead verification:** raw samples, process identities, source/build checksums, active windows, semantic comparisons, refusal/recovery effects and cleanup were checked independently of job exit codes. Fourteen retained whole-desktop PNGs were personally inspected for boundary open/operation/refusal/recovery, active advanced operations and walkthrough recovery/fault states. Original failed offline reports are retained: the r8/r7 adapters' visual authored-member comparison incorrectly distinguished JSON `0.0` and `0`. The append-only r9/r8 successors use a numeric-value oracle checked by 28 positive/negative controls; numerical/window/CPU/checksum evaluators and limits are unchanged. Accepted local reports are `post-bounded-save-matrix-r12/resource-verification-r2.json` (SHA256 `401ef5344a6075f032cac1470f83cf894dfe98ddf5ef32ff635e3a22c65758b5`) and `ancillary-verification-r2.json` (SHA256 `a45a227427a6b42ed4d27e6d170be3936b694a63609547c6a8176bbab8adefb4`) under `.pi/audit-fixes-20261017/resource-audit/`.
- **Resource results (USS):** a separate truly empty, zero-profile editor passed at 7.421875 MiB and 0% CPU over 60.0077 s; the 20-profile × 10-cooldown × 2-panel store passed at 8.64453125 MiB and 0% over 60.002 s. Active representative operations peaked at 12.55859375 MiB. All 19 selected boundary cases passed the unchanged 48 MiB bound; the largest retained peak was 44.8125 MiB (`profiles:panels-byte-limit`), with visible refusal and independently verified durable recovery. Drift after 100 cycles was 12,288 bytes (0.01171875 MiB), with no continuing upward trend; all 32 required walkthrough cases passed. These remain distinct from their harness processes and from historical idle-view observations.
- **Measurement limits:** active/boundary peaks are maxima of retained samples, not proof of unobserved sub-interval peaks or every possible combination of valid profile fields. The zero-profile run retained startup maxima at 5 ms sampling and raw 60 s idle samples; individual startup readings were not retained. Cycle medians lack their constituent 0.1 s readings; advanced-idle-view CPU is a reported observation without raw tick endpoints. Raw idle CPU endpoints, active-operation windows and resource limits were verified separately. No GPU-memory or physical-desktop conclusion is inferred from Xvfb.
- **Broad requalification:** one fresh MSVC Debug build passed all 22 CTest suites; the subsequent tooling-only repair in `tools/tests/test_linux_gui_visual.py` changes a heredoc lookup and adds a uniqueness assertion, with all runtime, build and CTest inputs unchanged. That delta passed 17 focused tests and is explicitly bound to the retained Windows result, rather than relabeling Windows as rerun. Fresh isolated Debian WSL editor/Wayland/Flatpak gates and the digest-pinned Debian container editor/Wayland/native-package gates each passed 60 CTest suites (39 headless, 7 X11, 2 editor, 1 Wayland, 5 D-Bus, 6 GJS), with no required skips or failures. Native and Flatpak installed smokes passed; the `.deb` was built, checked, installed, smoke-tested and removed. LayerC CMake targets are warning-free; the pinned Flatpak dependencies retain three explicitly reviewed AT-SPI compiler warnings and one FLTK configuration notice for disabled FLUID. No diagnostic flags were suppressed. The failed Docker-engine and X11-skip attempts remain frozen; the successful container-only retry used a temporary, logged `Xvfb -noreset` fixture without changing source or weakening gates. A controlled probe supports that fixture mechanism, not a unique historical root-cause claim. The source-bound acceptance before documentation is SHA256 `4716b49fe5185284bf5a4f4081079c132286cb67f0d213483f10a09e12dc3c89`; the final reconciliation permits exactly these four documentation changes, and no runtime/test delta.
- **Scope and remaining gaps:** automated WSL/Xvfb, Flatpak and disposable-container evidence is not physical GNOME Xorg/KDE X11, VM, real gamepad, real KWin or an assistive-technology user study. `ci.yml` was not run; no push, PR, tag, publication, signing or release was performed. The historical records below remain tied to their original revisions and archives. Current raw resource evidence is live under `out/linux-gui/`; the accepted local provenance and final source reconciliation are under `.pi/audit-fixes-20261017/resource-audit/`.

### Audit rerun and final local qualification (2026-10-18)

- **Snapshot:** automated qualification of `v27/linux-editor` at HEAD `0c06511613559fb3ac78fc923decb7c574bbcc7b` plus the intentional working-tree changes, not HEAD alone. The qualified tracked-diff SHA-256 is `9041393caad595402f8c60c71c40019580f397eb948437fa0487109bb3a8b61d`; the GUI source-change digest is `9cea95558c0eac24f46becc8fb94c3315579b72a1e5fa8c4bab263b8661adc4a`. The frozen normalized source manifest and acceptance receipts are under `.pi/audit-fixes-20261017/theme-final/`. This status reconciliation changes only this plan after qualification; it does not relabel the dirty snapshot as a clean release commit.
- **Audit corrections:** losing a window provider follows normal session retirement and checkpoint retry; demanded failed overlays retain visible faults and monotonic recovery deadlines; simultaneous X11 faults are retained per target; all three unconditional neutral-session/route suites belong to the mandatory headless inventory. The earlier ownership refactor split accessibility into the semantic implementation, tree/reconciliation owner and 157-line bridge adapter, preserving the public facade. The final rendered check additionally caught and fixed unreadable tabs after Teal-to-System restoration. Discriminating regression tests and independent static confirmations close these findings; verified reports are in `review/archiv/layer46.review-2026-10-16.md` through `layer49.review-2026-10-18.md`. Static review is not runtime evidence.
- **Windows (historical snapshot):** the frozen warning-free MSVC Debug qualification passed all 23 CTest suites with no failures or skips. At that qualification's final source reconciliation, only `src/editor/linux/appearance_window.cpp` and `tests/linux_editor_widget_tests.cpp` had changed; the shared and Windows inputs were unchanged, and Windows was **not rerun** against that full-tree digest. The equivalence/disclosure receipt is `.pi/audit-fixes-20261017/theme-final/source-scope-acceptance.json`. Subsequent shared-core repairs supersede that equivalence for the current tree; the fresh broad gates and explicit source bridge are recorded in [the current source-bound resource audit](#resource-audit-and-current-local-qualification-2026-10-20).
- **Linux and packages:** `out/linux-automation/theme-audit-20261018-isolated` passed the required editor, Wayland and Flatpak gates; `out/linux-ci/theme-audit-20261018-isolated` passed the digest-pinned Debian container job with required editor, Wayland and package gates, including `.deb` validation, install, installed smokes and removal. Each job passed 60 distinct CTest suites: 39 headless, 7 X11, 1 Wayland, 5 D-Bus, 6 GJS and 2 editor, with no required skips, failures or compiler warnings. The native and Flatpak installed smokes passed. The Flatpak retains the exact seven declared permissions and no network sharing. These are local working-tree results, not GitHub CI or publication. Full source, raw XML, required-stage and artifact-checksum acceptance is in `.pi/audit-fixes-20261017/theme-final/cli-package-reviewed.json`.
- **Fresh GUI and resources:** `out/linux-gui/walkthrough-20261018T005941Z-ce4ec5e0` passed all 32 required cases; `out/linux-gui/resources-20261018T011806Z-30e7e929` passed the representative fixture and 100-cycle checks. Idle USS was 9.16 MiB at 0.0% CPU over 60.232 s; the 20-profile × 10-cooldown × 2-panel fixture used 9.44 MiB at 0.0% over 60.213 s; the observed maximum across seven advanced views was 13.02 MiB USS (rounded) and 0.0166% CPU. The 100-cycle USS drift was 16 KiB, without a continuing upward trend. All are within the unchanged targets, but the advanced views were sampled open and idle; they are not active-peak proof. See [the current source-bound resource audit](#resource-audit-and-current-local-qualification-2026-10-20). Raw-sample, source, checksum and owned-process-cleanup acceptance is in `.pi/audit-fixes-20261017/theme-final/gui-lead/qualification-check.json`; frozen presentation is `summary.json`.
- **Rendered check:** the lead inspected the changed minimum-size and save-failure screenshots, confirming readable System-theme tabs, retained native list highlights, scrollbars and an explicit failed-save message. Seven other representative PNGs are byte-identical to earlier-inspected images. The separate rendered verdict is `.pi/audit-fixes-20261017/theme-final/gui-lead/render-review.json`; numeric or DOM checks alone are not a visual verdict.
- **Remaining boundaries:** no push, GitHub CI, release/tag, signing, VM or physical-desktop qualification; no screen-reader user study, real-game/gamepad coverage or new native KWin/GNOME support claim. The genuine Low items and per-compositor gaps remain recorded below and in [Wayland.md](Wayland.md#compositor-evidence-and-gaps). This recorded local automated implementation/packaging/accessibility acceptance for that snapshot only, not those unobserved qualifications; the later resource objection and source changes reopened it. X11 remains the unpublished v2.6c scope (package 2.6.2); native Wayland remains V2.7 in development.

### Final review and evidence gaps (2026-10-13, historical)

Scope: branch `v27/linux-editor` against `main` (Changes 1–6 and 8; 208 files). Every change had its own reviews; this pass added a constraint audit and three cross-cutting reviews (build graph, user documentation, Linux gates and packaging).

- **Constraint audit:** Xtst links only into the two X11 test targets; added sources contain no network APIs, no wall clocks and no platform headers in `src/core` or `src/editor/common`. `layere` forces the FLTK X11 backend (`FLTK_BACKEND=x11`), so Debian's hybrid FLTK 1.4 cannot select Wayland.
- **Reviews:** no High findings. Fixed: `LAYERC_REQUIRE_EDITOR` is now declared in `CMakeLists.txt` and fails configuration when the editor or X11 is missing; a failing `fltk-config` no longer aborts builds that do not require the editor; `tools/ci-linux.sh` rejects images that are not digest-pinned, a missing evidence directory and `--flatpak` options; `docs/Linux.md` no longer describes CI as run, the editor row and X11 limits are current, and harness input is stated as test-only; README links point to tracked documentation. Required editor suites already fail when skipped (no `allowSkip` in `tools/test-gates.json`).
- **Results (commit `5ced4e6` plus these fixes):**
  - Windows MSVC Debug, full build (`c9-win-20261013T195027Z`): no warnings; 23/23 CTest suites passed.
  - Debian WSL `run-linux-tests.sh --flatpak required --editor required` (`out/linux-automation/c8a-20261013T195108Z`): overall pass, no warnings; 23 headless, 5 X11 and 2 editor suites (none skipped) and 75 tool tests. The native editor used 13.39 MiB USS and the Flatpak editor 14.62 MiB, both at 0.0% CPU (targets 20 MiB, 0.2%). The package stage was off in this run; the last package run is the Change 6 Debian container run.
  - GUI walkthrough `out/linux-gui/walkthrough-20261013T190715Z-c6c944e1`: 30/30.
- **Evidence gaps:**
  - **CI not run:** nothing was pushed, so `ci.yml` (Windows, Debian/Ubuntu containers, Flatpak) has no recorded run for this branch.
  - **Physical and VM:** no GNOME Xorg or KDE Plasma X11 session, real game, gamepad, multi-monitor/HiDPI desktop or screen-reader user study; all Linux evidence is Xvfb/WSL/container automation. Physical native and Flatpak evidence remains pending.
  - **Packaging:** the `.deb` install/remove smoke passed in the Debian container on commit `25985aa` (see Completion-audit repairs). The single-run release-evidence mode (`LAYERC_RELEASE_EVIDENCE=1`) needs one root container that can also run Flatpak; release evidence therefore combines the container package job and the Flatpak job at one commit. Nothing is published or signed.
  - **Wayland:** Change 7's native/degraded backend and XWayland editor paths are
    implemented locally as V2.7 (not a v2.6c support claim). The editor reports
    display/provider rows and labels Detected Tasks and placement XWayland-only.
    Headless sway walkthrough: 5/5; unchanged X11 walkthrough: 30/30. The final
    c7h implementation, including Flatpak permissions and required-mode fixture
    enforcement, passed `run-linux-tests.sh --wayland required --editor required
    --flatpak required --package off` (`out/linux-automation/c7h-20261015T102053Z`,
    commit `5c8e8fa`; dirty only for the preserved user change and untracked
    docs): 36 headless, 7 X11, 2 editor, 1 Wayland, 5 D-Bus and 6 GJS suites,
    none skipped, and 88 tooling tests. Native/Flatpak editor idle USS was
    13.26/13.09 MiB, both 0.0% CPU over 20 s. Workflow lint passed with
    checksum-verified actionlint 1.7.12. Review found no High; its
    missing-protocol-fixture Medium was
    fixed and independently confirmed. The native-container package rerun later
    passed on `25985aa` (see Completion-audit repairs).
    Per-compositor gaps are in `docs/Wayland.md`; no real KWin/GNOME, VM or
    physical-desktop evidence, and CI has not run.
- **Open Low items:** the Flatpak grants read-write `xdg-documents` and `xdg-download` instead of portal file choosers; the Flatpak scratch root is kept after a failed run; without `dbus-run-session` the editor smoke runs without an accessibility bus; the post-removal package check covers a fixed file list; sysroot builds can mix host and sysroot FLTK flags; FLTK compile flags are public and not system includes; two metainfo files carry a release date for the unpublished 2.6.2; the Change 8d Low items above.

### Packaging and CI integration evidence (Change 6, 2026-10-10)

v2.6c carries package version 2.6.2 (`CMakeLists.txt`, both metainfo files). Evidence is automated only: Debian WSL and digest-pinned containers, not a physical desktop or a CI run.

- **Install tree:** with `layere`, `cmake --install` adds the desktop entry, hicolor icons (48–256 px) and a desktop-application metainfo; builds without the editor keep the console-application metainfo.
- **Gates:** `run-linux-tests.sh` gains `--editor` (default `auto`) and `--package` (default `off`); release evidence requires both, plus Flatpak. The FLTK widget suite moved from the `x11` gate to its own `editor` label and `linux-editor` gate, so builds without the editor keep an unchanged X11 gate and an editor gate fails if the suite is missing or skipped. `required` sets `LAYERC_REQUIRE_EDITOR`. `tools/linux-editor-smoke.sh` starts the installed editor beside the installed daemon, requires its window, records USS/PSS/RSS and idle CPU against the 20 MiB / 0.2% targets, and quits it through Ctrl+Q.
- **Native package:** CPack builds `layerc_2.6.2-1_amd64.deb` from a separate `/usr` Release tree; `tools/linux_package_check.py` checks its version, ATK/AT-SPI ≥ 2.62 and FLTK dependencies, contents, notices, desktop entry and AppStream data. Inside a container that its launcher (`tools/ci-linux.sh`) declared disposable, the gate installs it with apt, runs the headless, X11 and editor smokes against `/usr/bin`, removes it and checks that the package and its files are gone.
- **Flatpak:** the manifest builds FLTK 1.4.3 (static) and at-spi2-core 2.62.0 from checksum-pinned archives, requires the editor and runs it by default. At the Change 6 snapshot, permissions were `--socket=x11` plus `xdg-documents` and `xdg-download` for the import/export chooser, with no network, device, home, Wayland or session-bus access. Subsequent V2.7 Change 7 adds `--socket=wayland` and scoped talk access to `org.kde.KWin`, `org.gnome.Shell` and `io.github.EnderKeera.LayerC.GnomeShell`; it adds neither network/device/home access nor unrestricted session-bus access. The gate counts only a system-wide 25.08 runtime, builds in a per-run directory beside the build tree (with flatpak-builder's state directory inside it and without rofiles-fuse), runs the headless, X11 and editor smokes, exports `LayerC.flatpak` and removes the directory after success.
- **CI:** the `debian:stable` job runs `--editor required --package required`; `ubuntu:24.04` (AT-SPI 2.52) runs `--editor off --package off`. The Flatpak job installs `bzip2`, `xz-utils`, `dbus` and `xdotool` for the archive sources and the editor smoke. Workflow lint (pinned actionlint) passes. CI has not run, because nothing was pushed.
- **Results:**
  - Debian 13 container, as the CI job runs it (`out/linux-ci/c6-debian-20261010T154450Z`): every stage passed, including install, the three package smokes and removal. The installed-package editor used 10.36 MiB USS / 13.48 PSS / 20.75 RSS at 0.0% CPU over 20 s; the native install used 10.41 MiB USS. The `.deb` is 2,988,724 bytes.
  - Ubuntu 24.04 container (`out/linux-ci/c6-ubuntu-20261010T155627Z`): headless, X11 and native smokes passed; editor and package were recorded `not-run`, as configured.
  - Debian WSL with `--flatpak required --editor required` (`out/linux-automation/c6-20261010T155204Z`): every stage passed. The native editor used 12.85 MiB USS at 0.0% CPU; the Flatpak editor used 13.86 MiB USS / 16.30 PSS / 18.75 RSS at 0.0%. The bundle is 2,080,648 bytes, and the staging directory was removed. Local harness detail: WSLg mounts `/tmp/.X11-unix` read-only, so this run gave Xvfb a private writable socket directory through bubblewrap.
  - Windows MSVC Debug rebuilt without warnings, and 20/20 CTest suites passed.
- **Review:** three static reviewer passes (gate scripts; CMake, manifest and CI; docs) found no High-severity issue. Their Medium findings are fixed: the package stage now installs only inside a container its launcher declared disposable; the untested bubblewrap root helper was removed; stale editor binaries and install trees no longer stand in for a build; only system Flatpak runtimes count; Flatpak staging leaves the source tree; and the Debian install command uses `-t trixie-backports`. Low fixes include an exact-one `.deb` check, a fuller removal check, a checker for notices, the user unit and paths outside `/usr`, a real package description, the editor's window class matching its desktop ID, and the Flatpak copy skipping local agent directories. `--share=ipc` stays unset on purpose.
- **Not covered:** physical GNOME/KDE installs, a published repository or Flathub, and package upgrades from an earlier `layerc` package (none was ever published).

## 10. Implementation sequence

| Change | Scope | Acceptance and dependency |
| --- | --- | --- |
| 1. Baseline and toolkit prototypes | Standalone editor measurement and representative UI comparison; no production toolkit change. | Record Windows baseline and Linux candidates; choose the toolkit and freeze reviewed limits before Change 3. |
| 2. Shared document behavior | Extract required document/history/mutation/commit units; adapt Windows to the same units. | Focused Windows tests and new Linux document/round-trip/recovery checks pass. Can proceed before toolkit selection. |
| 3. Linux editor vertical slice | Toolkit shell, one disposable profile, one cooldown/panel, Save and daemon status/reload. | UI -> durable profile -> daemon overlay works; invalid/save/disconnect paths are visible. Requires Changes 1 and 2. |
| 4. Computer-use bridge and walkthrough | Linux driver, isolated session, fresh screenshots, resizing, menus, evidence, ownership/cleanup. | Windows-side agent drives the actual slice and verifies saved/runtime outcomes. Can begin during Change 1; complete after Change 3. |
| 5. First-release feature set | Remaining basic profile/binding/panel operations, import/export, undo/redo, validation and accessible interaction. | Scope matrix and required walkthrough cases pass within the resource gates. Requires Changes 3 and 4. |
| 6. CI/install/package integration | Editor target/dependencies, explicit required tests, installed smoke, memory reports, native/Flatpak permissions and docs. | Source-only/headless builds retain clear behavior; selected editor gates cannot silently skip. Requires Change 5. **Done (2026-10-10)**; see the Change 6 evidence above. |
| 7. Wayland integration | Consume completed V2.7 providers; verify editor/backend faults, capture consent, scaling and cleanup. | Implemented locally (2026-10-15), after the prerequisite merge. Headless sway/XWayland, X11 and private-bus evidence pass; actual KWin/GNOME, VM/physical and CI gaps remain explicit. See the final review above and `Wayland.md`. |
| 8. Advanced parity | Automation, simulation, calibration, live draft/autosave and other deferred UI, each independently reviewable. | Per-feature semantic, GUI, resource and recovery evidence; no blanket parity claim. 8a (automation), 8b (visual/calibration), 8c (live draft/autosave) and 8d (diagnostics, device capture, appearance, live placement) are done; see their evidence above. |

Each change should be a small reviewable commit/PR as appropriate. Preserve current user changes and existing Wayland lanes. Resolve exact targets and commands from CMake/source when implementing. No estimates or completion claims are attached to unfinished work.

## 11. Review decisions

1. Approve or revise option B and the first-release scope; keep full parity separately tracked.
2. Approve or revise the proposed editor memory/CPU gates and reference fixtures before toolkit selection.
3. Confirm the first two toolkit prototypes, including accessibility requirements and packaging constraints.
4. Confirm whether native and Flatpak editor installation are both required for the first editor release.
5. Assign a release label and reconcile it with the active V2.7 Wayland branch and historical roadmap.

The separate implementation goal approved the X11 v2.6c scope, native and Flatpak packaging, the comparison candidates and the resource targets. The toolkit gate is closed by the measured comparison and six review rounds recorded above. Changes 1–6 and 8 have local automated implementation, packaging and AT-SPI/GUI acceptance, refreshed by the 2026-10-21 rendered/resource and source-bound local qualification; Change 7 is implemented locally after the prerequisite merge and remains V2.7 development work. Publication, GitHub CI, physical-desktop/screen-reader-user and outstanding compositor qualification are not complete. This plan does not change the released support matrix in [README.md](../README.md) or [Linux.md](Linux.md); update those support claims only when implementation and acceptance evidence justify them.
