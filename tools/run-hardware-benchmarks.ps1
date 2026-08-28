#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateBuildDirectory,
    [string]$BaselineBuildDirectory = '',
    [switch]$VisualOnly
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$CandidateBuildDirectory = [IO.Path]::GetFullPath($CandidateBuildDirectory)

function Assert-Executables {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string[]]$Names
    )

    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Build directory does not exist: $Directory"
    }
    foreach ($name in $Names) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            throw "Missing $name in $Directory."
        }
    }
}

function Assert-BenchmarkPassed {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [bool]$Succeeded,
        [Parameter(Mandatory = $true)]
        [int]$ExitCode
    )

    if (-not $Succeeded -or $ExitCode -ne 0) {
        if ($ExitCode -eq 0) { $ExitCode = 1 }
        throw "$Name failed with exit code $ExitCode."
    }
}

Assert-Executables $CandidateBuildDirectory @(
    'layerd.exe',
    'lctest.exe',
    'layerc_capture_probe.exe'
)

if (-not $VisualOnly) {
    if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
        throw 'BaselineBuildDirectory is required unless VisualOnly is set.'
    }
    $BaselineBuildDirectory = [IO.Path]::GetFullPath($BaselineBuildDirectory)
    Assert-Executables $BaselineBuildDirectory @('layerd.exe')
}

Write-Host '==> V1.1 visual low'
$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'benchmark_v11.ps1') -BuildDirectory $CandidateBuildDirectory -Quality low
Assert-BenchmarkPassed 'V1.1 visual low' $? $LASTEXITCODE

Write-Host '==> V1.1 visual balanced'
$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'benchmark_v11.ps1') -BuildDirectory $CandidateBuildDirectory -Quality balanced
Assert-BenchmarkPassed 'V1.1 visual balanced' $? $LASTEXITCODE

Write-Host '==> V2.1 visual template'
$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'benchmark_v11.ps1') -BuildDirectory $CandidateBuildDirectory -Quality balanced -Workload v21-template
Assert-BenchmarkPassed 'V2.1 visual template' $? $LASTEXITCODE

if (-not $VisualOnly) {
    Write-Host '==> V2.2 input-to-overlay latency'
    $global:LASTEXITCODE = 0
    & (Join-Path $PSScriptRoot 'benchmark_latency.ps1') -BaselineBuildDirectory $BaselineBuildDirectory -CandidateBuildDirectory $CandidateBuildDirectory
    Assert-BenchmarkPassed 'V2.2 input-to-overlay latency' $? $LASTEXITCODE
}

Write-Host ''
Write-Host "done. evidence: $(Join-Path $repositoryRoot 'out\benchmarks')"
