#Requires -Version 7.3
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../linux-gui/Lifecycle.psm1') -Force

$root = Join-Path ([IO.Path]::GetTempPath()) ('layerc-gui-completion-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$job = $null
$cases = 0
function Assert-True([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
}
function Assert-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert-True $failed 'Expected an incomplete/corrupt publication to be rejected.'
}
function New-Fixture([bool]$Passed = $true) {
    $path = Join-Path $root ([Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $path
    [IO.File]::WriteAllText((Join-Path $path 'result.json'),
        (@{ mode = 'session'; passed = $Passed } | ConvertTo-Json))
    [IO.File]::WriteAllText((Join-Path $path 'SHA256SUMS'), "fixture manifest`n")
    return $path
}
function New-Completion([string]$Path, [int]$Code = 0) {
    return (@{ version = 1; exitCode = $Code
        resultSha256 = (Get-FileHash -LiteralPath (Join-Path $Path 'result.json')).Hash
        manifestSha256 = (Get-FileHash -LiteralPath (Join-Path $Path 'SHA256SUMS')).Hash
    } | ConvertTo-Json)
}
function Publish([string]$Path, [int]$Code = 0) {
    $temp = Join-Path $Path '.runner-complete.tmp'
    [IO.File]::WriteAllText($temp, (New-Completion $Path $Code))
    Move-Item -LiteralPath $temp -Destination (Join-Path $Path 'runner-complete.json')
}

try {
    $path = New-Fixture; Publish $path
    Assert-True (Wait-LayerCGuiResult -Directory $path -Milliseconds 100).passed 'Completed evidence passes.'
    $cases++

    $path = New-Fixture
    $payload = New-Completion $path
    $job = Start-Job -ArgumentList $path, $payload -ScriptBlock {
        param($Path, $Payload)
        Start-Sleep -Milliseconds 200
        $temp = Join-Path $Path '.runner-complete.tmp'
        [IO.File]::WriteAllText($temp, $Payload)
        Move-Item -LiteralPath $temp -Destination (Join-Path $Path 'runner-complete.json')
    }
    Assert-True (Wait-LayerCGuiResult -Directory $path -Milliseconds 10000).passed 'Delayed Linux publication is awaited.'
    $null = Wait-Job $job -Timeout 10
    Assert-True ($job.State -eq 'Completed') 'The fixture publisher is joined.'
    Receive-Job $job
    Remove-Job $job; $job = $null
    $cases++

    $path = New-Fixture
    Assert-Failure { Wait-LayerCGuiResult -Directory $path -Milliseconds 50 }
    Assert-True (Test-Path -LiteralPath (Join-Path $path 'result.json')) 'Timeout preserves incomplete evidence.'
    $cases++

    $path = New-Fixture; Publish $path
    [IO.File]::AppendAllText((Join-Path $path 'result.json'), ' ')
    Assert-Failure { Wait-LayerCGuiResult -Directory $path -Milliseconds 50 }
    $cases++

    $path = New-Fixture; Publish $path
    [IO.File]::AppendAllText((Join-Path $path 'SHA256SUMS'), 'changed')
    Assert-Failure { Wait-LayerCGuiResult -Directory $path -Milliseconds 50 }
    $cases++

    $path = New-Fixture
    [IO.File]::WriteAllText((Join-Path $path 'runner-complete.json'), '{}')
    Assert-Failure { Wait-LayerCGuiResult -Directory $path -Milliseconds 50 }
    $cases++

    $path = New-Fixture $false; Publish $path 1
    Assert-True (-not (Wait-LayerCGuiResult -Directory $path -Milliseconds 50).passed) 'A genuine failed run stays failed.'
    $cases++

    $path = New-Fixture; Publish $path 1
    Assert-Failure { Wait-LayerCGuiResult -Directory $path -Milliseconds 50 }
    $cases++
    Write-Host "$cases/8 GUI completion publication cases passed."
} finally {
    if ($job) {
        Stop-Job $job -ErrorAction SilentlyContinue
        Remove-Job $job -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $root -Recurse -Force
}
