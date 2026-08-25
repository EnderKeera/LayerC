#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Repository = '',
    [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptRoot
if ([string]::IsNullOrWhiteSpace($Repository)) {
    $Repository = gh repo view --json nameWithOwner -q .nameWithOwner
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repositoryRoot 'out\layerc'
}
$staging = Join-Path $repositoryRoot 'out\pulldown'
New-Item -ItemType Directory -Force $staging, $OutputPath | Out-Null
$log = Join-Path $staging 'pull.log'

[IO.File]::AppendAllText(
    $log,
    ("[{0}] pulling rolling build from {1}" -f (Get-Date -Format s), $Repository) +
    [Environment]::NewLine)
gh release download rolling --repo $Repository --dir $staging --clobber |
    Out-Null
$extract = Join-Path $staging 'extracted'
New-Item -ItemType Directory -Force $extract | Out-Null
Get-ChildItem -Path $staging -Filter '*.zip' | ForEach-Object {
    Expand-Archive -LiteralPath $_.FullName -DestinationPath $extract -Force
}
Get-ChildItem -Path $extract -Directory | ForEach-Object {
    Copy-Item -Path (Join-Path $_.FullName '*') -Destination $OutputPath `
        -Recurse -Force
}
$exeCount = @(Get-ChildItem -Path $OutputPath -Filter '*.exe').Count
[IO.File]::AppendAllText(
    $log,
    ("[{0}] done: {1} executables staged in {2}" -f
        (Get-Date -Format s), $exeCount, $OutputPath) +
    [Environment]::NewLine)
