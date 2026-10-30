#Requires -Version 5.1
[CmdletBinding()]
param(
    [int]$PullRequest = 0,
    [string]$Commit = '',
    [string]$Repository = '',
    [string]$FromResults = '',
    [string]$BaselineBuildDirectory = '',
    [string]$BuiltCommitParentsFile = '',
    [ValidateSet('physical', 'vm')][string]$Machine = 'physical',
    [switch]$VisualOnly,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force

function Resolve-EvidenceBuildParents {
    param(
        [string]$Built,
        [string]$Commit,
        [string]$Repository,
        [string]$Stage,
        [string]$ParentsFile
    )
    if ($Built -eq $Commit) { return '' }
    if ([string]::IsNullOrWhiteSpace($ParentsFile)) {
        $parents = @(gh api "repos/$Repository/commits/$Built" --jq '.parents[].sha')
        Assert-NativeSuccess 'Reading the CI build commit parents'
        $ParentsFile = Join-Path $Stage 'built-commit-parents.txt'
        [IO.File]::WriteAllText($ParentsFile, ($parents -join "`n") + "`n",
            (New-Object System.Text.UTF8Encoding($false)))
    }
    $parents = @(Get-Content -LiteralPath $ParentsFile | ForEach-Object {
        $_.Trim().ToLowerInvariant()
    } | Where-Object { $_ -ne '' })
    if ($parents.Count -ne 2 -or
        @($parents | Where-Object { $_ -cnotmatch '^[0-9a-f]{40}$' }).Count -ne 0 -or
        $parents[1] -ne $Commit) {
        throw "CI artifact was built from $Built, which is neither $Commit nor its pull request merge commit."
    }
    return $ParentsFile
}

$root = Split-Path -Parent $PSScriptRoot
$python = if ([string]::IsNullOrWhiteSpace($env:PYTHON)) { 'python' } else { $env:PYTHON }
if (-not [string]::IsNullOrWhiteSpace($FromResults)) {
    $FromResults = [IO.Path]::GetFullPath($FromResults)
}
if (-not [string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
    $BaselineBuildDirectory = [IO.Path]::GetFullPath($BaselineBuildDirectory)
}
if (-not [string]::IsNullOrWhiteSpace($BuiltCommitParentsFile)) {
    $BuiltCommitParentsFile = [IO.Path]::GetFullPath($BuiltCommitParentsFile)
}
if (-not $DryRun -and $PullRequest -le 0) {
    throw 'PullRequest is required unless DryRun is set.'
}

Push-Location $root
try {
    if ([string]::IsNullOrWhiteSpace($Repository)) {
        $Repository = gh repo view --json nameWithOwner -q .nameWithOwner
        Assert-NativeSuccess 'Resolving the GitHub repository'
        $Repository = $Repository.Trim()
    }
    if ($Repository -cnotmatch '^[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}$') {
        throw 'Repository must be OWNER/REPO.'
    }
    if ([string]::IsNullOrWhiteSpace($Commit)) {
        if ($PullRequest -le 0) { throw 'Specify Commit or PullRequest.' }
        $prJson = gh pr view $PullRequest --repo $Repository --json headRefOid,state,mergeCommit
        Assert-NativeSuccess 'Resolving the pull request commit'
        $pr = $prJson | ConvertFrom-Json
        $Commit = if ($pr.state -eq 'MERGED') { $pr.mergeCommit.oid } else { $pr.headRefOid }
    }
    if ($Commit -notmatch '^[0-9a-fA-F]{40}$') { throw 'Commit must be a full 40-digit hex SHA.' }
    $Commit = $Commit.ToLowerInvariant()
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $stage = Join-Path $root "out\desktop-evidence\$($Commit.Substring(0, 12))-$stamp"
    New-Item -ItemType Directory -Force $stage | Out-Null

    if (-not [string]::IsNullOrWhiteSpace($FromResults)) {
        $results = $FromResults
        $identityPath = Join-Path $results 'layerc-build-identity.json'
        if (-not (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
            throw "Missing CI artifact build identity: $identityPath"
        }
        $identity = Get-LayerCBuildIdentity -BuildDirectory $results
        if ($identity.source -ne 'ci-artifact') { throw 'Results have no CI artifact build identity.' }
        $parentsPath = Resolve-EvidenceBuildParents -Built $identity.commitSha -Commit $Commit `
            -Repository $Repository -Stage $stage -ParentsFile $BuiltCommitParentsFile
    } else {
        $head = Get-LayerCCommitSha
        if ($head -ne $Commit) { throw "Checkout HEAD ($head) does not match $Commit." }
        $trackedChanges = @(git status --porcelain --untracked-files=no)
        Assert-NativeSuccess 'Checking the working tree'
        if ($trackedChanges.Count -ne 0) { throw 'Tracked changes are present; use a clean checkout.' }
        Assert-LayerCQuiescent -ProcessNames layerd, layere `
            -FailureMessage 'Close LayerD and LayerE before collecting desktop evidence.'
        $runJson = gh run list --repo $Repository --workflow CI --commit $Commit `
            --status success --limit 1 --json databaseId,headSha -q '.[0]'
        Assert-NativeSuccess 'Resolving successful CI'
        if ([string]::IsNullOrWhiteSpace($runJson)) { throw "No successful CI run found for $Commit." }
        $run = $runJson | ConvertFrom-Json
        $runId = [string]$run.databaseId
        if ($run.headSha -ne $Commit -or $runId -notmatch '^[0-9]+$') {
            throw 'Resolved CI run does not name the requested commit.'
        }
        $tools = Join-Path $stage 'tools'
        gh run download $runId --repo $Repository -n layerc-windows-x64-tools -D $tools
        Assert-NativeSuccess 'Downloading CI benchmark tools'
        $tools = Resolve-LayerCBuildRoot -Path $tools `
            -RequiredExecutables @('layerd.exe', 'lctest.exe', 'layerc_capture_probe.exe')
        $identity = Get-LayerCBuildIdentity -BuildDirectory $tools
        if ($identity.source -ne 'ci-artifact' -or $identity.runId -ne $runId) {
            throw 'Downloaded build identity does not name the resolved CI run.'
        }
        $identityPath = Join-Path $tools 'layerc-build-identity.json'
        $parentsPath = Resolve-EvidenceBuildParents -Built $identity.commitSha -Commit $Commit `
            -Repository $Repository -Stage $stage -ParentsFile $BuiltCommitParentsFile
        if (-not $VisualOnly) {
            if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
                $baseline = Join-Path $stage 'baseline'
                New-Item -ItemType Directory -Force $baseline | Out-Null
                gh release download baseline-v22 --repo $Repository --dir $baseline --pattern '*.zip'
                Assert-NativeSuccess 'Downloading the V2.2 baseline'
                $archives = @(Get-ChildItem -LiteralPath $baseline -Filter '*.zip' -File)
                if ($archives.Count -ne 1) { throw "Expected one baseline ZIP, found $($archives.Count)." }
                Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $baseline -Force
                $BaselineBuildDirectory = $baseline
            }
            $BaselineBuildDirectory = Resolve-LayerCBuildRoot -Path $BaselineBuildDirectory `
                -RequiredExecutables @('layerd.exe')
        }
        $results = Join-Path $stage 'results'
        New-Item -ItemType Directory -Force $results | Out-Null
        Copy-Item -LiteralPath $identityPath -Destination (Join-Path $results 'layerc-build-identity.json')
        $arguments = New-LayerCHardwareBenchmarkArguments -CandidateBuildDirectory $tools `
            -BaselineBuildDirectory $BaselineBuildDirectory -VisualOnly:$VisualOnly
        $arguments.OutputDirectory = $results
        $arguments.ContinueOnFailure = $true
        try {
            & (Join-Path $PSScriptRoot 'run-hardware-benchmarks.ps1') @arguments
        } catch {
            Write-Warning "Benchmarks did not all pass; the report will include failures: $($_.Exception.Message)"
        }
    }
    $gpu = ''
    try {
        $gpu = (@(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { $_.Name }) -join '; ')
    } catch {
        $gpu = ''
    }
    $report = Join-Path $stage 'report.json'
    $adapterArguments = @('windows-report', '--results', $results, '--identity', $identityPath,
        '--commit', $Commit, '--repository', $Repository, '--os', [Environment]::OSVersion.VersionString,
        '--machine', $Machine, '--out', $report)
    if (-not [string]::IsNullOrWhiteSpace($gpu)) { $adapterArguments += @('--gpu', $gpu) }
    if ($VisualOnly) { $adapterArguments += '--visual-only' }
    if ($parentsPath) { $adapterArguments += @('--built-commit-parents', $parentsPath) }
    & $python (Join-Path $PSScriptRoot 'desktop_evidence_layerc.py') @adapterArguments
    Assert-NativeSuccess 'Creating the desktop report'
    $comment = Join-Path $stage 'comment.md'
    $commentArguments = @('comment', '--config', (Join-Path $root '.github/desktop-evidence.json'),
        '--report', $report, '--repository', $Repository, '--out', $comment)
    if ($parentsPath) { $commentArguments += @('--built-commit-parents', $parentsPath) }
    & $python (Join-Path $PSScriptRoot 'desktop_evidence.py') @commentArguments
    Assert-NativeSuccess 'Validating and rendering the comment'
    if ($DryRun) {
        Get-Content -LiteralPath $comment -Raw
        Write-Host "Dry run: not posted. Comment: $comment"
    } else {
        gh pr comment $PullRequest --repo $Repository --body-file $comment
        Assert-NativeSuccess 'Posting the desktop evidence comment'
    }
} finally {
    Pop-Location
}
