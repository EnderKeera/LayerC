#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [ValidateRange(2, 60)]
    [int]$WarmupSeconds = 5,
    [ValidateRange(5, 120)]
    [int]$DurationSeconds = 10,
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force
$context = New-LayerCBenchmarkContext -BuildDirectory $BuildDirectory `
    -OutputPath $OutputPath -OutputStem 'v22-modular-idle' `
    -TemporaryStem 'LayerC-v22-benchmark'
$BuildDirectory = $context.BuildDirectory
$OutputPath = $context.OutputPath
$temporary = $context.Temporary
$profileDirectory = $context.ProfileDirectory
$utf8 = $context.Utf8
foreach ($name in @('layerd.exe', 'layere.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name))) {
        throw "Missing $name in $BuildDirectory. Build Release first."
    }
}
Assert-LayerCQuiescent -ProcessNames layerd, layere `
    -FailureMessage 'Close existing LayerD and LayerE processes before running the isolated V2.2 benchmark.'
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
[IO.Directory]::CreateDirectory($profileDirectory) | Out-Null
$measure = @{
    Seconds = $DurationSeconds
    FailureMessage = 'LayerD exited during the V2.2 resource measurement.'
}

$oldProfileDirectory = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
$daemon = $null
$editor = $null
try {
    Enter-LayerCProfileSandbox -ProfileDirectory $profileDirectory
    $daemon = Start-LayerCDaemon -BuildDirectory $BuildDirectory
    Start-Sleep -Seconds $WarmupSeconds
    if ($daemon.HasExited) { throw "LayerD exited during warmup with code $($daemon.ExitCode)." }
    $baseline = Measure-LayerCPlateau -Process $daemon @measure

    $editor = Start-Process -FilePath (Join-Path $BuildDirectory 'layere.exe') `
        -WorkingDirectory $BuildDirectory -PassThru
    for ($attempt = 0; $attempt -lt 80; ++$attempt) {
        Start-Sleep -Milliseconds 100
        $editor.Refresh()
        if ($editor.HasExited) { throw "LayerE exited during connection with code $($editor.ExitCode)." }
        if ($editor.MainWindowHandle -ne 0) { break }
    }
    if ($editor.MainWindowHandle -eq 0) { throw 'LayerE did not create its main window.' }
    Start-Sleep -Seconds $WarmupSeconds
    $editorOpen = Measure-LayerCPlateau -Process $daemon @measure

    [void]$editor.CloseMainWindow()
    if (-not $editor.WaitForExit(5000)) {
        Stop-Process -Id $editor.Id -Force -ErrorAction SilentlyContinue
        [void]$editor.WaitForExit(5000)
    }
    Start-Sleep -Seconds $WarmupSeconds
    $afterClose = Measure-LayerCPlateau -Process $daemon @measure

    $privateDelta = $editorOpen.medianPrivateMiB - $baseline.medianPrivateMiB
    $workingDelta = $editorOpen.medianWorkingSetMiB - $baseline.medianWorkingSetMiB
    $threadDelta = $editorOpen.maxThreadCount - $baseline.maxThreadCount
    $recoveredPrivateDelta = $afterClose.medianPrivateMiB - $baseline.medianPrivateMiB
    $recoveredWorkingDelta = $afterClose.medianWorkingSetMiB - $baseline.medianWorkingSetMiB
    $thresholds = [ordered]@{
        noGameMaxPrivateMiB = 12.0
        noGameMaxWorkingSetMiB = 20.0
        noGameMaxAverageCpuPercent = 0.2
        editorMaxPrivateDeltaMiB = 1.0
        editorMaxWorkingSetDeltaMiB = 2.0
        editorMaxThreadDelta = 0
        closeMaxPrivateDeltaMiB = 1.0
        closeMaxWorkingSetDeltaMiB = 2.0
    }
    $passed = $baseline.maxPrivateMiB -le $thresholds.noGameMaxPrivateMiB -and
        $baseline.maxWorkingSetMiB -le $thresholds.noGameMaxWorkingSetMiB -and
        $baseline.averageCpuPercent -le $thresholds.noGameMaxAverageCpuPercent -and
        $privateDelta -le $thresholds.editorMaxPrivateDeltaMiB -and
        $workingDelta -le $thresholds.editorMaxWorkingSetDeltaMiB -and
        $threadDelta -le $thresholds.editorMaxThreadDelta -and
        $recoveredPrivateDelta -le $thresholds.closeMaxPrivateDeltaMiB -and
        $recoveredWorkingDelta -le $thresholds.closeMaxWorkingSetDeltaMiB
    $data = [ordered]@{
        buildDirectory = $BuildDirectory
        baseline = $baseline
        editorOpen = $editorOpen
        afterEditorClose = $afterClose
        deltas = [ordered]@{
            editorPrivateMiB = [Math]::Round($privateDelta, 3)
            editorWorkingSetMiB = [Math]::Round($workingDelta, 3)
            editorThreads = $threadDelta
            recoveredPrivateMiB = [Math]::Round($recoveredPrivateDelta, 3)
            recoveredWorkingSetMiB = [Math]::Round($recoveredWorkingDelta, 3)
        }
        note = 'Empty profile directory. LayerE is opened without edit mode; RuntimeService demand-matrix tests verify no visual, template, HID, edit-overlay, timer, or worker ownership.'
    }
    $result = New-LayerCBenchmarkResult -OutputPath $OutputPath -Data $data `
        -Thresholds $thresholds -Passed $passed -Depth 7
    $result | ConvertTo-Json -Depth 7
    if (-not $passed) { exit 1 }
} finally {
    Stop-LayerCBenchmarkProcess -Process $editor
    Stop-LayerCBenchmarkProcess -Process $daemon
    Exit-LayerCProfileSandbox -PreviousDirectory $oldProfileDirectory `
        -TemporaryDirectory $temporary
}
