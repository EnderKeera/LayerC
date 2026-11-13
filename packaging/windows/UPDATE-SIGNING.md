# Offline Windows update signing

This document covers local preparation and verification of the Windows launcher's
signed update assets. `tools/prepare_update_release.py` is an offline helper: it
uses the existing `layerc_manifest` serializer, checks that an explicit private
key matches an explicit pinned public key, signs the exact manifest bytes, and
atomically writes a content-addressed output directory. It never uploads or
publishes assets.

**Launcher updates stay OFF by default; users opt in.** This helper creates no
key, does not change launcher behavior, and does not publish an update feed.
The production feed is signed and published only by `release.yml` for `v*.*.*`
tags ([CONTRIBUTING.md](../../CONTRIBUTING.md#release-signing-windows-updates)).
Do not infer production signing from a local disposable-key test.

## Prerequisites and local invocation

Use a Windows build with the launcher targets configured. The offline CNG
verifier delegates signature verification to the same `BcryptSignatureVerifier`
used by the launcher; it does not install, activate, access the registry, or
contact a service.

```powershell
cmake --preset vs2026
cmake --build build/vs2026 --config Release --target layerc_manifest layerc_verify_update -j 4

$python = 'C:\Program Files\Python313\python.exe'
$openssl = 'C:\Program Files\Git\mingw64\bin\openssl.exe'
$manifestTool = 'build\vs2026\Release\layerc_manifest.exe'
$verifier = 'build\vs2026\Release\layerc_verify_update.exe'
$payload = 'out\layerc'
$publicKey = 'C:\release-material\update-signing-key.hex'
$privateKey = 'C:\release-material\update-signing-private.pem'
$channel = 'rolling'
$sequence = 1 # Replace with a value greater than every prior publication.
$commit = '<full-lowercase-40-hex-source-commit>'
$output = "D:\release-staging\layerc-$channel-$sequence" # Parent must exist; output must not.

& $python tools\prepare_update_release.py `
    --root $payload `
    --manifest-tool $manifestTool `
    --private-key $privateKey `
    --public-key $publicKey `
    --output $output `
    --channel $channel `
    --sequence $sequence `
    --commit $commit `
    --openssl $openssl
if ($LASTEXITCODE -ne 0) { throw 'Offline release preparation failed' }

& $verifier $publicKey (Join-Path $output 'manifest.json') (Join-Path $output 'manifest.sig')
if ($LASTEXITCODE -ne 0) { throw 'Native CNG signature verification failed' }
```

The channel must match the launcher configuration and the commit must identify
the payload. Supply all paths explicitly. The payload root must exist; the
output parent must already exist; the final output path must not exist and must
be outside the payload. The private-key path must resolve outside the payload.
The helper stages beside the final output, verifies the result, then publishes
by same-filesystem rename. On every failure it removes its scratch and staging
directories and does not create the final output.

The resulting directory contains only `manifest.json`, raw DER
`manifest.sig`, and one file per distinct lowercase SHA-256 digest referenced
by the manifest. Equal payload files share one digest asset. The helper copies
payload bytes in bounded chunks and checks their size and hash against the
manifest before signing. If a payload changes after native manifest generation,
preparation fails instead of signing assets that do not match it.

## Key format and handling

The public-key text file must contain the launcher-compatible 130 hex
characters: `04` followed by the uncompressed P-256 X and Y coordinates.
Surrounding ASCII whitespace is accepted. Before signing, the helper derives
SubjectPublicKeyInfo DER from the supplied private key and requires the exact
P-256 SPKI header
`3059301306072a8648ce3d020106082a8648ce3d030107034200` followed by the
matching 65-byte uncompressed point. A mismatch is a sanitized error; there is
no generated-key fallback.

A release owner must provision the public key and private-key file explicitly.
Keep private material outside the repository and payload in an access-controlled
location, do not stage, commit, copy into the output, or log it, and follow the
release owner's approved backup and retention procedures. The helper passes an
empty passphrase to OpenSSL noninteractively; encrypted keys therefore fail
rather than prompting. Do not weaken that behavior or put passphrases in
commands or logs. The helper removes its temporary derived public files after
use but never deletes or changes the supplied private key.

## Disposable-key tests and evidence boundary

After building `layerc_manifest` and `layerc_verify_update`, run the focused
offline suite with the installed tools:

```powershell
& 'C:\Program Files\Python313\python.exe' tests\update_release_tests.py `
    --manifest-tool build\vs2026\Release\layerc_manifest.exe `
    --verifier build\vs2026\Release\layerc_verify_update.exe `
    --openssl 'C:\Program Files\Git\mingw64\bin\openssl.exe'
```

The test suite generates two independent disposable P-256 keys in a temporary
directory, checks successful preparation and CNG verification, and exercises
manifest/signature tampering, wrong and mismatched keys, missing keys, unsafe
paths, existing output preservation, payload mutation, and symlink rejection
where symlink creation is available. It does not use or provision production
key material.

A passing local test establishes only that the local helper and native CNG
verifier interoperate for disposable keys and test payloads. It is not evidence
that a production release was signed, the release assets were published, or the
update feed was tested. The `sign-update` job runs this same suite before it
signs with the production key; publication happens only in `publish-update`.
Do not upload assets prepared locally to the `rolling` release.
