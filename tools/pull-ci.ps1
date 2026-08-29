#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repository = '',
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptRoot

function Assert-NativeSuccess {
    param([Parameter(Mandatory = $true)][string]$Operation)
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

function Test-InstallRoot {
    param([Parameter(Mandatory = $true)][string]$Path)
    foreach ($name in @('layerd.exe', 'layere.exe', 'layercal.exe', 'layerw.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

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
    Expand-Archive -LiteralPath $archives[0].FullName -DestinationPath $extract

    $installRoot = $null
    if (Test-InstallRoot $extract) {
        $installRoot = $extract
    } else {
        $candidates = @(
            Get-ChildItem -LiteralPath $extract -Directory |
                Where-Object { Test-InstallRoot $_.FullName }
        )
        if ($candidates.Count -ne 1) {
            throw "Expected one LayerC install root, found $($candidates.Count)."
        }
        $installRoot = $candidates[0].FullName
    }

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
