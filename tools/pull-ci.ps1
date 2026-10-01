#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repository = '',
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptRoot
Import-Module (Join-Path $scriptRoot 'LayerC.Tools.psm1') -Force

if ([string]::IsNullOrWhiteSpace($Repository)) {
    $Repository = gh repo view --json nameWithOwner -q .nameWithOwner
    Assert-NativeSuccess 'Resolving the GitHub repository'
}
$Repository = $Repository.Trim()
if ([string]::IsNullOrWhiteSpace($Repository)) {
    throw 'Repository must not be empty.'
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repositoryRoot 'out\layerc'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force $outputParent | Out-Null

$operationId = [Guid]::NewGuid().ToString('N')
$staging = Join-Path $outputParent ".layerc-pull-$operationId"
$download = Join-Path $staging 'download'
$extract = Join-Path $staging 'extract'
$backup = Join-Path $outputParent ".layerc-previous-$operationId"
$logDirectory = Join-Path $repositoryRoot 'out\pulldown'
$log = Join-Path $logDirectory 'pull.log'
New-Item -ItemType Directory -Force $download, $extract, $logDirectory | Out-Null

function Write-PullLog {
    param([Parameter(Mandatory = $true)][string]$Message)
    [IO.File]::AppendAllText(
        $log,
        ("[{0}] {1}" -f (Get-Date -Format s), $Message) + [Environment]::NewLine)
}

Write-PullLog "pulling rolling build from $Repository"
$oldOutputMoved = $false
try {
    gh release download rolling --repo $Repository --dir $download `
        --pattern '*.zip' | Out-Null
    Assert-NativeSuccess 'Downloading the rolling release'

    $archives = @(Get-ChildItem -LiteralPath $download -Filter '*.zip' -File)
    if ($archives.Count -ne 1) {
        throw "Expected one rolling ZIP, found $($archives.Count)."
    }

    # publish-rolling.sh commits a publication by writing notes that name the
    # tagged commit and the ZIP's SHA-256. Anything else is a partial
    # publication (or one replaced mid-download); refuse it.
    $notes = gh release view rolling --repo $Repository --json body --jq .body
    Assert-NativeSuccess 'Reading the rolling release notes'
    $tagCommit = gh api "repos/$Repository/git/ref/tags/rolling" --jq .object.sha
    Assert-NativeSuccess 'Resolving the rolling tag'
    $notes = $notes -join "`n"
    $claimedCommit = [regex]::Match($notes, 'main@([0-9a-f]{40})').Groups[1].Value
    $claimedDigest = [regex]::Match(
        $notes, 'layerc-windows-x64\.zip SHA-256: ([0-9a-f]{64})').Groups[1].Value
    $digest = (Get-FileHash -LiteralPath $archives[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([string]::IsNullOrEmpty($claimedCommit) -or "$tagCommit".Trim() -cne $claimedCommit -or
        [string]::IsNullOrEmpty($claimedDigest) -or $digest -cne $claimedDigest) {
        throw ('The rolling release is incomplete or inconsistent (tag {0}, notes main@{1}, ' +
               'ZIP {2}, notes SHA-256 {3}). Retry after publish-rolling finishes.') -f `
            "$tagCommit".Trim(), $claimedCommit, $digest, $claimedDigest
    }
    Write-PullLog "verified rolling main@$claimedCommit ($digest)"
    Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $extract

    $installRoot = Resolve-LayerCBuildRoot -Path $extract `
        -RequiredExecutables @('layerd.exe', 'layere.exe', 'layercal.exe', 'layerw.exe')

    if (Test-Path -LiteralPath $OutputPath) {
        Move-Item -LiteralPath $OutputPath -Destination $backup
        $oldOutputMoved = $true
    }
    try {
        Move-Item -LiteralPath $installRoot -Destination $OutputPath
    } catch {
        if ($oldOutputMoved -and -not (Test-Path -LiteralPath $OutputPath) -and
            (Test-Path -LiteralPath $backup)) {
            Move-Item -LiteralPath $backup -Destination $OutputPath
            $oldOutputMoved = $false
        }
        throw
    }

    if ($oldOutputMoved) {
        Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue
    }
    $exeCount = @(Get-ChildItem -LiteralPath $OutputPath -Filter '*.exe' -File).Count
    Write-PullLog "done: $exeCount executables staged in $OutputPath"
    Write-Host "staged $exeCount executables in $OutputPath"
} catch {
    Write-PullLog "failed: $($_.Exception.Message)"
    throw
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
