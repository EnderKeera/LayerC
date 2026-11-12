# Local Game Bar MSIX signing and sideloading

The Game Bar MSIX target remains unsigned and available without a certificate. Signing is an explicit local-development step; this documentation does not claim Store certification or a production signature. The signer helper does not create or import certificates, add trust, install packages, timestamp, publish, export private keys, or handle PFX passwords. Keep private keys outside the repository and logs, and provision them through an approved external process.

## Build and sign

Build the unsigned package as usual; no signing certificate is required:

```powershell
cmake --preset vs2026
cmake --build build/vs2026 --config Release --target layerc_gamebar_msix -j 4
```

The package publisher in `src/gamebar/Package.appxmanifest` is `CN=LayerC`. A signer certificate must have that exact subject, a private key, current validity, and an explicit Code Signing EKU (`1.3.6.1.5.5.7.3.3`). The helper resolves it only from `Cert:\CurrentUser\My\<thumbprint>`. SignTool verification also needs the certificate to be trusted independently; the helper never changes trust.

After an approved external provisioning process has placed the signing certificate in the current user's Personal store and its public certificate in an approved trust store, configure the public thumbprint and build the opt-in target:

```powershell
$thumbprint = '<40 hexadecimal characters>'
cmake --preset vs2026 "-DLAYERC_GAMEBAR_SIGNING_THUMBPRINT=$thumbprint"
cmake --build build/vs2026 --config Release --target layerc_gamebar_signed_msix -j 4
```

CMake discovers SignTool beside the Windows SDK `MakeAppx.exe`. The signed package is `build/vs2026/gamebar-msix/Release/LayerC.GameBar.v1.signed.msix`. The target does not exist unless the public thumbprint is configured and the unsigned MSIX target is available. It depends on the unchanged unsigned target. The helper refuses to overwrite an existing signed output; review and remove only a disposable prior output explicitly before rebuilding.

The helper can also be run directly:

```powershell
$signedPackage = 'build\vs2026\gamebar-msix\Release\LayerC.GameBar.v1.signed.msix'
& .\tools\Sign-GameBarMsix.ps1 `
    -InputPath 'build\vs2026\gamebar-msix\Release\LayerC.GameBar.v1.msix' `
    -OutputPath $signedPackage `
    -CertificateThumbprint $thumbprint `
    -SignToolPath 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\SignTool.exe'
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\SignTool.exe' `
    verify /pa /all $signedPackage
```

The helper stages beside the output, signs and verifies the staged copy with SignTool `/pa /all`, then publishes it with a same-directory `File.Move` that fails rather than replacing an existing file or directory. It rejects signed input, invalid or unavailable signers, publisher mismatch, and pre-existing output. It does not contact a timestamp server.

## External certificate and package handling

Provision a disposable or authorized signing certificate outside this helper. If an approved PFX is supplied, import it only into the current user's Personal store and enter its password interactively as a secure string:

```powershell
$pfxPassword = Read-Host 'PFX password' -AsSecureString
$signer = Import-PfxCertificate `
    -FilePath 'D:\approved-private-storage\LayerC-GameBar.pfx' `
    -CertStoreLocation Cert:\CurrentUser\My `
    -Password $pfxPassword
$thumbprint = $signer.Thumbprint
```

Do not put the password in a command line, script, environment variable, transcript, or log. Signer private-key storage and package trust are separate decisions. After inspecting the public `.cer` and receiving explicit approval to trust it for local testing, import only that public certificate into the current user's `TrustedPeople` store:

```powershell
Import-Certificate `
    -FilePath 'D:\approved-public-storage\LayerC-GameBar.cer' `
    -CertStoreLocation Cert:\CurrentUser\TrustedPeople
```

`TrustedPeople` alone does not necessarily establish the chain trust required by
SignTool `/pa /all`. For a disposable self-signed test certificate, an approved
current-user `Root` entry may also be needed. Root trust affects Windows, not
just LayerC: inspect the public certificate and its complete SHA-1 thumbprint,
obtain explicit approval, and never add an unrelated or production root for a
test. In an interactive Windows desktop, after that approval:

```powershell
Import-Certificate `
    -FilePath 'D:\approved-public-storage\LayerC-GameBar.cer' `
    -CertStoreLocation Cert:\CurrentUser\Root
```

Windows can require a security confirmation for **both adding and removing** a
current-user root. Compare the displayed thumbprint with the recorded test
certificate before confirming. A headless command can wait on that dialog;
a timeout is neither successful provisioning nor completed cleanup. Do not
bypass the prompt, silently broaden trust, or keep retrying unattended. The
signer helper deliberately does not perform these operations.

Verify the resulting package before any sideload:

```powershell
& 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\SignTool.exe' `
    verify /pa /all $signedPackage
```

SignTool `/pa /all` success establishes Authenticode verification under the
calling user's trust policy; it does **not** establish Windows AppX deployment
trust. A self-signed test package can pass that verification while
`Add-AppxPackage` rejects it with `0x800B0109`, even when its certificate is in
current-user `Root` and `TrustedPeople`. Do not silently broaden trust to
machine-wide stores or elevate to make this test pass. Arrange approved
package-deployment trust externally and retain the deployment error separately.

An explicitly approved development-layout registration on a desktop where
Developer Mode is **already enabled** can exercise the widget without changing
machine trust. Inspect existing registrations and preserve data/rollback first;
never enable Developer Mode or replace an installation automatically. Register
only the reviewed staged layout:

```powershell
Add-AppxPackage -Register 'build\vs2026\gamebar-package\Release\AppxManifest.xml' `
    -DisableDevelopmentMode
```

Such development registration is **not successful signed-MSIX installation**,
production signing, Store certification, or hardware qualification. Remove only
the owned test registration and restore the recorded temporary state afterwards.

Inspect installed packages before changing package state. Preserve a rollback package or ask before replacing an existing installation; uninstall only the exact test package you own. Sideload only after reviewing identity and version:

```powershell
Get-AppxPackage -Name 'LayerC.GameBar.v1' |
    Format-List Name, PackageFullName, Version, InstallLocation, IsDevelopmentMode
Add-AppxPackage -Path $signedPackage
```

For cleanup, remove only the exact test package and disposable certificate thumbprints you recorded. Do not remove unrelated or production certificates:

```powershell
$testPackage = Get-AppxPackage -Name 'LayerC.GameBar.v1'
# After confirming this is the test package you own:
# Remove-AppxPackage -Package $testPackage.PackageFullName
# Only for the exact public test certificate imported above:
# Remove-Item -LiteralPath "Cert:\CurrentUser\TrustedPeople\$thumbprint"
# Only if you explicitly created this exact disposable test Root entry:
# Remove-Item -LiteralPath "Cert:\CurrentUser\Root\$thumbprint"
# Only for the owned disposable private key, never an external or production signer:
# Remove-Item -LiteralPath "Cert:\CurrentUser\My\$thumbprint" -DeleteKey
```

For a current-user Root removal, compare the confirmation thumbprint with the
recorded test certificate and approve only that exact removal. Then verify the
recorded store entries are absent. Externally provisioned and production
certificates or trust entries are never cleanup targets.

## Disposable tests and evidence boundary

The certificate-independent negative tests can be run without creating or trusting a certificate:

```powershell
powershell -NoProfile -File tests/gamebar_signing_tests.ps1 `
    -UnsignedPackage 'build/vs2026/gamebar-msix/Release/LayerC.GameBar.v1.msix' `
    -SignToolPath 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\SignTool.exe'
```

Without `-CertificateThumbprint`, trusted-signer tests are reported as **NOT RUN**. With an externally supplied trusted disposable signer, the optional cases sign and verify a temporary package, check that the unsigned input is unchanged, reject already-signed input, and reject a tampered payload. Tests never create or trust certificates or install packages. Disposable-key results establish local mechanics only; they do not establish production-key availability, release signing, Store certification, or automatic publishing.
