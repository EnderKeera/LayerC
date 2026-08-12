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
$repository = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repository 'build\mingw-preset'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
foreach ($name in @('layerd.exe', 'layere.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name))) {
        throw "Missing $name in $BuildDirectory. Build Release first."
    }
}
if (Get-Process -Name layerd, layere -ErrorAction SilentlyContinue) {
    throw 'Close existing LayerD and LayerE processes before running the isolated V2.2 benchmark.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputDirectory = Join-Path $repository 'out\benchmarks'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $OutputPath = Join-Path $outputDirectory "v22-modular-idle-$stamp.json"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) "LayerC-v22-benchmark-$PID-$stamp"
$profileDirectory = Join-Path $temporary 'profiles'
[IO.Directory]::CreateDirectory($profileDirectory) | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Measure-Plateau([Diagnostics.Process]$Process, [int]$Seconds) {
    $private = [Collections.Generic.List[long]]::new()
    $working = [Collections.Generic.List[long]]::new()
    [int]$maxThreads = 0
    [int]$maxHandles = 0
    $Process.Refresh()
    $startCpu = $Process.TotalProcessorTime.TotalMilliseconds
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds -and -not $Process.HasExited) {
        $Process.Refresh()
        $private.Add($Process.PrivateMemorySize64)
        $working.Add($Process.WorkingSet64)
        $maxThreads = [Math]::Max($maxThreads, $Process.Threads.Count)
        $maxHandles = [Math]::Max($maxHandles, $Process.HandleCount)
        Start-Sleep -Milliseconds 250
    }
    if ($Process.HasExited -or $private.Count -eq 0) {
        throw 'LayerD exited during the V2.2 resource measurement.'
    }
    $Process.Refresh()
    $elapsedMs = [Math]::Max(1.0, $clock.Elapsed.TotalMilliseconds)
    $cpuMs = $Process.TotalProcessorTime.TotalMilliseconds - $startCpu
    $sortedPrivate = @($private | Sort-Object)
    $sortedWorking = @($working | Sort-Object)
    $middle = [int][Math]::Floor($sortedPrivate.Count / 2)
    return [ordered]@{
        durationSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
        averageCpuPercent = [Math]::Round(
            $cpuMs / $elapsedMs / [Environment]::ProcessorCount * 100.0, 3)
        medianPrivateMiB = [Math]::Round($sortedPrivate[$middle] / 1MB, 3)
        maxPrivateMiB = [Math]::Round(($sortedPrivate | Measure-Object -Maximum).Maximum / 1MB, 3)
        medianWorkingSetMiB = [Math]::Round($sortedWorking[$middle] / 1MB, 3)
        maxWorkingSetMiB = [Math]::Round(($sortedWorking | Measure-Object -Maximum).Maximum / 1MB, 3)
        maxThreadCount = $maxThreads
        maxHandleCount = $maxHandles
    }
}

$oldProfileDirectory = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
$daemon = $null
$editor = $null
try {
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $profileDirectory, 'Process')
    $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds $WarmupSeconds
    if ($daemon.HasExited) { throw "LayerD exited during warmup with code $($daemon.ExitCode)." }
    $baseline = Measure-Plateau $daemon $DurationSeconds

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
    $editorOpen = Measure-Plateau $daemon $DurationSeconds

    [void]$editor.CloseMainWindow()
    if (-not $editor.WaitForExit(5000)) {
        Stop-Process -Id $editor.Id -Force -ErrorAction SilentlyContinue
        [void]$editor.WaitForExit(5000)
    }
    Start-Sleep -Seconds $WarmupSeconds
    $afterClose = Measure-Plateau $daemon $DurationSeconds

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
    $result = [ordered]@{
        schemaVersion = 1
        packageVersion = '2.2.0'
        measuredAt = (Get-Date).ToUniversalTime().ToString('o')
        host = [ordered]@{
            computerName = $env:COMPUTERNAME
            os = [Environment]::OSVersion.VersionString
            logicalProcessors = [Environment]::ProcessorCount
        }
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
        thresholds = $thresholds
        passed = $passed
        note = 'Empty profile directory. LayerE is opened without edit mode; RuntimeService demand-matrix tests verify no visual, template, HID, edit-overlay, timer, or worker ownership.'
    }
    [IO.File]::WriteAllText($OutputPath, ($result | ConvertTo-Json -Depth 7), $utf8)
    $result | ConvertTo-Json -Depth 7
    if (-not $passed) { exit 1 }
} finally {
    if ($null -ne $editor -and -not $editor.HasExited) {
        Stop-Process -Id $editor.Id -Force -ErrorAction SilentlyContinue
        [void]$editor.WaitForExit(5000)
    }
    if ($null -ne $daemon -and -not $daemon.HasExited) {
        Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue
        [void]$daemon.WaitForExit(5000)
    }
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $oldProfileDirectory, 'Process')
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
