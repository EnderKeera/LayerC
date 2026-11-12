#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputPath,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [Parameter(Mandatory=$true)][string]$CertificateThumbprint,
    [string]$SignToolPath = 'signtool.exe'
)
$ErrorActionPreference = 'Stop'
Import-Module "$PSHOME\Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1" -ErrorAction Stop

Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Xml.Linq

function Test-PathExists([string]$Path) {
    return [System.IO.File]::Exists($Path) -or
        [System.IO.Directory]::Exists($Path) -or
        (Test-Path -LiteralPath $Path)
}

function Get-PackagePublisher([string]$PackagePath) {
    $archive = $null
    $manifestStream = $null
    $xmlReader = $null
    try {
        $archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
        if ($null -ne $archive.GetEntry('AppxSignature.p7x')) {
            throw 'The input MSIX is already signed.'
        }
        $manifestEntry = $archive.GetEntry('AppxManifest.xml')
        if ($null -eq $manifestEntry) {
            throw 'The input MSIX has no AppxManifest.xml.'
        }
        $manifestStream = $manifestEntry.Open()
        $settings = [System.Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $xmlReader = [System.Xml.XmlReader]::Create($manifestStream, $settings)
        $document = [System.Xml.Linq.XDocument]::Load($xmlReader)
        $namespace = 'http://schemas.microsoft.com/appx/manifest/foundation/windows10'
        $packageRoot = [System.Xml.Linq.XName]::Get('Package', $namespace)
        if (-not $document.Root.Name.Equals($packageRoot)) {
            throw 'The input MSIX manifest has an invalid root element.'
        }
        $identity = $document.Root.Element(
            [System.Xml.Linq.XName]::Get('Identity', $namespace)
        )
        if ($null -eq $identity) {
            throw 'The input MSIX manifest has no package identity.'
        }
        $publisher = $identity.Attribute([System.Xml.Linq.XName]::Get('Publisher'))
        if ($null -eq $publisher -or [string]::IsNullOrWhiteSpace($publisher.Value)) {
            throw 'The input MSIX manifest has no publisher.'
        }
        return $publisher.Value
    }
    catch {
        throw 'Input MSIX must be an unsigned package with a valid AppxManifest.xml publisher.'
    }
    finally {
        if ($null -ne $xmlReader) { $xmlReader.Dispose() }
        if ($null -ne $manifestStream) { $manifestStream.Dispose() }
        if ($null -ne $archive) { $archive.Dispose() }
    }
}

function Get-CurrentUserSigner([string]$Thumbprint, [string]$Publisher) {
    $certificatePath = "Cert:\CurrentUser\My\$Thumbprint"
    try {
        $certificate = Get-Item -LiteralPath $certificatePath -ErrorAction Stop
    }
    catch {
        throw 'No current-user Personal certificate matches the supplied thumbprint.'
    }
    if (-not $certificate.HasPrivateKey) {
        throw 'The matching current-user certificate has no private key.'
    }
    $now = [DateTime]::Now
    if ($now -lt $certificate.NotBefore -or $now -ge $certificate.NotAfter) {
        throw 'The matching current-user certificate is not currently valid.'
    }
    $hasCodeSigningEku = $false
    foreach ($extension in $certificate.Extensions) {
        if ($extension.Oid.Value -eq '2.5.29.37' -and
            $extension -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            foreach ($usage in $extension.EnhancedKeyUsages) {
                if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') {
                    $hasCodeSigningEku = $true
                }
            }
        }
    }
    if (-not $hasCodeSigningEku) {
        throw 'The matching current-user certificate lacks the Code Signing EKU.'
    }
    if (-not [string]::Equals(
        $certificate.Subject,
        $Publisher,
        [StringComparison]::Ordinal
    )) {
        throw 'The certificate subject does not match the MSIX publisher.'
    }
    return $certificate
}

function Invoke-SignTool([string]$Path, [string[]]$Arguments) {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Path @Arguments 2>&1 | Out-Null
        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Resolve-SignTool([string]$Path) {
    try {
        $application = Get-Command -Name $Path -CommandType Application -ErrorAction Stop |
            Select-Object -First 1
        return $application.Source
    }
    catch {
        throw 'SignTool executable was not found; supply its explicit path.'
    }
}

$stagingPath = $null
$stagingOwned = $false
try {
    try {
        $inputItem = Get-Item -LiteralPath $InputPath -ErrorAction Stop
    }
    catch {
        throw 'Input MSIX does not exist.'
    }
    if ($inputItem.PSIsContainer -or
        -not $inputItem.Name.EndsWith('.msix', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Input path must be an existing .msix file.'
    }
    $resolvedInputPath = $inputItem.FullName

    try {
        $outputProvider = $null
        $outputDrive = $null
        $fullOutputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
            $OutputPath,
            [ref]$outputProvider,
            [ref]$outputDrive
        )
    }
    catch {
        throw 'Output path must resolve to a filesystem location.'
    }
    if ($null -eq $outputProvider -or $outputProvider.Name -ne 'FileSystem') {
        throw 'Output path must resolve through the FileSystem provider.'
    }
    if (-not $fullOutputPath.EndsWith('.msix', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Output path must end in .msix.'
    }
    if ([string]::Equals(
        $resolvedInputPath,
        $fullOutputPath,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw 'Input and output paths must be distinct.'
    }
    $outputParent = [System.IO.Path]::GetDirectoryName($fullOutputPath)
    if (-not [System.IO.Directory]::Exists($outputParent)) {
        throw 'Output parent directory must already exist.'
    }
    if (Test-PathExists $fullOutputPath) {
        throw 'Output already exists; choose a new path or remove only a disposable output explicitly.'
    }

    $normalizedThumbprint = $CertificateThumbprint -replace '\s', ''
    if ($normalizedThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
        throw 'Certificate thumbprint must contain exactly 40 hexadecimal characters.'
    }
    $normalizedThumbprint = $normalizedThumbprint.ToUpperInvariant()

    $publisher = Get-PackagePublisher $resolvedInputPath
    $certificate = Get-CurrentUserSigner $normalizedThumbprint $publisher
    $resolvedSignTool = Resolve-SignTool $SignToolPath

    $stagingPath = Join-Path $outputParent ([Guid]::NewGuid().ToString('N') + '.staging.msix')
    $sourceStream = $null
    $stagingStream = $null
    try {
        $sourceStream = [System.IO.File]::OpenRead($resolvedInputPath)
        $stagingStream = [System.IO.File]::Open(
            $stagingPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        $stagingOwned = $true
        $sourceStream.CopyTo($stagingStream)
    }
    finally {
        if ($null -ne $stagingStream) { $stagingStream.Dispose() }
        if ($null -ne $sourceStream) { $sourceStream.Dispose() }
    }

    if ((Invoke-SignTool $resolvedSignTool @('sign', '/fd', 'SHA256', '/sha1', $normalizedThumbprint, '/s', 'My', $stagingPath)) -ne 0) {
        throw 'Game Bar MSIX signing failed.'
    }
    if ((Invoke-SignTool $resolvedSignTool @('verify', '/pa', '/all', $stagingPath)) -ne 0) {
        throw 'Game Bar MSIX signature/trust verification failed.'
    }

    if (Test-PathExists $fullOutputPath) {
        throw 'Output appeared before publication; refusing to overwrite it.'
    }
    [System.IO.File]::Move($stagingPath, $fullOutputPath)
    Write-Output $fullOutputPath
}
finally {
    if ($stagingOwned -and $null -ne $stagingPath -and [System.IO.File]::Exists($stagingPath)) {
        Remove-Item -LiteralPath $stagingPath -Force -ErrorAction SilentlyContinue
    }
}
