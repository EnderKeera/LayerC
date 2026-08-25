#Requires -Version 5.1
# "Push and test it": push main, wait for cloud CI (compile + v23 benchmark),
# pull the CI-built binaries, then run the remaining local hardware
# benchmarks (v22 idle plateau, v11 visual, latency vs baseline).
[CmdletBinding()]
param(
    [string]$Branch = 'main',
    [string]$BaselineBuildDirectory = '',
    [switch]$NoPush,
    [switch]$VisualOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repository = gh repo view --json nameWithOwner -q .nameWithOwner
$staging = Join-Path $root 'out\bench-stage'

if (-not $NoPush) {
    Write-Host "==> pushing $Branch"
    git push origin $Branch
}

Write-Host "==> waiting for cloud CI (compile + v23) on $repository"
$runId = gh run list --workflow CI --branch $Branch --limit 1 `
    --json databaseId,displayTitle -q '.[0].databaseId'
gh run watch $runId --exit-status

Write-Host "==> pulling CI binaries into $staging"
Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $staging | Out-Null
gh run download $runId -n layerc-windows-x64-tools -D $staging

$benchmarks = @(
    @{ Name = 'v22 idle plateau';    Script = 'benchmark_v22.ps1';    Args = @('-BuildDirectory', $staging) },
    @{ Name = 'v11 visual low';      Script = 'benchmark_v11.ps1';    Args = @('-BuildDirectory', $staging, '-Quality', 'low') },
    @{ Name = 'v11 visual balanced'; Script = 'benchmark_v11.ps1';    Args = @('-BuildDirectory', $staging, '-Quality', 'balanced') },
    @{ Name = 'v11 visual tmpl';     Script = 'benchmark_v11.ps1';    Args = @('-BuildDirectory', $staging, '-Quality', 'balanced', '-Workload', 'v21-template') },
    @{ Name = 'v22 latency';         Script = 'benchmark_latency.ps1'; Args = @() }
)

if ($VisualOnly) {
    $benchmarks = $benchmarks | Where-Object { $_.Name -like 'v11*' }
}

foreach ($entry in $benchmarks) {
    $script = Join-Path $root "tools\$($entry.Script)"
    $args = @($entry.Args)
    if ($entry.Name -eq 'v22 latency') {
        if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
            $BaselineBuildDirectory = Join-Path $root 'out\layerc-vs2026-v22'
        }
        $args += @('-BaselineBuildDirectory', $BaselineBuildDirectory,
                   '-CandidateBuildDirectory', $staging)
    }
    Write-Host "==> $($entry.Name)"
    & $script @args
    if ($LASTEXITCODE -ne 0) {
        throw "$($entry.Name) failed with exit code $LASTEXITCODE"
    }
}

Write-Host ""
Write-Host "done. evidence: $(Join-Path $root 'out\benchmarks')"
