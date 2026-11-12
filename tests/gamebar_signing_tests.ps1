[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$UnsignedPackage,
    [Parameter(Mandatory=$true)][string]$SignToolPath,
    [string]$CertificateThumbprint,
    [string]$WrongPublisherThumbprint,
    [string]$NoPrivateKeyThumbprint,
    [string]$ExpiredThumbprint,
    [string]$WrongEkuThumbprint,
    [string]$UntrustedThumbprint
)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$evidenceDirectory = Join-Path $repositoryRoot 'out\release-readiness'
$signingScript = Join-Path $repositoryRoot 'tools\Sign-GameBarMsix.ps1'
$unsignedItem = Get-Item -LiteralPath $UnsignedPackage -ErrorAction Stop
if ($unsignedItem.PSIsContainer -or
    -not $unsignedItem.Name.EndsWith('.msix', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'UnsignedPackage must be an existing .msix file.'
}
$unsignedPath = $unsignedItem.FullName
[System.IO.Directory]::CreateDirectory($evidenceDirectory) | Out-Null
if (-not (Test-Path -LiteralPath $SignToolPath -PathType Leaf) -and
    -not (Get-Command -Name $SignToolPath -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'SignTool executable was not found.'
}

function Get-FileSha256([string]$Path) {
    $stream = $null
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha256.Dispose()
    }
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-NoStagingFiles([string]$Directory) {
    $stagingFiles = @(Get-ChildItem -LiteralPath $Directory -Filter '*.staging.msix' `
        -File -ErrorAction SilentlyContinue)
    Assert-True ($stagingFiles.Count -eq 0) "Staging files remain in $Directory."
}

function Invoke-Application([string]$Executable, [string[]]$Arguments) {
    $previousErrorActionPreference = $ErrorActionPreference
    $nativeOutput = @()
    $exitCode = $null
    try {
        $ErrorActionPreference = 'Continue'
        $nativeOutput = @(& $Executable @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    $outputText = (@($nativeOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine)
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $outputText }
}

function Invoke-Signer(
    [string]$PackagePath,
    [string]$DestinationPath,
    [string]$Thumbprint,
    [switch]$OmitThumbprint
) {
    $arguments = @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', $signingScript,
        '-InputPath', $PackagePath,
        '-OutputPath', $DestinationPath,
        '-SignToolPath', $SignToolPath
    )
    if (-not $OmitThumbprint) {
        $arguments += @('-CertificateThumbprint', $Thumbprint)
    }
    return Invoke-Application $script:PowerShellExecutable $arguments
}

function Assert-Rejected(
    [string]$CaseName,
    [string]$PackagePath,
    [string]$DestinationPath,
    [string]$Thumbprint,
    [string]$ExpectedError,
    [switch]$OmitThumbprint
) {
    $beforeHash = if (Test-Path -LiteralPath $PackagePath -PathType Leaf) {
        Get-FileSha256 $PackagePath
    } else {
        $null
    }
    $result = Invoke-Signer $PackagePath $DestinationPath $Thumbprint -OmitThumbprint:$OmitThumbprint
    Assert-True ($result.ExitCode -ne 0) "$CaseName was unexpectedly accepted. Output: $($result.Output)"
    if ($null -ne $beforeHash) {
        Assert-True ((Get-FileSha256 $PackagePath) -ceq $beforeHash) "$CaseName changed its input."
    }
    Assert-True (-not (Test-Path -LiteralPath $DestinationPath)) "$CaseName created an output."
    if (-not [string]::IsNullOrEmpty($ExpectedError)) {
        Assert-True (
            $result.Output.IndexOf($ExpectedError, [StringComparison]::OrdinalIgnoreCase) -ge 0
        ) "$CaseName failed for the wrong reason. Expected '$ExpectedError'; got: $($result.Output)"
    }
    $destinationParent = [System.IO.Path]::GetDirectoryName(
        [System.IO.Path]::GetFullPath($DestinationPath)
    )
    Assert-NoStagingFiles $destinationParent
    Write-Output "PASS $CaseName"
}

function Write-NotRun([string]$CaseName) {
    Write-Output "${CaseName}: NOT RUN (thumbprint not supplied)."
}

function ConvertTo-PowerShellLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-RelativeOutputCase([string]$Thumbprint, [string]$TemporaryDirectory) {
    $providerDirectory = Join-Path $TemporaryDirectory 'provider-location'
    New-Item -ItemType Directory -Path $providerDirectory -ErrorAction Stop | Out-Null
    $relativeName = 'relative-' + [Guid]::NewGuid().ToString('N') + '.msix'
    $providerOutput = Join-Path $providerDirectory $relativeName
    $processOutput = Join-Path $repositoryRoot $relativeName
    Assert-True (-not (Test-Path -LiteralPath $providerOutput)) 'Relative output fixture already exists.'
    Assert-True (-not (Test-Path -LiteralPath $processOutput)) 'Process-CWD output fixture already exists.'

    $wrapperPath = Join-Path $TemporaryDirectory 'invoke-relative-output.ps1'
    $stdoutPath = Join-Path $TemporaryDirectory 'relative-output.stdout'
    $stderrPath = Join-Path $TemporaryDirectory 'relative-output.stderr'
    $originalHash = Get-FileSha256 $unsignedPath
    $process = $null
    try {
        $lines = @(
            '$ErrorActionPreference = ''Stop'''
            'Set-Location -LiteralPath ' + (ConvertTo-PowerShellLiteral $providerDirectory)
            '[System.Environment]::CurrentDirectory = ' + (ConvertTo-PowerShellLiteral $repositoryRoot)
            'if ([System.IO.Path]::GetFullPath((Get-Location).ProviderPath) -eq [System.IO.Path]::GetFullPath([System.Environment]::CurrentDirectory)) { throw ''Provider and process locations must differ.'' }'
            '& ' + (ConvertTo-PowerShellLiteral $signingScript) +
                ' -InputPath ' + (ConvertTo-PowerShellLiteral $unsignedPath) +
                ' -OutputPath ' + (ConvertTo-PowerShellLiteral $relativeName) +
                ' -CertificateThumbprint ' + (ConvertTo-PowerShellLiteral $Thumbprint) +
                ' -SignToolPath ' + (ConvertTo-PowerShellLiteral $SignToolPath)
        )
        [System.IO.File]::WriteAllLines(
            $wrapperPath,
            $lines,
            [System.Text.UTF8Encoding]::new($false)
        )
        $arguments = @(
            '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', ('"' + $wrapperPath + '"')
        )
        $process = Start-Process -FilePath $script:PowerShellExecutable -WindowStyle Hidden `
            -ArgumentList $arguments -WorkingDirectory $repositoryRoot `
            -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        [void]$process.Handle
        $process.WaitForExit()
        $process.Refresh()
        $childOutput = ''
        if (Test-Path -LiteralPath $stdoutPath) {
            $childOutput += [System.IO.File]::ReadAllText($stdoutPath)
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $childOutput += [System.IO.File]::ReadAllText($stderrPath)
        }
        Assert-True ($process.ExitCode -eq 0) "Relative-output signing failed: $childOutput"
        Assert-True (Test-Path -LiteralPath $providerOutput -PathType Leaf) `
            'Relative output was not published in the PowerShell provider location.'
        Assert-True (-not (Test-Path -LiteralPath $processOutput)) `
            'Relative output was published in the process working directory.'
        Assert-True ((Get-FileSha256 $unsignedPath) -ceq $originalHash) `
            'Relative-output signing changed the unsigned input.'
        Assert-NoStagingFiles $providerDirectory
        Write-Output 'PASS relative output follows PowerShell provider location'
        return [pscustomobject]@{ ProviderOutput = $providerOutput; ProcessOutput = $processOutput }
    }
    finally {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill()
                    $process.WaitForExit()
                }
            }
            catch {
            }
            $process.Dispose()
        }
        if (Test-Path -LiteralPath $processOutput) {
            Remove-Item -LiteralPath $processOutput -Force -ErrorAction SilentlyContinue
        }
    }
}

if (Test-Path -LiteralPath (Join-Path $PSHOME 'pwsh.exe')) {
    $script:PowerShellExecutable = Join-Path $PSHOME 'pwsh.exe'
} else {
    $script:PowerShellExecutable = Join-Path $PSHOME 'powershell.exe'
}
$temporaryDirectory = Join-Path $evidenceDirectory ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryDirectory -ErrorAction Stop | Out-Null
$relativeOutputFixture = $null
try {
    $originalHash = Get-FileSha256 $unsignedPath
    $validSyntaxThumbprint = '0000000000000000000000000000000000000000'

    Assert-Rejected 'missing mandatory certificate thumbprint' $unsignedPath `
        (Join-Path $temporaryDirectory 'missing-thumbprint.msix') '' 'CertificateThumbprint' -OmitThumbprint
    Assert-Rejected 'malformed certificate thumbprint' $unsignedPath `
        (Join-Path $temporaryDirectory 'malformed-thumbprint.msix') 'not-a-thumbprint' `
        'exactly 40 hexadecimal characters'
    Assert-Rejected 'missing certificate in CurrentUser My' $unsignedPath `
        (Join-Path $temporaryDirectory 'missing-certificate.msix') $validSyntaxThumbprint `
        'No current-user Personal certificate matches'
    Assert-Rejected 'missing input package' `
        (Join-Path $temporaryDirectory 'missing-input.msix') `
        (Join-Path $temporaryDirectory 'missing-input-output.msix') $validSyntaxThumbprint `
        'Input MSIX does not exist'

    $samePathHash = Get-FileSha256 $unsignedPath
    $samePath = Invoke-Signer $unsignedPath $unsignedPath $validSyntaxThumbprint
    Assert-True ($samePath.ExitCode -ne 0) 'Same input/output path was unexpectedly accepted.'
    Assert-True ((Get-FileSha256 $unsignedPath) -ceq $samePathHash) 'Same-path rejection changed the input.'
    Assert-True (
        $samePath.Output.IndexOf('Input and output paths must be distinct', [StringComparison]::OrdinalIgnoreCase) -ge 0
    ) "Same input/output path failed for the wrong reason: $($samePath.Output)"
    Write-Output 'PASS same input/output path'

    $sentinelOutput = Join-Path $temporaryDirectory 'sentinel.msix'
    [System.IO.File]::WriteAllText($sentinelOutput, 'sentinel')
    $sentinelHash = Get-FileSha256 $sentinelOutput
    $sentinelInputHash = Get-FileSha256 $unsignedPath
    $sentinelResult = Invoke-Signer $unsignedPath $sentinelOutput $validSyntaxThumbprint
    Assert-True ($sentinelResult.ExitCode -ne 0) 'Pre-existing output was unexpectedly accepted.'
    Assert-True (
        $sentinelResult.Output.IndexOf('Output already exists', [StringComparison]::OrdinalIgnoreCase) -ge 0
    ) "Pre-existing output failed for the wrong reason: $($sentinelResult.Output)"
    Assert-True ((Get-FileSha256 $sentinelOutput) -ceq $sentinelHash) 'Pre-existing output was changed.'
    Assert-True ((Get-FileSha256 $unsignedPath) -ceq $sentinelInputHash) 'Pre-existing-output rejection changed the input.'
    Assert-NoStagingFiles $temporaryDirectory
    Write-Output 'PASS pre-existing output file sentinel preserved'

    $directoryOutput = Join-Path $temporaryDirectory 'existing-directory.msix'
    New-Item -ItemType Directory -Path $directoryOutput -ErrorAction Stop | Out-Null
    $directorySentinel = Join-Path $directoryOutput 'sentinel.txt'
    [System.IO.File]::WriteAllText($directorySentinel, 'directory-sentinel')
    $directorySentinelHash = Get-FileSha256 $directorySentinel
    $directoryInputHash = Get-FileSha256 $unsignedPath
    $directoryResult = Invoke-Signer $unsignedPath $directoryOutput $validSyntaxThumbprint
    Assert-True ($directoryResult.ExitCode -ne 0) 'Pre-existing output directory was unexpectedly accepted.'
    Assert-True (
        $directoryResult.Output.IndexOf('Output already exists', [StringComparison]::OrdinalIgnoreCase) -ge 0
    ) "Pre-existing output directory failed for the wrong reason: $($directoryResult.Output)"
    Assert-True ((Get-FileSha256 $directorySentinel) -ceq $directorySentinelHash) `
        'Pre-existing output directory sentinel was changed.'
    Assert-True ((Get-FileSha256 $unsignedPath) -ceq $directoryInputHash) `
        'Pre-existing-output-directory rejection changed the input.'
    Assert-NoStagingFiles $temporaryDirectory
    Write-Output 'PASS pre-existing output directory preserved'

    $malformedInput = Join-Path $temporaryDirectory 'malformed.msix'
    [System.IO.File]::WriteAllBytes($malformedInput, [byte[]](0x4D, 0x53, 0x49, 0x58))
    Assert-Rejected 'malformed MSIX' $malformedInput `
        (Join-Path $temporaryDirectory 'malformed-output.msix') $validSyntaxThumbprint `
        'Input MSIX must be an unsigned package with a valid AppxManifest.xml publisher'

    Assert-True ((Get-FileSha256 $unsignedPath) -ceq $originalHash) `
        'A certificate-independent negative case changed the unsigned package.'

    $optionalSigners = @(
        @{ Name = 'wrong-publisher certificate'; Thumbprint = $WrongPublisherThumbprint; Error = 'does not match the MSIX publisher' }
        @{ Name = 'no-private-key certificate'; Thumbprint = $NoPrivateKeyThumbprint; Error = 'has no private key' }
        @{ Name = 'expired certificate'; Thumbprint = $ExpiredThumbprint; Error = 'not currently valid' }
        @{ Name = 'wrong-EKU certificate'; Thumbprint = $WrongEkuThumbprint; Error = 'lacks the Code Signing EKU' }
        @{ Name = 'untrusted certificate'; Thumbprint = $UntrustedThumbprint; Error = 'signature/trust verification failed' }
    )
    foreach ($case in $optionalSigners) {
        if ([string]::IsNullOrWhiteSpace($case.Thumbprint)) {
            Write-NotRun $case.Name
        } else {
            Assert-Rejected $case.Name $unsignedPath `
                (Join-Path $temporaryDirectory (($case.Name -replace '[^A-Za-z0-9]+', '-') + '.msix')) `
                $case.Thumbprint $case.Error
        }
    }

    if ([string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        Write-Output 'Trusted signing, /pa /all verification, already-signed rejection, ZIP payload tamper, and relative-output cases: NOT RUN (trusted signer thumbprint not supplied).'
    } else {
        $signedOutput = Join-Path $temporaryDirectory 'signed.msix'
        $signResult = Invoke-Signer $unsignedPath $signedOutput $CertificateThumbprint
        Assert-True ($signResult.ExitCode -eq 0) "Trusted-signer signing failed: $($signResult.Output)"
        $verifyResult = Invoke-Application $SignToolPath @('verify', '/pa', '/all', $signedOutput)
        Assert-True ($verifyResult.ExitCode -eq 0) `
            "SignTool verify /pa /all rejected the signed MSIX: $($verifyResult.Output)"
        Assert-True ((Get-FileSha256 $unsignedPath) -ceq $originalHash) 'Signing changed the unsigned input.'
        Assert-NoStagingFiles $temporaryDirectory
        Write-Output 'PASS trusted signer signs and verifies /pa /all'

        Assert-Rejected 'already-signed input' $signedOutput `
            (Join-Path $temporaryDirectory 'signed-input-output.msix') $CertificateThumbprint `
            'Input MSIX must be an unsigned package with a valid AppxManifest.xml publisher'

        $tamperedPackage = Join-Path $temporaryDirectory 'tampered.msix'
        Copy-Item -LiteralPath $signedOutput -Destination $tamperedPackage -ErrorAction Stop
        $archive = [System.IO.Compression.ZipFile]::Open(
            $tamperedPackage,
            [System.IO.Compression.ZipArchiveMode]::Update
        )
        try {
            $payloadEntry = $archive.GetEntry('layerc-gamebar.exe')
            if ($null -eq $payloadEntry) { throw 'Signed MSIX has no layerc-gamebar.exe payload.' }
            $payloadEntry.Delete()
            $payloadEntry = $archive.CreateEntry('layerc-gamebar.exe')
            $payloadStream = $payloadEntry.Open()
            try { $payloadStream.WriteByte([byte]0x58) }
            finally { $payloadStream.Dispose() }
        }
        finally {
            $archive.Dispose()
        }
        $tamperVerify = Invoke-Application $SignToolPath @('verify', '/pa', '/all', $tamperedPackage)
        Assert-True ($tamperVerify.ExitCode -ne 0) `
            "Tampered signed MSIX unexpectedly passed SignTool verification: $($tamperVerify.Output)"
        Write-Output 'PASS tampered ZIP payload fails SignTool verify /pa /all'

        $relativeOutputFixture = Invoke-RelativeOutputCase $CertificateThumbprint $temporaryDirectory
    }
}
finally {
    if ($null -ne $relativeOutputFixture -and
        (Test-Path -LiteralPath $relativeOutputFixture.ProcessOutput)) {
        Remove-Item -LiteralPath $relativeOutputFixture.ProcessOutput -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
