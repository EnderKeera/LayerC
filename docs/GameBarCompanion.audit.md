# Game Bar Companion V2.4 — Release Evidence Record

This file tracks the **Game Bar companion only**. It is not the current status
of the whole LayerC project and does not claim that a signed MSIX has shipped.
The architecture contract lives in [`GameBarCompanion.md`](GameBarCompanion.md);
project-wide status lives in [`../README.md`](../README.md).

## Implementation baseline

The V2.4 host, activation path, App Service IPC, eight panel widgets, settings
widget, snapshot bus, package manifest, daemon transport, and companion-sidecar
persistence are implemented as described in `GameBarCompanion.md`.

## Recorded automated verification

The following is a historical evidence snapshot recorded on **2026-08-24**. It
was not rerun by later documentation edits:

- MSVC and MinGW Release suites: 10/10 at the time of recording.
- Unsigned `layerc_gamebar_msix` produced successfully.
- V2.2, V2.3, and latency gates passed.
- Recorded daemon metrics: 2.16 MiB private bytes, 11.5 MiB working set, and
  11.33 microseconds average evaluator time.
- CI used the `ci` preset with sccache and UNITY builds.

These values remain evidence for that run, not a substitute for current CI.

## Mandatory pre-release widget gate

Windows CI (`windows-msvc` on GitHub-hosted `windows-2022`) does not
require the UWP C++/XAML workload; `layerc_gamebar_host` is conditionally
registered only when the UWP toolset, Windows SDK C++/WinRT headers, and
MSBuild are installed, so a green CI run does not prove the widget host links. Before each Game Bar
package release, on an MSVC machine with that workload and restored widget
NuGet packages, run:

```powershell
cmake -S . -B build -DLAYERC_BUILD_TESTS=ON
cmake --build build --config Debug --target layerc_gamebar_host
```

Both commands must succeed, and the second must link `layerc-gamebar.exe`;
absence of the target fails the gate rather than counting as a skip. Record the
commit and the successful link with the package evidence.

## Local visual resource investigation — 2026-11-03

**The visual resource gate still fails. No threshold was relaxed.** Four cold
`v11-pixel` direct-runtime runs used the existing 8-second warm-up and 20-second
measurement, in the predeclared order balanced, low, balanced, low. Every
outcome is retained; these are not selected passing samples.

The tested base commit was `815b12d5439dbc71828a1115ec1ecee35eb0e35d`, with
local updater/packaging-tool changes and unmodified capture/runtime sources.
The measured, immutable `layerd.exe` SHA-256 was
`CC92F1190ED25682C77706BB0F01A785459E76B58FAB9CEFE1454229A27A1C8E`.
The host reported Windows `10.0.26300.0`, 16 logical processors, AMD driver
`32.0.32015.7029`, and NVIDIA driver `32.0.16.1742`.

| Run | Quality | Peak private MiB | Peak working-set MiB | Average CPU % | Gate |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | balanced | 52.336 | 51.273 | 0.052 | fail |
| 2 | low | 51.844 | 46.660 | 0.005 | fail |
| 3 | balanced | 53.680 | 47.402 | 0.052 | fail |
| 4 | low | 52.832 | 46.727 | 0.019 | fail |

The unchanged limits are 35.0 MiB private, 50.0 MiB working set, and 3.0%
normalized average CPU. Active daemon capture was verified in every sample;
the separate capture probe reported WGC on the NVIDIA RTX 4050 Laptop GPU.
That probe does not independently prove the daemon's adapter. All four samples
completed on their first measurement attempt, the executable hashes remained
unchanged, and the owned daemon/fixture processes were absent after cleanup.

Local raw evidence is under
`out/release-readiness/resource-20261103T152334Z/`: `identity.json`, all four
numbered sample JSONs, `outcomes.json`, `cleanup.json`, and
`gpu-path-comparison.json`. The checked, frozen interpretation is
`out/release-readiness/resource-findings.json`. The original and copied
capture probes were byte-identical and both selected NVIDIA; no explicit
current-user GPU preference existed for the six original/copied test
executable paths. No GPU preference was changed.

The historical 35.3–35.8 MiB finding is substantiated by
`out/benchmarks/v11_pixel-balanced-20260825-011104.json` (35.797 MiB) and
`out/benchmarks/v11_pixel-low-20260825-011142.json` (35.312 MiB). Later local
records, `v11_pixel-balanced-20260926-005641.json` and
`v11_pixel-low-20260926-005600.json` in that same directory, recorded
26.340/25.129 MiB and passed at the time. These historical JSONs lack a commit,
build identity, and package version; they are not current-revision passes.
They also used an AMD capture probe, Windows `10.0.26200.0`, and older drivers,
so the measurements are not a controlled source-regression comparison.

Source inspection found default-adapter D3D creation, one WGC pool buffer,
bounded low/balanced readback, and owned capture teardown. It did **not**
attribute the excess private commit to a particular allocation or driver.
A controlled same-build/OS/driver comparison with the actual daemon adapter
recorded, followed by idle/device/session allocation attribution, is still
needed. No speculative adapter-policy, quality, or pooling change is justified
by this evidence. Integrated-GPU, isolated-worker, template, and fullscreen
qualification remain separate; this investigation does not pass them.

## Local signing and interactive widget checks — 2026-11-04

These are **local advisory observations, not release qualification**. Base HEAD
was `815b12d5439dbc71828a1115ec1ecee35eb0e35d`, with dirty local tooling
identity `1A8DAB4F928A5619900C131A6ACBB059AAC7991BC40E91B94B3AF96697636540`
recorded in
`out/release-readiness/gamebar-cert-round4/providerfix-signed-build-source-identity.json`.
Later documentation edits do not relabel that tested source identity.

The real Debug UWP host linked successfully with zero warnings/errors
(SHA-256 `5C342484BB70AAC0266726185104480636EA3CA0F5D65C62B1259388A29140BC`).
The Release host SHA-256 was
`3CA4F0875D5A2CE4EE30E7B12DB9C767D4363FD59644F70674F796BB8A6DFB5D`.
The opt-in certificate-signed CMake target and SignTool `/pa /all` verification
passed during the approved disposable-trust window. The signed MSIX SHA-256 was
`18BFAA9D1EA07AD23A834DD0B1A622AE6C365387CAFA7D661D77BBD9AD886107`.
Before the final explicit PowerShell Security-provider import, both Windows
PowerShell 5.1 and PowerShell 7 passed the full 17-case signing suite. After
that import, both shells passed the certificate-independent and trusted-signer
cases with inherited module paths unchanged; the five optional negative-certificate
cases were **NOT RUN** again because their test certificates had been removed.
These are separate coverage records, not a full 17-case rerun at the final
source identity. This is not a production signature. Test trust was removed
afterwards; the retained test package is not promised to remain trusted or
installable.

Actual signed-MSIX installation failed with `0x800B0109`: Authenticode
verification did not establish AppX deployment trust. No machine-wide trust or
elevation was added. With explicit approval, the already-enabled Developer Mode
layout was registered instead. **Development registration is not successful
signed-MSIX installation.** See the provisioning and verification boundaries in
[`GAMEBAR-SIDELOAD.md`](../packaging/windows/GAMEBAR-SIDELOAD.md).

| Case | Observed evidence and remaining boundary |
| --- | --- |
| Eight independent timers | The renewed isolated fixture fired all eight bindings; all eight real widgets showed distinct remaining 19/24/29/34/40/45/50/56 seconds and bars, then all eight returned Ready. This is the controlled legacy fixture, not every profile/row/game type. |
| All-widget stale hiding/recovery | Bounded owned-producer suspension produced all-eight row hiding and all-eight Ready after resume. Conservative capture timing bounds the transition at 1.2264264–4.5377768 seconds after suspension. A later private instrumented build logged the actual deadline callback; see the 2026-11-06/07 platform probes below. Neither establishes reconnect latency or a production-binary bound. |
| Preferences and host restart | The initial session durably saved mappings/opacity and observed pin 1, opacity and all eight bindings/geometry after its approved Game Bar restart. Stored sidecar pin/click-through bits alone are not live shell/input proof. |
| Explorer obstruction and renewed recovery | The original Explorer-only recovery remained unverified after capture failed. The user then explicitly approved bounded recovery; an additional Game Bar restart plus capture-session reconnection restored an observable surface. This is recovered-after-Game-Bar-restart evidence, not a retroactive Explorer-only pass or a LayerC root-cause finding. |
| Live shell controls | Panel 1 compact rendering showed its correct title/row/value/bar; actual Compact Mode On was observed and restored Off. With the same pinned Panel 1/owned-caption geometry, click-through enabled allowed a 60×40 pixel caption drag; restored off blocked the identical gesture 0×0. Only this widget/input witness was tested, not controller/game input or all-eight compact navigation. The user accepted the OS-reflowed Performance-widget position; no global layout reset was attempted. |
| DPI | Windows Settings 100%→125%→100% was exercised live; all eight rows stayed legible and original 100% was restored. One observable primary 1920×1080 display is not a mixed-monitor test, and capture metadata `scale_factor=1` is not OS scaling proof. |
| API presentation | D3D11, D3D12 and OpenGL each ran windowed, borderless and requested-fullscreen local clear/present smoke fixtures, with all eight widgets correctly rendered. Windowed canvases were beside widgets; other modes covered the canvas beneath them. DXGI `GetFullscreenState` returning true is not physical/DWM-bypass qualification; OpenGL recorded CDS_FULLSCREEN success only, not ownership-query proof. Render iterations are not FPS. No real-game automation was authorized. |
| Unavailable input/API prerequisites | The user explicitly had no physical-controller interaction available. No configured Vulkan development SDK was found in the checked environment/known location; only installed SDKs were authorized, not dependencies/drivers/SDK installation. These cases are not passes. |
| Separate resource accounting | Earlier whole-process samples remain historical. A later controlled A1-off/B1-on/B2-on/A2-off study checked all 124 raw samples and separately recorded native bridge/widget and all three stable Xbox host processes' CPU/private/working-set metrics in the same windows, plus per-PID/adapter/engine GPU data. See the controlled-accounting checkpoint below. Native had no matching PDH instance, **not zero**. This is not the failing visual-capture workload or a 35 MiB gate fix. |
| Signed package recovery | Signed-MSIX installation remained blocked by `0x800B0109`. Approved development registration, removal and renewed registration were observed instead; no successful signed reinstall, production trust, or update-data migration is claimed. |

The initial-session observations remain historical in
`out/release-readiness/gamebar-runtime-92f8b2e7/runtime-findings.json`; its
`currentStatusFile` points to the separately recorded audit follow-up,
`out/release-readiness/gamebar-rework-20261104/runtime-findings.audit-fix.json`.
That directory retains source/binary identities, raw and checked resource data,
case reports and byte-preserved screenshots. Original deployment trust and
visual-resource failures are unchanged; the follow-up is local advisory
coverage, not production or physical release qualification.

Follow-up restoration completed on 2026-11-05: the test registration and owned
registry were removed, nine original package files were independently SHA-256
verified, and the temporary command allowlist was restored byte-for-byte with
its private backup removed. A newly discovered daemon outside the old runtime
journal was stopped only after separate user approval of its exact identity;
an unrelated existing fixture was preserved. The user explicitly accepted the
Performance-widget reflow. Receipts are `independent-restoration-check.json`,
`approved-extra-process-cleanup.json`, `package-cleanup-state.json` and
`temporary-powershell-allowlist.json` in the follow-up directory. This is
file-content restoration, not original orphan registration/ACL rollback. No
certificate/trust mutation was made in the follow-up; earlier disposable-key
cleanup evidence remains separately scoped in
`out/release-readiness/gamebar-cert-round4/final-certificate-cleanup.json`.
Production keys, physical Linux testing, hosted cross-platform CI and Store
certification remain separate or out of scope as already documented.

### 2026-11-05 controlled bridge/widget accounting

The renewed study uses the same private native test binary, original
uninstrumented widget binary, profile, native-overlay policy and foreground
fixture in four cohorts: bridge off/on/on/off. Only the companion-demand flag
changes. Each cohort has ten seconds' warm-up and 31 samples (30 CPU intervals);
all 124 raw samples were independently checked. Frozen inputs and per-process,
per-adapter and per-engine GPU data are retained in
`out/release-readiness/gamebar-instrumented-20261105/paired-resource-analysis-attempt2.json`.
The three stable Xbox platform processes have their own CPU, private-commit and
working-set observations in those same windows; shared OS brokers are excluded.
GPU counters are not added across engines/adapters, and missing native instances
remain unavailable rather than zero. CPU below is percentage points of one
logical CPU; machine-normalized values use the recorded 16 logical CPUs.

| Enabled minus disabled pair | Native private MiB | Native working-set MiB | Native CPU pp | Widget private MiB | Widget working-set MiB | Widget CPU pp |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| B1 minus A1 | 1.125882 | 9.845388 | 0.260424 | 2.974168 | 9.037676 | 2.708368 |
| B2 minus A2 | 1.100932 | 9.918599 | 0.155826 | 1.077243 | 1.214088 | 2.447203 |

These are descriptive paired observations, not statistical significance or
universal allocation attribution. Retained widget caches/order effects remain
visible. Neither this private native test seam nor this cooldown-only workload
resolves the unchanged historical visual-resource failure.

### 2026-11-06/07 instrumented platform probes

After the user restarted Pi with the approved AppLocalFolder read root, the
probes ran against private instrumented builds of the real UWP widget. Shipping
sources were unchanged. Native test binary SHA-256 `42B8035D…F8A0A6`; the
expiry/fault widget `E93C98FC…7408D29`; the retained-view widget `DCA315B1…EC3ABB`.
Each case is one actual sample bound to PID, process creation time, package
family and hashes. Logged intervals include instrumentation; none is a
production-binary latency, an OS scheduling guarantee, or a two-second upper
bound.

| Case | Observed result | Boundary |
| --- | --- | --- |
| Held-producer expiry | Owned producer suspended, then resumed. Deadline two seconds; callback logged 13.5344 ms after it, expiry 14.2223 ms. All eight empty-data renders completed 17.6067–41.3170 ms after the deadline; post-to-entry ≤4.6621 ms; render call ≤2.1188 ms. Inspected pixels: all eight hidden while held, fresh after recovery. | One sample. |
| Subscriber fault | A deliberately injected exception in one subscriber at revision 2302; at the later accepted revision 2303 all eight actual CoreDispatcher post/entry/render/completion chains completed. | Test-seam exception, not an OS enqueue failure. |
| OS close/reopen | All 1,808 old-page posts examined: none pending at revocation and no old-page render afterwards. A distinct reopened page rendered. | Windows retired the old dispatcher before the deferred post, so no callback entered after OS close. Not claimed. |
| Retained-view revocation | Actual Normal-priority CoreDispatcher work was posted 3.1453 ms before the page's real subscription cleanup. It entered 1.9632 ms after revocation with the view still alive, completed, and did not render. A later real close/reopen rendered a distinct page. | View deliberately retained; not a natural OS-close race. |

Final status, frozen traces, analyses and personally inspected screenshots:
`out/release-readiness/gamebar-instrumented-20261105/instrumented-final-status.json`
(supersedes the earlier `instrumented-followup-status.json` checkpoint).

Restoration completed on 2026-11-08: the development test registration and
owned registry subtree were removed, no LayerC test process remained, nine
original package files were restored and independently SHA-256 verified, and
the temporary lean-ctx read root and command allowlist entry were restored
byte-for-byte with private backups removed. Receipts:
`package-cleanup-state.json`, `independent-restoration-check.json`,
`temporary-read-root-restored.json` and `temporary-powershell-allowlist.json`.
This is file-content restoration, not original orphan registration/ACL
rollback. Original signed-install trust and visual-resource failures remain
unchanged.

## Outstanding manual and certification evidence

Instrumented callback/deadline timing and dispatcher fault isolation were
observed once each in private builds (above). Production-binary timing and a
callback entering after a natural OS close remain unobserved. Remaining
restart/update-migration and physical configurations retain
their distinct authority, trust and hardware prerequisites. The dated local
observations qualify only their stated cases. Controlled core interleavings and
clear/present fixtures do not substitute for platform timing, real-game
compatibility, controller or physical-hardware qualification.

A production Game Bar release remains incomplete until all of the following are
recorded:

1. **Reconnection matrix:** App Service recovery across LayerD, Game Bar,
   Explorer, and package restarts.
2. **Eight-slot concurrency:** all panel slots active concurrently with
   independent cooldowns.
3. **Settings persistence:** opacity, pinning, click-through, and slot mappings
   survive companion-sidecar and process restarts.
4. **Stale hiding:** every widget clears to disconnected after more than two
   seconds without a valid snapshot.
5. **Shell controls:** controller navigation, pinning, click-through, compact
   mode, transparency, and per-monitor DPI behavior.
6. **Resource accounting:** LayerC bridge/widget CPU, working set, and GPU memory
   measured separately from the Game Bar host.
7. **Fullscreen matrix:** exclusive-fullscreen and borderless-windowed behavior
   documented per graphics API.
8. **V1.1 visual resource gate:** resolve or formally disposition the recorded
   35.3–35.8 MiB private-byte result against the 35.0 MiB gate.
9. **Production signing/certification:** sign the final MSIX with the externally
   supplied certificate and record the applicable store/certification result.

## Completion boundary

Mark this record released only when the manual matrix, resource evidence, and
signing/certification artifacts are linked here or from an approved release
record. Compilation, an unsigned MSIX, and automated companion tests establish
narrower evidence only.
