# Contributing to LayerC

Thanks for helping with LayerC. This page routes contributors to the source that
owns each decision and describes the CI and release gates.

## Choose the relevant reference

| Change | Read |
| --- | --- |
| Setup, programs, supported surfaces, profile format, test targets | [`README.md`](README.md) |
| Architecture, invariants, code conventions | [Project boundaries](#project-boundaries) |
| Editor UI on either platform (binding design rules) | [`docs/DesignSystem.md`](docs/DesignSystem.md) |
| Linux adapters, CLI, packaging, or physical evidence | [`docs/Linux.md`](docs/Linux.md) |
| Desktop/hardware evidence | [`docs/DesktopEvidence.md`](docs/DesktopEvidence.md) |
| Game Bar transport, widgets, or MSIX | [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) |
| Exact build graph or CI behavior | current `CMakeLists.txt` or affected workflow |

Open only the references your change needs. Tracked source, tests, CMake, and
workflows resolve stale documentation.

## Work on a change

Preserve unrelated worktree changes and keep generated `build/`, `build-*/`,
`out/`, `review/`, `.review_tmp/`, `.codewhale/`, `.mimosa/`, and `.codegraph/`
content out of commits. Use disposable profiles under `examples/` for
mutation-heavy local checks.

The non-negotiable [project boundaries](#project-boundaries) apply to every
change: passive input, no anti-cheat bypass, no telemetry/network beyond the
same-package Game Bar service and the launch-time `layerc.exe` updater,
monotonic timing, demand-driven services, and durable profile compatibility.

Run the smallest configured check that exercises the behavior you changed and
rerun failures caused by the change. CI owns the full cross-platform gate; use a
full local build and every suite only for work with that breadth. Resolve current
commands and targets from [`README.md`](README.md), CMake, and the affected
test registration.

### Editor UI changes

[`docs/DesignSystem.md`](docs/DesignSystem.md) is binding for Windows and Linux
editor chrome. Read the affected component rules before implementation. Reuse
the owning theme tokens, native fonts and shared geometry; keep System and
contrast themes, keyboard navigation, screen-reader semantics and role overrides
working. An overlay preview must remain faithful to its profile.

Run `python tools/tests/test_design_system.py` and
`python tools/check_design_system.py` before submitting, plus focused native
tests. The checker compares token tables/JSON/source, rejects local colors,
rounded chrome and literal control heights, and checks resource/menu wording.
It does not prove layout, reading order or accessibility: attach relevant
rendered screenshots and describe keyboard/accessibility checks, including
unobserved platform or hardware gaps. Follow the PR checklist. Propose and agree
a specification change before deviating; update its implementation, tokens,
documentation and tests together.

## Project boundaries

### Product

- Input is passive: never block, modify, or synthesize it. XTest is test-only and
  must not link into `layerd`. The only exceptions are the two Windows editor
  modes named in the README guarantees, Pick window and Direct drag. Keep them
  user-started and bounded: cancel, capture loss, turning the mode off, or the
  end of the edit ends them. Do not add another mode that takes input.
- No injection, hooks, game-memory reads, drivers, administrator elevation, or
  anti-cheat bypass.
- No telemetry or network access, with two exceptions: the same-package Game
  Bar App Service `LayerC.GameBar.v1`, and the Windows launcher `layerc.exe`,
  which may make anonymous HTTPS GETs to this repository's GitHub release at
  launch to update itself and the app, can be turned off by the user, and
  exits after starting `layerd`. `layerd`, `layere`, `layercal`, and `layerw`
  never access the network or link an HTTP client.
- Cooldowns, retries, deadlines, and timers use monotonic clocks.
- Optional services are demand-driven, idle-blocking, owned, and joined.
  Unavailable capabilities retain visible fault reasons without weakening
  unrelated features.
- Preserve profile compatibility and unsupported bindings. Move profiles between
  Windows and Linux through validated JSON or `.layercprofile` bundles, never by
  copying a platform store.
- Gamescope and Wayland are unsupported in V2.6; do not imply otherwise. The
  V2.7 Wayland backend is in development under
  [`docs/Wayland.md`](docs/Wayland.md); it makes no support claim until it
  lands. Its only global Wayland input source is passive evdev, opt-in and
  dependent on the user's own `input` group membership; uinput is test-only.
  Gamescope stays unsupported.

### Architecture

- Keep shared-core interfaces free of Win32/X11 handles and platform headers.
- Reuse core JSON, text, UTF-8, hashing, and validation. Emit byte-stable JSON
  through `json::append_*`; do not add platform-local serializers or duplicate
  normalization.
- Profile writes use the existing durable `stage -> commit` transaction,
  one-writer ownership, backup/journal markers, and crash recovery.
- Derive automation limits and schemas from the current parser and
  `src/core/include/layerc/core/automation.hpp`. Validate the complete winning
  effect set before committing external effects; persistent state remains under
  transactional `_state` storage.
- Shared mutable data is mutex-protected with value-copy boundaries. The daemon
  owns one platform event/message loop.

On Windows, overlays stay non-activating and click-through; only the user's
Direct drag lets LayerD's visible panels take clicks. Visual capture uses
WGC with Desktop Duplication fallback and explicit unavailable/fault
diagnostics. The isolated worker remains authenticated, Job-bounded,
kill-on-close, and demand-launched. Game Bar-specific limits and authority live
in [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md).

On Linux, base profiles live in XDG config, sidecars/state in XDG data, and
authenticated control sockets in `XDG_RUNTIME_DIR` or the private documented
fallback. One X11 connection is shared by adapters and integrated into the
blocking event loop. XI2 input is read-only; Shape overlays stay hidden unless
an empty input region is verified. Composite/Damage capture uses the bounded
documented fallback. Headless operation retains store/control behavior while
reporting graphical capabilities unavailable. See [`docs/Linux.md`](docs/Linux.md)
for exact contracts.

### Code conventions

- Use C++20 and keep MSVC/GCC/MinGW builds warning-free with the flags configured
  by CMake.
- Keep translation units focused and around 1000 lines or less.
- Prefer `optional`/`variant`, typed `*Error` values, and `[[nodiscard]]` for
  fallible returns.
- Use the existing `layerc::core`, `platform`, `capture`, `runtime`,
  `daemon`, `editor`, and `gamebar` namespaces as appropriate.
- Keep automated, interactive-hardware, physical Linux, signing, and
  certification evidence distinct; do not promote unobserved or skipped
  coverage.

## CI and release gates

CI, release, and evidence-comment automation use GitHub-hosted runners:
`windows-2022` for Windows builds, `ubuntu-24.04` for Linux build and Flatpak
jobs, and `ubuntu-latest` for workflow lint, publishing, and automation.
GitHub-hosted jobs normally have a 10-minute timeout. The Windows CI and
release build jobs allow 30 minutes; `windows-analyze` and `linux-flatpak`
allow 60 minutes for cold builds, SDK setup, and analysis recovery retries.
Pull requests, including forks, receive both Windows and Linux CI checks. There
are no self-hosted runners; desktop/hardware checks arrive as advisory pull
request evidence.

[`ci.yml`](.github/workflows/ci.yml) runs on pushes to `main`, pull requests,
and manual dispatch; a newer push to a pull request cancels its older run.

| Job | Gate |
| --- | --- |
| `workflow-lint` | `actionlint` over every workflow; design-system checker and planted-violation tests (`tools/check_design_system.py`, `tools/tests/test_design_system.py`) |
| `windows-msvc` | [`build-package`](.github/actions/build-package/action.yml): `ci` preset (Ninja, sccache, unity), CTest with registration/JUnit policy, `benchmark_v22`, `benchmark_v23`, install, ZIP, artifacts |
| `windows-analyze` | Non-unity Release build with MSVC `/analyze` for every LayerC target (compile-time only) |
| `linux` | [`tools/ci-linux.sh`](tools/ci-linux.sh) in digest-pinned `debian:stable` and `ubuntu:24.04`: headless and required Xvfb suites, installed-binary smoke with private XDG roots, commit-bound evidence. `debian:stable` also requires the `editor` gate, installed-editor smoke and the `.deb` build/install/remove (`--editor required --package required`); `ubuntu:24.04` also requires the `editor` gate and installed-editor smoke with the pinned FLTK 1.4.5 (Ubuntu ships FLTK 1.3), without the `.deb` (`--editor required --package off`). Both require `--wayland required`: headless sway, private-bus and GJS fixtures, not physical compositor qualification |
| `linux-flatpak` | 25.08 manifest (bundled FLTK 1.4.5) with required daemon/control/X11 and editor smoke under Xvfb (`--flatpak-only`) |
| `publish-rolling` | `main` pushes only; needs all five jobs above and publishes the `rolling` prerelease |

Every CTest gate (`windows-msvc` and the Linux
`headless`, `x11`, `editor`, `wayland`, `dbus` and `gjs` stages) is validated by
[`tools/linux_test_report.py`](tools/linux_test_report.py) against the suites
that [`tools/test-gates.json`](tools/test-gates.json) names for it, not against
whatever happened to be registered. A gate fails on empty runs, a required
suite that is unregistered, unlabelled, or did not run, a labelled suite the
policy does not list, and skips the policy does not name. Adding or removing a
suite therefore updates the policy in the same change. On Windows,
`layerc_overlay_tests` may skip when no isolated desktop is available; report
that as unavailable interactive coverage, not a pass. Linux `auto` backend
gates allow only the harness skips named in the policy and report unavailable
coverage. `--wayland required` promotes every optional backend suite and the
Wayland protocol/connection fixtures in the headless/X11 gates to required,
and forbids their skips (not the unrelated PowerShell tooling). Optional Linux
suites also include the PowerShell benchmark-result test and dependency-dependent Wayland/PipeWire
fixtures; `tools/test-gates.json` is authoritative.
[`release.yml`](.github/workflows/release.yml) builds `v*.*.*` tags with the
same setup and `build-package` actions, so benchmark gating cannot drift.
`main` puts only the Windows x64 ZIP on `rolling`; tags also add the signed
launcher update feed there
([Release signing](#release-signing-windows-updates)). A tag publishes a non-prerelease
release with the Windows ZIP, the Debian 13 `.deb` that its own
`debian:stable` gate (`--editor required --package required --wayland
required`, the same image digest as `ci.yml`) built, installed and removed,
the `layerc-<version>-x86_64.flatpak` bundle that its required Flatpak smoke
installed, and `SHA256SUMS`. Its `version` job refuses a tag that differs from
`project(LayerC VERSION ...)`, from either AppStream `<release>` entry, or from
the `LayerC <version>` line of
[`packaging/release-notes.md`](packaging/release-notes.md), which is the
release body. Bump the version, both metainfo release entries, the release
notes and the README/`docs/Linux.md` status text in one change before tagging,
and tag only a commit whose `ci.yml` run is green.

[`tools/publish-rolling.sh`](tools/publish-rolling.sh) promotes `rolling` in
one serialized job (`concurrency: publish-rolling`). It skips a commit older
than the one already published, uploads the package under a non-ZIP staging
name and verifies the served bytes, swaps it in as the only
`layerc-windows-x64.zip`, moves the `rolling` tag explicitly (`gh release edit
--target` never moves an existing tag), and finally writes notes naming
`main@<commit>` and the ZIP's SHA-256. Those notes are the commit point:
`publish-rolling.sh --verify` and `tools/pull-ci.ps1` accept `rolling` only when
the tag, the noted commit, and the ZIP's SHA-256 agree, so an interrupted
publication is refused rather than half-used, and rerunning the job converges.
`tools/tests/test_publish_rolling.py` injects a failure at every mutation
against a fake `gh`.

Artifacts: `layerc-windows-x64` is the full bundle (install layout, benchmark
JSON, ZIP); `layerc-windows-x64-tools` feeds
`tools/submit-desktop-evidence.ps1` and `tools/push-and-test.ps1`; only
`publish-rolling` consumes
`layerc-rolling-package`. The build writes `layerc-build-identity.json`
(commit, run, attempt, configuration) next to its binaries and ships it in
`layerc-windows-x64-tools`. Benchmark results take `commitSha` and `build` from
that file, never from the workflow event. `push-and-test.ps1` refuses to
measure unless it and the checkout match the resolved candidate.
`submit-desktop-evidence.ps1` requires the identity to name the resolved CI
run and its built commit to be the reported commit or that commit's pull
request test merge commit (checked with the GitHub API). Results from a local
build directory are marked `local-build` with
the checkout's commit and dirty state.

Other workflows:

- [`desktop-evidence.yml`](.github/workflows/desktop-evidence.yml): validates
  new pull request comments carrying a report using the default branch's
  validator and harness configuration, then publishes an advisory,
  non-required check on the reported commit. See
  [`docs/DesktopEvidence.md`](docs/DesktopEvidence.md).

### Local hardware benchmarks and desktop evidence

Run hardware benchmarks on an interactive, signed-in desktop, not a headless
agent: visual capture needs the real screen. Keep the box quiescent: checks
fail, listing PIDs, when `layerd` or `layere` is running and never kill them.
Syncing the rolling install locally is the manual `tools/pull-ci.ps1` step.
Benchmarks run their daemons with
disposable profiles (`LAYERC_PROFILE_DIR`) and a disposable
`HKCU\Software\LayerC.Benchmark\<run>` settings key (`LAYERC_SETTINGS_KEY`),
both removed afterwards; they never read or write the user's
`HKCU\Software\LayerC`.

The V2.3 automation resource gate measures baseline, activation, and disable
within one owned daemon PID. Its disposable sidecar is absent at baseline,
installed for activation, and removed before the supported profile reload on
disable. Recovery is sampled only after observing the active event's reset on
the retained handle; a restart is not teardown evidence. Evaluator thresholds
in the result come from the measured executable, including Debug/Release limits.

To reproduce the hardware benchmarks for a CI run:

```powershell
gh run download <run-id> -n layerc-windows-x64-tools -D out\bench-stage
gh release download baseline-v22 -D out\baseline --clobber
$baselineZip = Get-ChildItem out\baseline -Filter *.zip -File
if ($baselineZip.Count -ne 1) { throw "Expected one baseline ZIP." }
Expand-Archive $baselineZip.FullName -DestinationPath out\baseline -Force

.\tools\run-hardware-benchmarks.ps1 `
  -CandidateBuildDirectory .\out\bench-stage `
  -BaselineBuildDirectory .\out\baseline
```

`tools/push-and-test.ps1` pushes the current branch, waits for CI on that
exact commit, and can then run these checks (`-WatchOnly`, `-NoPush`,
`-VisualOnly`). Linux CI is the automated compatibility gate; the physical
GNOME Xorg/KDE X11 native and Flatpak checklist in
[`docs/Linux.md`](docs/Linux.md) stays pending until evidence is archived.

Submit results with `tools/submit-desktop-evidence.ps1` (Windows) or
`tools/submit-desktop-evidence.sh` (Linux); their dry-run modes render the
comment without posting. Posted reports become maintainer-attested or
unverified advisory checks, not mandatory CI gates. See
[`docs/DesktopEvidence.md`](docs/DesktopEvidence.md).

## Pull requests

Explain what changed and why, the observable result you checked, and any
remaining manual, hardware, or platform gap. Update the owning documentation
when behavior, support, packaging, or release gates changed.

A contribution is ready when:

- the requested behavior and nearest relevant failure or recovery path are
  checked;
- the diff is focused and contains no generated output;
- compatibility or migration effects are documented; and
- user-facing and specialized documentation remains accurate.

## Release signing (Windows updates)

The launcher's self-update preference defaults to OFF; users opt in. The
empty-public-key guard is separate: opting in cannot make update requests from
a build without a valid key. User opt-in/out and `--offline` are documented in
[README.md](README.md#windows-launcher-and-updates-v26b).

`tools/update_manifest.cpp` (`layerc_manifest`) emits **unsigned**, byte-stable
JSON. The launcher verifies a detached ECDSA P-256 / SHA-256 signature over
those exact bytes, in OpenSSL's DER format, before parsing. The public key in
`cmake/release_key.hpp.in` comes from the CMake cache variable
`LAYERC_UPDATE_PUBLIC_KEY`; its default is the contents of the committed
`packaging/windows/update-signing-key.hex`, or empty if that file is absent.
The matching PEM private key is the repository Actions secret
**`LAYERC_UPDATE_SIGNING_KEY`**; the release owner keeps its only other copy
outside any checkout. Shipped launchers trust only their compiled-in key, so
replacing it strands every install built with the old one.

The portable ZIP starts LayerC via `layerd.exe`; CMake installs `layerc.exe`
into it only while `LAYERC_UPDATE_PUBLIC_KEY` is non-empty. A keyless launcher
cannot bootstrap a first build, so it is then omitted from the ZIP while the
launcher and its tests still build unconditionally.

For every `v*.*.*` tag, `release.yml` publishes the update feed that launchers
read from the release named by `LAYERC_UPDATE_CHANNEL` (`rolling`, embedded as
`release_tag`):

| Job | Update-feed role |
| --- | --- |
| `version` | Requires CMake's `LAYERC_UPDATE_CHANNEL` default to equal the workflow's `UPDATE_CHANNEL` and derives the sequence `major * 1000000 + minor * 1000 + patch` (minor and patch at most 999) |
| `build` | Uploads the tested `out/layerc` install tree that the ZIP packages, with `layerc_manifest.exe` and `layerc_verify_update.exe`, as `layerc-update-payload` |
| `sign-update` | The only job that reads the secret; builds nothing. Runs `tests/update_release_tests.py` with disposable keys, signs the payload with `tools/prepare_update_release.py`, deletes its temporary key file, and checks the result with the launcher's CNG verifier |
| `publish-update` | Runs only after the versioned release is published (`concurrency: update-feed`). Refuses a feed whose published sequence is higher, or equal for another commit; uploads only the hash-named assets `rolling` lacks, then replaces `manifest.sig` and `manifest.json` |

Launchers therefore move only between tagged releases, never to an older one,
and `main` builds never change the feed: `publish-rolling.sh` replaces only
`*.zip` and `*.staged` assets. Hash-named assets are never deleted, so a
release owner who prunes them must keep every one the current manifest names.

To provision a key, which was done once for **EnderKeera/LayerC** and is
repeated only for a fork or a deliberate rotation, the release owner must:

1. Generate a P-256 key pair in a protected directory **outside the checkout**.
   Never commit or print the private key. Example in PowerShell 7 with OpenSSL
   (create and secure `$keyDir` first):

   ```powershell
   $keyDir = 'C:\secure\LayerC-release'
   openssl ecparam -name prime256v1 -genkey -noout -out "$keyDir\update-signing.pem"
   openssl ec -in "$keyDir\update-signing.pem" -pubout -outform DER -out "$keyDir\update-signing-public.der"
   ```

2. Extract the last 65 bytes of the DER public key: the uncompressed SEC1
   point `04 || X || Y`. Commit **only** its 130 hexadecimal characters, not
   the DER wrapper or PEM private key:

   ```powershell
   $der = [IO.File]::ReadAllBytes("$keyDir\update-signing-public.der")
   $point = [byte[]]($der[($der.Length - 65)..($der.Length - 1)])
   if ($point.Length -ne 65 -or $point[0] -ne 4) { throw 'Expected an uncompressed P-256 public point.' }
   $publicKey = [Convert]::ToHexString($point).ToLowerInvariant()
   New-Item -ItemType Directory -Force .\packaging\windows | Out-Null
   [IO.File]::WriteAllText((Join-Path $PWD 'packaging\windows\update-signing-key.hex'), $publicKey)
   # Explicit -D also replaces an older cached key (including an empty one).
   cmake --preset vs2026 "-DLAYERC_UPDATE_PUBLIC_KEY=$publicKey"
   cmake --build --preset vs2026-release
   ```

   Alternatively, supply the same hex string with `-DLAYERC_UPDATE_PUBLIC_KEY`
   without committing a public-key file. A fresh CMake configure reads the
   file automatically. Keep the matching private key securely backed up;
   shipped launchers trust only their compiled-in key.

3. Store the PEM private key in a repository Actions secret named
   **`LAYERC_UPDATE_SIGNING_KEY`**, which only `sign-update` reads. Supply it
   on standard input without echoing it or placing its value in command
   arguments:

   ```powershell
   Get-Content -LiteralPath "$keyDir\update-signing.pem" -Raw |
     gh secret set LAYERC_UPDATE_SIGNING_KEY -R EnderKeera/LayerC
   ```

Publish feeds only through `release.yml`. To prepare and verify one locally
without publishing, use the offline helper in
[packaging/windows/UPDATE-SIGNING.md](packaging/windows/UPDATE-SIGNING.md).
Never upload an unsigned manifest or edit one after signing. Don't publish a
feed by hand either: a higher sequence would block every later tag.

## Reporting issues

Use the bug or feature template. Include only relevant environment details,
minimal reproduction steps, expected and actual behavior, and useful diagnostics.
Sanitize profiles, bundles, traces, and screenshots before attaching them.
