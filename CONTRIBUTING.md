# Contributing to LayerC

Thanks for helping with LayerC. This page routes contributors to the source that
owns each decision instead of duplicating the project contract.

## Choose the relevant reference

| Change | Read |
| --- | --- |
| Setup, programs, supported surfaces, profile format, test targets | [`README.md`](README.md) |
| Linux adapters, CLI, packaging, or physical evidence | [`docs/Linux.md`](docs/Linux.md) |
| Game Bar transport, widgets, or MSIX | [`docs/GameBarCompanion.md`](docs/GameBarCompanion.md) |
| Exact build graph or CI behavior | current `CMakeLists.txt` or affected workflow |

Open only the references your change needs. Tracked source, tests, CMake, and
workflows resolve stale documentation.

## Work on a change

Preserve unrelated worktree changes and keep generated `build/`, `out/`,
`review/`, `.review_tmp/`, and `.codewhale/` content out of commits. Use
disposable profiles under `examples/` for mutation-heavy local checks.

The non-negotiable project boundaries apply to every
change: passive input, no anti-cheat bypass, no telemetry/network beyond the
same-package Game Bar service and the launch-time `layerc.exe` updater, monotonic timing, demand-driven services, and
durable profile compatibility.

Run the smallest configured check that exercises the behavior you changed and
rerun failures caused by the change. CI owns the full cross-platform gate; use a
full local build and every suite only for work with that breadth. Resolve current
commands and targets from [`README.md`](README.md), CMake, and the affected
test registration.

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

The launcher's self-update preference defaults to OFF because production
signing is not provisioned. The empty-public-key guard is separate: opting in
cannot make update requests from a build without a valid key. User opt-in/out
and `--offline` are documented in
[README.md](README.md#windows-launcher-and-updates-v26b).

`tools/update_manifest.cpp` (`layerc_manifest`) emits **unsigned**, byte-stable
JSON. The launcher verifies a detached ECDSA P-256 / SHA-256 signature over
those exact bytes, in OpenSSL's DER format, before parsing. The public key in
`cmake/release_key.hpp.in` comes from the CMake cache variable
`LAYERC_UPDATE_PUBLIC_KEY`; its default is the contents of
`packaging/windows/update-signing-key.hex`, or empty if that file is absent.
Neither a production key file nor a release-signing secret is currently
configured. `.github/workflows/release.yml` only publishes the Windows ZIP;
it does not generate, sign, or upload update manifests or hash-named assets.
The portable ZIP starts LayerC via `layerd.exe`; CMake installs `layerc.exe`
into it only once `LAYERC_UPDATE_PUBLIC_KEY` is non-empty. A keyless launcher
cannot bootstrap a first build, so it is omitted from the ZIP while the
launcher and its tests still build unconditionally. Do not attach an unsigned
manifest to that workflow.

Before enabling release updates, the release owner must:

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
   **`LAYERC_UPDATE_SIGNING_KEY`**, reserved for a future signing job. It is not
   used by the current workflow. For example, supply it on standard input
   without echoing it or placing its value in command arguments:

   ```powershell
   Get-Content -LiteralPath "$keyDir\update-signing.pem" -Raw |
     gh secret set LAYERC_UPDATE_SIGNING_KEY -R EnderKeera/LayerC
   ```

4. Install the release payload, generate its manifest, then sign the exact
   output bytes. Use a strictly increasing sequence for each publication and
   the payload's full commit SHA; the channel must match
   `LAYERC_UPDATE_CHANNEL` (default `rolling`):

   ```powershell
   cmake --install build/vs2026 --config Release --prefix out/layerc
   $sequence = 1 # Example only: choose a value greater than every prior publication.
   $commit = git rev-parse HEAD
   .\build\vs2026\Release\layerc_manifest.exe --root .\out\layerc --channel rolling --sequence $sequence --commit $commit --output .\out\manifest.json
   openssl dgst -sha256 -sign "$keyDir\update-signing.pem" -out .\out\manifest.sig .\out\manifest.json
   ```

   Verify the signature with the matching public key before publishing. Do
   not reformat JSON, change newlines, or edit the manifest after signing.
   Upload `manifest.json`, detached DER `manifest.sig`, and each payload file
   as an asset named by its lowercase SHA-256 from the manifest (deduplicated
   by hash) to **EnderKeera/LayerC** at the tag named by `LAYERC_UPDATE_CHANNEL`
   (default `rolling`, also embedded as `release_tag`).
   A future job must retrieve the secret without logging it and remove its
   temporary private-key file after signing. Publishing only a ZIP is not
   an update feed; retain the OFF default until this signed feed and embedded
   key are provisioned and verified. Users must still explicitly opt in.

## Reporting issues

Use the bug or feature template. Include only relevant environment details,
minimal reproduction steps, expected and actual behavior, and useful diagnostics.
Sanitize profiles, bundles, traces, and screenshots before attaching them.
