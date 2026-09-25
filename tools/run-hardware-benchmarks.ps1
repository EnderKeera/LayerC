#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateBuildDirectory,
    [string]$BaselineBuildDirectory = '',
    [switch]$VisualOnly
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$CandidateBuildDirectory = [IO.Path]::GetFullPath($CandidateBuildDirectory)

Assert-LayerCExecutables $CandidateBuildDirectory @(
    'layerd.exe', 'lctest.exe', 'layerc_capture_probe.exe')

$visual = @{ BuildDirectory = $CandidateBuildDirectory }
$runs = @(
    @{ Name = 'V1.1 visual low'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'low' } }
    @{ Name = 'V1.1 visual balanced'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'balanced' } }
    @{ Name = 'V2.1 visual template'; Script = 'benchmark_v11.ps1'
       Arguments = $visual + @{ Quality = 'balanced'; Workload = 'v21-template' } }
)
if (-not $VisualOnly) {
    if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
        throw 'BaselineBuildDirectory is required unless VisualOnly is set.'
    }
    $BaselineBuildDirectory = [IO.Path]::GetFullPath($BaselineBuildDirectory)
    Assert-LayerCExecutables $BaselineBuildDirectory @('layerd.exe')
    $runs += @{ Name = 'V2.2 input-to-overlay latency'; Script = 'benchmark_latency.ps1'
        Arguments = @{ BaselineBuildDirectory = $BaselineBuildDirectory
                       CandidateBuildDirectory = $CandidateBuildDirectory } }
}

foreach ($run in $runs) {
    Write-Host "==> $($run.Name)"
    $global:LASTEXITCODE = 0
    $arguments = $run.Arguments
    & (Join-Path $PSScriptRoot $run.Script) @arguments
    if (-not $? -or $LASTEXITCODE -ne 0) {
        throw "$($run.Name) failed with exit code $([Math]::Max(1, $LASTEXITCODE))."
    }
}

Write-Host ''
Write-Host "done. evidence: $(Join-Path $repositoryRoot 'out\benchmarks')"
