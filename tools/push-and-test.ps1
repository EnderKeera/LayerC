#Requires -Version 5.1
# Push the current branch, wait for CI on that exact commit, download its
# binaries and the frozen V2.2 baseline, then run the local hardware gates.
[CmdletBinding()]
param(
    [string]$Branch = 'main',
    [string]$BaselineBuildDirectory = '',
    [switch]$NoPush,
    [switch]$VisualOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$candidateDirectory = Join-Path $root 'out\bench-stage'

function Assert-NativeSuccess {
    param([Parameter(Mandatory = $true)][string]$Operation)
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

function Resolve-BuildRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RequiredExecutable
    )

    $Path = [IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath (Join-Path $Path $RequiredExecutable) -PathType Leaf) {
        return $Path
    }
    $candidates = @(
        Get-ChildItem -LiteralPath $Path -Directory |
            Where-Object {
                Test-Path -LiteralPath (Join-Path $_.FullName $RequiredExecutable) -PathType Leaf
            }
    )
    if ($candidates.Count -ne 1) {
        throw "Expected one build root containing $RequiredExecutable under $Path; found $($candidates.Count)."
    }
    return $candidates[0].FullName
}

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
$run = $null
$deadline = [DateTime]::UtcNow.AddMinutes(2)
do {
    $runJson = gh run list --workflow CI --branch $Branch --commit $currentSha `
        --limit 1 --json databaseId,headSha -q '.[0]'
    Assert-NativeSuccess 'Listing CI runs'
    if (-not [string]::IsNullOrWhiteSpace($runJson)) {
        $run = $runJson | ConvertFrom-Json
        break
    }
    Start-Sleep -Seconds 2
} while ([DateTime]::UtcNow -lt $deadline)

if ($null -eq $run) {
    throw "No CI run appeared for $currentSha within two minutes."
}
if ([string]$run.headSha -ne $currentSha) {
    throw "Resolved CI run $($run.databaseId) for unexpected commit $($run.headSha)."
}
$runId = [string]$run.databaseId

Write-Host "==> waiting for CI run $runId"
gh run watch $runId --exit-status
Assert-NativeSuccess "CI run $runId"

Write-Host "==> downloading CI binaries into $candidateDirectory"
Remove-Item -LiteralPath $candidateDirectory -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $candidateDirectory | Out-Null
gh run download $runId -n layerc-windows-x64-tools -D $candidateDirectory
Assert-NativeSuccess 'Downloading CI benchmark tools'
$candidateDirectory = Resolve-BuildRoot $candidateDirectory 'layerd.exe'

$arguments = @('-CandidateBuildDirectory', $candidateDirectory)
if ($VisualOnly) {
    $arguments += '-VisualOnly'
} else {
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
    $BaselineBuildDirectory = Resolve-BuildRoot $BaselineBuildDirectory 'layerd.exe'
    $arguments += @('-BaselineBuildDirectory', $BaselineBuildDirectory)
}

$global:LASTEXITCODE = 0
& (Join-Path $PSScriptRoot 'run-hardware-benchmarks.ps1') @arguments
$succeeded = $?
$exitCode = $LASTEXITCODE
if (-not $succeeded -or $exitCode -ne 0) {
    if ($exitCode -eq 0) { $exitCode = 1 }
    throw "Hardware benchmarks failed with exit code $exitCode."
}
