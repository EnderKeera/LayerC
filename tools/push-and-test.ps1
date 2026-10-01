#Requires -Version 5.1
# Push the current branch, wait for CI on that exact commit, then optionally
# download its binaries and run the local hardware checks.
[CmdletBinding()]
param(
    [string]$Branch = 'main',
    [string]$BaselineBuildDirectory = '',
    [switch]$NoPush,
    [switch]$WatchOnly,
    [switch]$VisualOnly
)

$ErrorActionPreference = 'Stop'
if ($WatchOnly) {
    $NoPush = $true
}
$root = Split-Path -Parent $PSScriptRoot
$candidateDirectory = Join-Path $root 'out\bench-stage'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force

$repository = gh repo view --json nameWithOwner -q .nameWithOwner
Assert-NativeSuccess 'Resolving the GitHub repository'
$repository = $repository.Trim()

$currentSha = git rev-parse --verify HEAD
Assert-NativeSuccess 'Resolving HEAD'
$currentSha = $currentSha.Trim()
$branchSha = git rev-parse --verify "$Branch^{commit}"
Assert-NativeSuccess "Resolving branch $Branch"
$branchSha = $branchSha.Trim()
if ($currentSha -ne $branchSha) {
    throw "HEAD ($currentSha) is not the tip of $Branch ($branchSha). Check out the branch before running."
}

$trackedChanges = @(git status --porcelain --untracked-files=no)
Assert-NativeSuccess 'Checking the working tree'
if ($trackedChanges.Count -ne 0) {
    throw 'Tracked changes are present. Commit them before running release gates.'
}

if (-not $NoPush) {
    Write-Host "==> pushing $Branch at $currentSha"
    git push origin $Branch
    Assert-NativeSuccess "Pushing $Branch"
}

Write-Host "==> resolving cloud CI for $currentSha on $repository"
$candidate = $null
$deadline = [DateTime]::UtcNow.AddMinutes(2)
while ($null -eq $candidate) {
    try {
        $candidate = & (Join-Path $PSScriptRoot 'resolve-ci-candidate.ps1') `
            -Branch $Branch -Workflow CI -Commit $currentSha
    } catch {
        if ($_.Exception.Message -notlike 'No CI run found*') { throw }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "No CI run appeared for $currentSha within two minutes."
        }
        Start-Sleep -Seconds 2
    }
}
$runId = $candidate.RunId

Write-Host "==> waiting for CI run $runId"
gh run watch $runId --exit-status
Assert-NativeSuccess "CI run $runId"
if ($WatchOnly) {
    return
}

Write-Host "==> downloading CI binaries into $candidateDirectory"
Remove-Item -LiteralPath $candidateDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $candidateDirectory | Out-Null
gh run download $runId -n layerc-windows-x64-tools -D $candidateDirectory
Assert-NativeSuccess 'Downloading CI benchmark tools'
$candidateDirectory = Resolve-LayerCBuildRoot -Path $candidateDirectory `
    -RequiredExecutables @('layerd.exe')
$null = Assert-LayerCBuildIdentity -BuildDirectory $candidateDirectory `
    -CommitSha $candidate.HeadSha -RunId $candidate.RunId

if (-not $VisualOnly) {
    if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
        $baselineStage = Join-Path $root 'out\baseline'
        Write-Host "==> downloading V2.2 baseline into $baselineStage"
        Remove-Item -LiteralPath $baselineStage -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $baselineStage | Out-Null
        gh release download baseline-v22 --repo $repository --dir $baselineStage `
            --pattern '*.zip' | Out-Null
        Assert-NativeSuccess 'Downloading the V2.2 baseline'
        $archives = @(Get-ChildItem -LiteralPath $baselineStage -Filter '*.zip' -File)
        if ($archives.Count -ne 1) {
            throw "Expected one baseline ZIP, found $($archives.Count)."
        }
        Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $baselineStage -Force
        $BaselineBuildDirectory = $baselineStage
    }
    $BaselineBuildDirectory = Resolve-LayerCBuildRoot -Path $BaselineBuildDirectory `
        -RequiredExecutables @('layerd.exe')
}
$arguments = New-LayerCHardwareBenchmarkArguments `
    -CandidateBuildDirectory $candidateDirectory `
    -BaselineBuildDirectory $BaselineBuildDirectory -VisualOnly:$VisualOnly

$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'run-hardware-benchmarks.ps1') @arguments
$succeeded = $?
$exitCode = $LASTEXITCODE
if (-not $succeeded -or $exitCode -ne 0) {
    if ($exitCode -eq 0) { $exitCode = 1 }
    throw "Hardware benchmarks failed with exit code $exitCode."
}
