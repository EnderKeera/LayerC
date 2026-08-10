#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('low', 'balanced')]
    [string]$Quality = 'balanced',
    [string]$BuildDirectory = '',
    [ValidateRange(2, 120)]
    [int]$WarmupSeconds = 8,
    [ValidateRange(5, 300)]
    [int]$DurationSeconds = 20,
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'

if (-not ('LayerCBenchmarkNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class LayerCBenchmarkNative {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@
}

function Set-BenchmarkForeground(
    [System.Diagnostics.Process]$Target,
    [object]$Activator) {
    $Target.Refresh()
    $window = $Target.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { return $false }

    [void][LayerCBenchmarkNative]::ShowWindowAsync($window, 9) # SW_RESTORE
    [void]$Activator.AppActivate($Target.Id)

    $currentThread = [LayerCBenchmarkNative]::GetCurrentThreadId()
    $foreground = [LayerCBenchmarkNative]::GetForegroundWindow()
    [uint32]$ignoredProcess = 0
    $foregroundThread = if ($foreground -ne [IntPtr]::Zero) {
        [LayerCBenchmarkNative]::GetWindowThreadProcessId(
            $foreground, [ref]$ignoredProcess)
    } else { 0 }
    [uint32]$targetProcess = 0
    $targetThread = [LayerCBenchmarkNative]::GetWindowThreadProcessId(
        $window, [ref]$targetProcess)

    $attachedForeground = $foregroundThread -ne 0 -and
        $foregroundThread -ne $currentThread -and
        [LayerCBenchmarkNative]::AttachThreadInput(
            $currentThread, $foregroundThread, $true)
    $attachedTarget = $targetThread -ne 0 -and
        $targetThread -ne $currentThread -and
        [LayerCBenchmarkNative]::AttachThreadInput(
            $currentThread, $targetThread, $true)
    try {
        [void][LayerCBenchmarkNative]::BringWindowToTop($window)
        [void][LayerCBenchmarkNative]::SetForegroundWindow($window)
    } finally {
        if ($attachedTarget) {
            [void][LayerCBenchmarkNative]::AttachThreadInput(
                $currentThread, $targetThread, $false)
        }
        if ($attachedForeground) {
            [void][LayerCBenchmarkNative]::AttachThreadInput(
                $currentThread, $foregroundThread, $false)
        }
    }

    $foregroundRoot = [LayerCBenchmarkNative]::GetAncestor(
        [LayerCBenchmarkNative]::GetForegroundWindow(), 2) # GA_ROOT
    $targetRoot = [LayerCBenchmarkNative]::GetAncestor($window, 2)
    return $foregroundRoot -eq $targetRoot
}

$repository = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repository 'build\vs2026\Release'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$required = @('layerd.exe', 'lctest.exe', 'layerc_capture_probe.exe')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name))) {
        throw "Missing $name in $BuildDirectory. Build Release with tests enabled first."
    }
}
if (Get-Process -Name layerd -ErrorAction SilentlyContinue) {
    throw 'Close the existing LayerD instance before running an isolated benchmark.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputDirectory = Join-Path $repository 'out\benchmarks'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $OutputPath = Join-Path $outputDirectory "v11-$Quality-$stamp.json"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null

$temporary = Join-Path ([IO.Path]::GetTempPath()) "LayerC-v11-benchmark-$PID-$stamp"
$profileDirectory = Join-Path $temporary 'profiles'
$visualDirectory = Join-Path $profileDirectory '_visual'
[IO.Directory]::CreateDirectory($visualDirectory) | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText(
    (Join-Path $profileDirectory 'lctest.json'),
    [IO.File]::ReadAllText((Join-Path $repository 'examples\lctest.json')),
    $utf8)
$visual = @"
{
  "schemaVersion": 1,
  "profileId": "lctest",
  "sampleRateHz": 15,
  "quality": "$Quality",
  "rules": [
    {
      "id": "benchmark-ready",
      "name": "Benchmark Ready",
      "region": {"x": 0, "y": 0, "width": 1, "height": 1},
      "predicate": {"type": "brightness", "comparison": "below", "threshold": 250},
      "stableSamples": 2,
      "repeat": "edge"
    }
  ],
  "triggers": [
    {
      "cooldownId": "dash",
      "ruleId": "benchmark-ready",
      "enabled": true,
      "mode": "pixelOnly",
      "keepInput": false,
      "onUnavailable": "ignore"
    }
  ]
}
"@
[IO.File]::WriteAllText((Join-Path $visualDirectory 'lctest.json'), $visual, $utf8)

function Get-GpuProcessMemoryBytes([int]$ProcessId) {
    try {
        $samples = (Get-Counter '\GPU Process Memory(*)\Local Usage' -ErrorAction Stop).CounterSamples
        $sum = ($samples | Where-Object {
            $_.Path -match "pid_$ProcessId(?:_|\))"
        } | Measure-Object -Property CookedValue -Sum).Sum
        if ($null -eq $sum) { return $null }
        return [int64]$sum
    } catch {
        return $null
    }
}

function Measure-LayerProcess([Diagnostics.Process]$Process, [int]$Seconds) {
    $Process.Refresh()
    $startCpu = $Process.TotalProcessorTime.TotalMilliseconds
    $clock = [Diagnostics.Stopwatch]::StartNew()
    [int64]$maxPrivate = 0
    [int64]$maxWorking = 0
    [int]$maxThreads = 0
    [int64]$maxGpuLocal = 0
    $gpuSeen = $false
    $nextGpuSample = 0.0
    while ($clock.Elapsed.TotalSeconds -lt $Seconds -and -not $Process.HasExited) {
        $Process.Refresh()
        $maxPrivate = [Math]::Max($maxPrivate, $Process.PrivateMemorySize64)
        $maxWorking = [Math]::Max($maxWorking, $Process.WorkingSet64)
        $maxThreads = [Math]::Max($maxThreads, $Process.Threads.Count)
        if ($clock.Elapsed.TotalSeconds -ge $nextGpuSample) {
            $gpu = Get-GpuProcessMemoryBytes $Process.Id
            if ($null -ne $gpu) {
                $gpuSeen = $true
                $maxGpuLocal = [Math]::Max($maxGpuLocal, $gpu)
            }
            $nextGpuSample += 1.0
        }
        Start-Sleep -Milliseconds 250
    }
    $Process.Refresh()
    $elapsedMs = [Math]::Max(1.0, $clock.Elapsed.TotalMilliseconds)
    $cpuMs = $Process.TotalProcessorTime.TotalMilliseconds - $startCpu
    $cpuPercent = $cpuMs / $elapsedMs / [Environment]::ProcessorCount * 100.0
    return [ordered]@{
        processId = $Process.Id
        durationSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
        averageCpuPercent = [Math]::Round($cpuPercent, 3)
        maxPrivateMiB = [Math]::Round($maxPrivate / 1MB, 3)
        maxWorkingSetMiB = [Math]::Round($maxWorking / 1MB, 3)
        maxThreadCount = $maxThreads
        maxGpuLocalMiB = if ($gpuSeen) { [Math]::Round($maxGpuLocal / 1MB, 3) } else { $null }
    }
}

$oldProfileDirectory = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
$settingsRegistryPath = 'HKCU:\Software\LayerC'
$visualSettingName = 'VisualRecognitionEnabled'
$visualSettingWasPresent = $false
$visualSettingTouched = $false
$oldVisualSetting = $null
$game = $null
$daemon = $null
$captureEvent = $null
try {
    try {
        $oldVisualSetting = (Get-ItemProperty -LiteralPath $settingsRegistryPath -Name $visualSettingName -ErrorAction Stop).$visualSettingName
        $visualSettingWasPresent = $true
    } catch {
        $visualSettingWasPresent = $false
    }
    [void](New-Item -Path $settingsRegistryPath -Force)
    Set-ItemProperty -LiteralPath $settingsRegistryPath -Name $visualSettingName -Type DWord -Value 1
    $visualSettingTouched = $true
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $profileDirectory, 'Process')
    $game = Start-Process -FilePath (Join-Path $BuildDirectory 'lctest.exe') -PassThru
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        Start-Sleep -Milliseconds 100
        $game.Refresh()
        if ($game.MainWindowHandle -ne 0) { break }
    }
    if ($game.MainWindowHandle -eq 0) { throw 'lctest did not create its window.' }
    $activator = New-Object -ComObject WScript.Shell
    [void](Set-BenchmarkForeground $game $activator)
    Start-Sleep -Milliseconds 300

    $probeOutput = Join-Path $temporary 'probe.json'
    $probeError = Join-Path $temporary 'probe.err'
    $probe = Start-Process -FilePath (Join-Path $BuildDirectory 'layerc_capture_probe.exe') `
        -ArgumentList @('--hwnd', [string][uint64]$game.MainWindowHandle.ToInt64(), '--quality', $Quality) `
        -RedirectStandardOutput $probeOutput -RedirectStandardError $probeError `
        -WindowStyle Hidden -PassThru
    # Windows PowerShell 5 can leave ExitCode unset for a redirected
    # Start-Process unless its native handle is materialized before waiting.
    $probeHandle = $probe.Handle
    [void](Set-BenchmarkForeground $game $activator)
    $probe.WaitForExit()
    $probe.Refresh()
    $probeExitCode = $probe.ExitCode
    $probeText = if (Test-Path -LiteralPath $probeOutput) {
        [IO.File]::ReadAllText($probeOutput).Trim()
    } else { '' }
    if ($probeExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($probeText)) {
        $details = if (Test-Path -LiteralPath $probeError) { [IO.File]::ReadAllText($probeError) } else { '' }
        throw "Hardware capture probe failed (exit $probeExitCode): $probeText $details"
    }
    $captureProbe = $probeText | ConvertFrom-Json

    $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
    [void]$activator.AppActivate($game.Id)
    $daemonCaptureVerified = $false
    for ($attempt = 0; $attempt -lt 50 -and -not $daemonCaptureVerified; ++$attempt) {
        if ($daemon.HasExited) {
            throw "LayerD exited before visual capture activated with code $($daemon.ExitCode)."
        }
        # Capture workers intentionally exist only for the foreground matched
        # client. Keep the fixture foreground while LayerD starts and performs
        # its first window scan; a one-shot AppActivate is racy with shell
        # focus changes during process startup.
        [void](Set-BenchmarkForeground $game $activator)
        if ($null -eq $captureEvent) {
            try {
                $captureEvent = [Threading.EventWaitHandle]::OpenExisting('Local\LayerC.LayerD.VisualCaptureActive')
            } catch [Threading.WaitHandleCannotBeOpenedException] {
            }
        }
        if ($null -ne $captureEvent) {
            $daemonCaptureVerified = $captureEvent.WaitOne(100)
        }
        if (-not $daemonCaptureVerified) { Start-Sleep -Milliseconds 100 }
    }
    if (-not $daemonCaptureVerified) {
        throw 'LayerD did not activate its visual capture worker; refusing an idle-process benchmark.'
    }
    Start-Sleep -Seconds $WarmupSeconds
    if ($daemon.HasExited) { throw "LayerD exited during warmup with code $($daemon.ExitCode)." }
    $measurement = Measure-LayerProcess $daemon $DurationSeconds
    if ($daemon.HasExited) {
        throw "LayerD exited during measurement with code $($daemon.ExitCode); refusing partial benchmark evidence."
    }
    $thresholds = [ordered]@{
        maxPrivateMiB = 35.0
        maxWorkingSetMiB = 50.0
        maxAverageCpuPercent = 3.0
    }
    $gatePassed = $measurement.maxPrivateMiB -le $thresholds.maxPrivateMiB -and
        $measurement.maxWorkingSetMiB -le $thresholds.maxWorkingSetMiB -and
        $measurement.averageCpuPercent -le $thresholds.maxAverageCpuPercent -and
        $daemonCaptureVerified
    $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
        [ordered]@{
            name = $_.Name
            driverVersion = $_.DriverVersion
            adapterRamMiB = if ($null -ne $_.AdapterRAM) { [Math]::Round([uint64]$_.AdapterRAM / 1MB, 1) } else { $null }
        }
    })
    $result = [ordered]@{
        schemaVersion = 1
        measuredAt = (Get-Date).ToUniversalTime().ToString('o')
        quality = $Quality
        sampleRateHz = 15
        host = [ordered]@{
            computerName = $env:COMPUTERNAME
            os = [Environment]::OSVersion.VersionString
            logicalProcessors = [Environment]::ProcessorCount
            gpuAdapters = $adapters
        }
        captureProbe = $captureProbe
        daemonCaptureVerified = $daemonCaptureVerified
        layerd = $measurement
        thresholds = $thresholds
        passed = $gatePassed
        note = 'Run both low and balanced on at least one integrated-GPU and one discrete-GPU system. Exact is intentionally outside this gate.'
    }
    [IO.File]::WriteAllText($OutputPath, ($result | ConvertTo-Json -Depth 8), $utf8)
    $result | ConvertTo-Json -Depth 8
    if (-not $gatePassed) { exit 1 }
} finally {
    if ($null -ne $captureEvent) { $captureEvent.Dispose() }
    if ($null -ne $daemon -and -not $daemon.HasExited) {
        Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue
        [void]$daemon.WaitForExit(5000)
    }
    if ($null -ne $game -and -not $game.HasExited) {
        Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
        [void]$game.WaitForExit(5000)
    }
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $oldProfileDirectory, 'Process')
    if ($visualSettingTouched) {
        if ($visualSettingWasPresent) {
            Set-ItemProperty -LiteralPath $settingsRegistryPath -Name $visualSettingName -Type DWord -Value ([int]$oldVisualSetting) -ErrorAction SilentlyContinue
        } else {
            Remove-ItemProperty -LiteralPath $settingsRegistryPath -Name $visualSettingName -ErrorAction SilentlyContinue
        }
    }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
