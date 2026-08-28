function New-LayerCBenchmarkContext {
    param(
        [string]$BuildDirectory,
        [string]$OutputPath,
        [Parameter(Mandatory)]
        [string]$OutputStem,
        [Parameter(Mandatory)]
        [string]$TemporaryStem
    )

    $repository = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
        $BuildDirectory = Join-Path $repository 'build\mingw-preset'
    }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $repository "out\benchmarks\$OutputStem-$stamp.json"
    }
    $temporary = Join-Path ([IO.Path]::GetTempPath()) "$TemporaryStem-$PID-$stamp"
    [pscustomobject]@{
        BuildDirectory = $BuildDirectory
        OutputPath = [IO.Path]::GetFullPath($OutputPath)
        Temporary = $temporary
        ProfileDirectory = Join-Path $temporary 'profiles'
        Utf8 = New-Object System.Text.UTF8Encoding($false)
    }
}

function Measure-LayerCPlateau {
    param(
        [Parameter(Mandatory)]
        [Diagnostics.Process]$Process,
        [Parameter(Mandatory)]
        [int]$Seconds,
        [int]$SampleIntervalMilliseconds = 250,
        [Parameter(Mandatory)]
        [string]$FailureMessage
    )

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
        Start-Sleep -Milliseconds $SampleIntervalMilliseconds
    }
    if ($Process.HasExited -or $private.Count -eq 0) {
        throw $FailureMessage
    }
    $Process.Refresh()
    $elapsedMs = [Math]::Max(1.0, $clock.Elapsed.TotalMilliseconds)
    $cpuMs = $Process.TotalProcessorTime.TotalMilliseconds - $startCpu
    $sortedPrivate = @($private | Sort-Object)
    $sortedWorking = @($working | Sort-Object)
    $middle = [int][Math]::Floor($sortedPrivate.Count / 2)
    [ordered]@{
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

function Set-BenchmarkForeground {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Target,
        [Parameter(Mandatory = $true)][object]$Activator,
        [Parameter(Mandatory = $true)][type]$Native
    )
    $Target.Refresh()
    $window = $Target.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { return $false }

    [void]$Native::ShowWindowAsync($window, 9) # SW_RESTORE
    [void]$Activator.AppActivate($Target.Id)

    $currentThread = $Native::GetCurrentThreadId()
    $foreground = $Native::GetForegroundWindow()
    [uint32]$ignoredProcess = 0
    $foregroundThread = if ($foreground -ne [IntPtr]::Zero) {
        $Native::GetWindowThreadProcessId(
            $foreground, [ref]$ignoredProcess)
    } else { 0 }
    [uint32]$targetProcess = 0
    $targetThread = $Native::GetWindowThreadProcessId(
        $window, [ref]$targetProcess)

    $attachedForeground = $foregroundThread -ne 0 -and
        $foregroundThread -ne $currentThread -and
        $Native::AttachThreadInput(
            $currentThread, $foregroundThread, $true)
    $attachedTarget = $targetThread -ne 0 -and
        $targetThread -ne $currentThread -and
        $Native::AttachThreadInput(
            $currentThread, $targetThread, $true)
    try {
        [void]$Native::BringWindowToTop($window)
        [void]$Native::SetForegroundWindow($window)
    } finally {
        if ($attachedTarget) {
            [void]$Native::AttachThreadInput(
                $currentThread, $targetThread, $false)
        }
        if ($attachedForeground) {
            [void]$Native::AttachThreadInput(
                $currentThread, $foregroundThread, $false)
        }
    }

    $foregroundRoot = $Native::GetAncestor(
        $Native::GetForegroundWindow(), 2) # GA_ROOT
    $targetRoot = $Native::GetAncestor($window, 2)
    return $foregroundRoot -eq $targetRoot
}
