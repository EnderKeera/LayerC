#Requires -Version 5.1
# Resolves the CI run (id + head SHA) that produced the binaries under test.
# Used by tools/push-and-test.ps1 so the "latest green run" and
# "run for this commit" queries live in one place.
# Writes runId=/headSha= to $env:GITHUB_OUTPUT when present (CI) and always
# emits the resolved pair as an object for local callers.
[CmdletBinding()]
param(
    [string]$Branch = 'main',
    [string]$Workflow = 'CI',
    [string]$Commit = ''
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force

if ([string]::IsNullOrWhiteSpace($Commit)) {
    $runJson = gh run list --workflow $Workflow --branch $Branch --limit 1 `
        --status success --json databaseId,headSha -q '.[0]'
    Assert-NativeSuccess 'Listing CI runs'
    if ([string]::IsNullOrWhiteSpace($runJson)) {
        throw "No successful $Workflow run found for $Branch."
    }
} else {
    $runJson = gh run list --workflow $Workflow --branch $Branch `
        --commit $Commit --limit 1 --json databaseId,headSha -q '.[0]'
    Assert-NativeSuccess 'Listing CI runs'
    if ([string]::IsNullOrWhiteSpace($runJson)) {
        throw "No $Workflow run found for $Commit on $Branch."
    }
}
$run = $runJson | ConvertFrom-Json
$id = [string]$run.databaseId
$sha = [string]$run.headSha
if ($id -notmatch '^[0-9]+$') { throw "Invalid CI run id: $id" }
if ($sha -notmatch '^[0-9a-fA-F]{40}$') { throw "Invalid CI head SHA: $sha" }
if (-not [string]::IsNullOrWhiteSpace($Commit) -and $sha -ne $Commit) {
    throw "Resolved CI run $id for unexpected commit $sha."
}
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) {
    "runId=$id" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
    "headSha=$sha" | Out-File -FilePath $env:GITHUB_OUTPUT -Encoding utf8 -Append
}
[pscustomobject]@{ RunId = $id; HeadSha = $sha }
