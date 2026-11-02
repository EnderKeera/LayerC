#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateBuildDirectory,
    [string]$BaselineBuildDirectory = '',
    [switch]$VisualOnly,
    [string]$OutputDirectory = '',
    [switch]$ContinueOnFailure
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$CandidateBuildDirectory = [IO.Path]::GetFullPath($CandidateBuildDirectory)

Assert-LayerCExecutables $CandidateBuildDirectory @(
    'layerd.exe', 'lctest.exe', 'layerc_capture_probe.exe')

if (-not [string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
}

$visual = @{ BuildDirectory = $CandidateBuildDirectory }
$runs = @(
    @{ Id = 'v11-visual-low'; Name = 'V1.1 visual low'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'low' } }
    @{ Id = 'v11-visual-balanced'; Name = 'V1.1 visual balanced'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'balanced' } }
    @{ Id = 'v21-template'; Name = 'V2.1 visual template'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'balanced'; Workload = 'v21-template' } }
)
if (-not $VisualOnly) {
    if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
        throw 'BaselineBuildDirectory is required unless VisualOnly is set.'
    }
    $BaselineBuildDirectory = [IO.Path]::GetFullPath($BaselineBuildDirectory)
    Assert-LayerCExecutables $BaselineBuildDirectory @('layerd.exe')
    $runs += @{ Id = 'v22-latency'; Name = 'V2.2 input-to-overlay latency'; Script = 'benchmark_latency.ps1'
        Arguments = @{ BaselineBuildDirectory = $BaselineBuildDirectory
                       CandidateBuildDirectory = $CandidateBuildDirectory } }
}

$failed = @()
foreach ($run in $runs) {
    Write-Host "==> $($run.Name)"
    $global:LASTEXITCODE = 0
    $arguments = $run.Arguments
    if (-not [string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $arguments.OutputPath = Join-Path $OutputDirectory "$($run.Id).json"
    }
    try {
        & (Join-Path $PSScriptRoot $run.Script) @arguments
        if (-not $? -or $LASTEXITCODE -ne 0) {
            throw "$($run.Name) failed with exit code $([Math]::Max(1, $LASTEXITCODE))."
        }
    } catch {
        if (-not $ContinueOnFailure) { throw }
        Write-Warning "$($run.Id): $($_.Exception.Message)"
        $failed += $run.Id
    }
}
if ($failed.Count -ne 0) {
    throw "Hardware benchmarks failed: $($failed -join ', ')."
}

Write-Host ''
$evidenceDirectory = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $repositoryRoot 'out\benchmarks'
} else { $OutputDirectory }
Write-Host "done. evidence: $evidenceDirectory"
