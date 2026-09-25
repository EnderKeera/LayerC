#Requires -Version 5.1

$ErrorActionPreference = 'Stop'

if (-not ('LayerCBenchmarkNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class LayerCBenchmarkNative {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@
}

function Assert-NativeSuccess {
    param([Parameter(Mandatory)][string]$Operation)
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE."
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory)][string]$Operation,
        [Parameter(Mandatory)][scriptblock]$Command
    )
    & $Command
    Assert-NativeSuccess $Operation
}

function Resolve-InstallRoot {
    <#.SYNOPSIS
        Resolves the directory that directly contains a staged LayerC install.
        Accepts either the install root itself or its parent when the archive
        extracted into a single nested folder.#>
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string[]]$RequiredExecutables
    )

    $Path = [IO.Path]::GetFullPath($Path)
    $missing = @($RequiredExecutables | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Path $_) -PathType Leaf)
    })
    if ($missing.Count -eq 0) {
        return $Path
    }
    $candidates = @(
        Get-ChildItem -LiteralPath $Path -Directory | Where-Object {
            $dir = $_.FullName
            @( $RequiredExecutables | Where-Object {
                -not (Test-Path -LiteralPath (Join-Path $dir $_) -PathType Leaf)
            }).Count -eq 0
        }
    )
    if ($candidates.Count -ne 1) {
        throw ("Expected one build root containing {0} under {1}; found {2}." -f
            ($RequiredExecutables -join ', '), $Path, $candidates.Count)
    }
    return $candidates[0].FullName
}

function Resolve-LayerCBuildRoot {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string[]]$RequiredExecutables
    )
    Resolve-InstallRoot -Path $Path -RequiredExecutables $RequiredExecutables
}

function New-LayerCBenchmarkContext {
    param(
        [string]$BuildDirectory,
        [string]$OutputPath,
        [Parameter(Mandatory)][string]$OutputStem,
        [Parameter(Mandatory)][string]$TemporaryStem,
        [string]$DefaultBuildDirectory = 'build\mingw-preset'
    )

    $repository = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
        $BuildDirectory = Join-Path $repository $DefaultBuildDirectory
    }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $repository "out\benchmarks\$OutputStem-$stamp.json"
    }
    $temporary = Join-Path ([IO.Path]::GetTempPath()) "$TemporaryStem-$PID-$stamp"
    [pscustomobject]@{
        Repository = $repository
        BuildDirectory = $BuildDirectory
        OutputPath = [IO.Path]::GetFullPath($OutputPath)
        Temporary = $temporary
        ProfileDirectory = Join-Path $temporary 'profiles'
        Utf8 = New-Object System.Text.UTF8Encoding($false)
    }
}

function Assert-LayerCQuiescent {
    param(
        [Parameter(Mandatory)][string[]]$ProcessNames,
        [Parameter(Mandatory)][string]$FailureMessage
    )
    $owners = @(Get-Process -Name $ProcessNames -ErrorAction SilentlyContinue)
    if ($owners.Count -ne 0) {
        $running = ($owners | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ', '
        throw "$FailureMessage (running: $running)"
    }
}

function Enter-LayerCProfileSandbox {
    param([Parameter(Mandatory)][string]$ProfileDirectory)
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $ProfileDirectory, 'Process')
}

function Exit-LayerCProfileSandbox {
    param(
        # PowerShell coerces $null to '' for [string] parameters.
        [AllowNull()][object]$PreviousDirectory,
        [Parameter(Mandatory)][string]$TemporaryDirectory,
        [switch]$KeepTemporary
    )
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $PreviousDirectory, 'Process')
    if (-not $KeepTemporary -and (Test-Path -LiteralPath $TemporaryDirectory)) {
        Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force
    }
}

function Start-LayerCDaemon {
    param([Parameter(Mandatory)][string]$BuildDirectory)
    Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
}

function Stop-LayerCBenchmarkProcess {
    param([Diagnostics.Process]$Process)
    if ($null -ne $Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        [void]$Process.WaitForExit(5000)
    }
}

function Get-LayerCPackageVersion {
    $cmake = Get-Content -LiteralPath (
        Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(LayerC\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not read the LayerC package version from CMakeLists.txt.'
    }
    return $Matches[1]
}

function Get-LayerCCommitSha {
    if ($env:GITHUB_SHA -match '^[0-9a-fA-F]{40}$') {
        return $env:GITHUB_SHA.ToLowerInvariant()
    }
    $repository = Split-Path -Parent $PSScriptRoot
    $sha = (& git -C $repository rev-parse --verify HEAD).Trim()
    Assert-NativeSuccess 'Resolving the benchmark commit'
    if ($sha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "Git returned an invalid benchmark commit: $sha"
    }
    return $sha.ToLowerInvariant()
}

function New-LayerCBenchmarkResult {
    param(
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][Collections.IDictionary]$Data,
        [Parameter(Mandatory)][Collections.IDictionary]$Thresholds,
        [Parameter(Mandatory)][bool]$Passed,
        [Collections.IDictionary]$HostData = [ordered]@{},
        [ValidateRange(2, 100)][int]$Depth = 8
    )

    $hostInfo = [ordered]@{
        computerName = $env:COMPUTERNAME
        os = [Environment]::OSVersion.VersionString
        logicalProcessors = [Environment]::ProcessorCount
    }
    foreach ($entry in $HostData.GetEnumerator()) {
        $hostInfo[$entry.Key] = $entry.Value
    }
    $result = [ordered]@{
        schemaVersion = 1
        packageVersion = Get-LayerCPackageVersion
        commitSha = Get-LayerCCommitSha
        measuredAt = (Get-Date).ToUniversalTime().ToString('o')
        host = $hostInfo
    }
    foreach ($entry in $Data.GetEnumerator()) {
        if ($result.Contains($entry.Key)) {
            throw "Benchmark data cannot replace identity field '$($entry.Key)'."
        }
        $result[$entry.Key] = $entry.Value
    }
    $result.thresholds = $Thresholds
    $result.passed = $Passed

    [IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText(
        $OutputPath, ($result | ConvertTo-Json -Depth $Depth), $utf8)
    return $result
}

function Measure-LayerCPlateau {
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [Parameter(Mandatory)][int]$Seconds,
        [int]$SampleIntervalMilliseconds = 250,
        [Parameter(Mandatory)][string]$FailureMessage
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
        maxPrivateMiB = [Math]::Round(
            ($sortedPrivate | Measure-Object -Maximum).Maximum / 1MB, 3)
        medianWorkingSetMiB = [Math]::Round($sortedWorking[$middle] / 1MB, 3)
        maxWorkingSetMiB = [Math]::Round(
            ($sortedWorking | Measure-Object -Maximum).Maximum / 1MB, 3)
        maxThreadCount = $maxThreads
        maxHandleCount = $maxHandles
    }
}

function Set-BenchmarkForeground {
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$Target,
        [Parameter(Mandatory)][object]$Activator
    )
    $Target.Refresh()
    $window = $Target.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { return $false }

    [void][LayerCBenchmarkNative]::ShowWindowAsync($window, 9)
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
        [LayerCBenchmarkNative]::GetForegroundWindow(), 2)
    $targetRoot = [LayerCBenchmarkNative]::GetAncestor($window, 2)
    return $foregroundRoot -eq $targetRoot
}

Export-ModuleMember -Function Assert-NativeSuccess, Invoke-Checked,
    Resolve-InstallRoot, Resolve-LayerCBuildRoot, New-LayerCBenchmarkContext,
    Assert-LayerCQuiescent, Enter-LayerCProfileSandbox, Exit-LayerCProfileSandbox,
    Start-LayerCDaemon, Stop-LayerCBenchmarkProcess, Get-LayerCPackageVersion,
    Get-LayerCCommitSha, New-LayerCBenchmarkResult, Measure-LayerCPlateau,
    Set-BenchmarkForeground
