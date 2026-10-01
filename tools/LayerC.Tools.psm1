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

function Assert-LayerCExecutables {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string[]]$Names,
        [string]$Hint = ''
    )
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
        throw "Build directory does not exist: $Directory"
    }
    foreach ($name in $Names) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            throw ("Missing $name in $Directory. $Hint").TrimEnd()
        }
    }
}

function Resolve-LayerCBuildRoot {
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

# A sandbox also owns a disposable HKCU settings key, handed to every LayerC
# process started inside it through LAYERC_SETTINGS_KEY, so benchmarks never
# read or write the user's HKCU\Software\LayerC.
function Get-LayerCSandboxSettingsKey {
    param([Parameter(Mandatory)][string]$TemporaryDirectory)
    return 'Software\LayerC.Benchmark\' + (Split-Path -Leaf $TemporaryDirectory)
}

function Enter-LayerCProfileSandbox {
    param([Parameter(Mandatory)][string]$ProfileDirectory)
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $ProfileDirectory, 'Process')
    $key = Get-LayerCSandboxSettingsKey -TemporaryDirectory (Split-Path -Parent $ProfileDirectory)
    $script:PreviousSettingsKey = [Environment]::GetEnvironmentVariable('LAYERC_SETTINGS_KEY', 'Process')
    [void](New-Item -Path "HKCU:\$key" -Force)
    [Environment]::SetEnvironmentVariable('LAYERC_SETTINGS_KEY', $key, 'Process')
}

function Set-LayerCSandboxSetting {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][int]$Value
    )
    $key = [Environment]::GetEnvironmentVariable('LAYERC_SETTINGS_KEY', 'Process')
    if ($key -notlike 'Software\LayerC.Benchmark\*') {
        throw 'LayerC settings are written only inside a benchmark sandbox.'
    }
    Set-ItemProperty -LiteralPath "HKCU:\$key" -Name $Name -Type DWord -Value $Value
}

function Exit-LayerCProfileSandbox {
    param(
        # PowerShell coerces $null to '' for [string] parameters.
        [AllowNull()][object]$PreviousDirectory,
        [Parameter(Mandatory)][string]$TemporaryDirectory,
        [switch]$KeepTemporary
    )
    # PowerShell passes $null to .NET strings as '', which now sets an empty
    # variable instead of removing it; [NullString] removes it.
    foreach ($restore in @(@('LAYERC_PROFILE_DIR', $PreviousDirectory),
            @('LAYERC_SETTINGS_KEY', $script:PreviousSettingsKey))) {
        $value = if ($null -eq $restore[1]) { [NullString]::Value } else { [string]$restore[1] }
        [Environment]::SetEnvironmentVariable($restore[0], $value, 'Process')
    }
    $settings = 'HKCU:\' + (Get-LayerCSandboxSettingsKey -TemporaryDirectory $TemporaryDirectory)
    if (Test-Path -LiteralPath $settings) {
        Remove-Item -LiteralPath $settings -Recurse -Force
    }
    $parent = Split-Path -Parent $settings
    if ((Test-Path -LiteralPath $parent) -and -not (Get-ChildItem -LiteralPath $parent)) {
        Remove-Item -LiteralPath $parent -Force
    }
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

$script:BuildIdentityFile = 'layerc-build-identity.json'

function Get-LayerCCommitSha {
    $repository = Split-Path -Parent $PSScriptRoot
    $sha = (& git -C $repository rev-parse --verify HEAD).Trim()
    Assert-NativeSuccess 'Resolving the checked-out commit'
    if ($sha -notmatch '^[0-9a-fA-F]{40}$') {
        throw "Git returned an invalid commit: $sha"
    }
    return $sha.ToLowerInvariant()
}

# Written by the CI build next to its binaries and carried with them through
# artifacts, so measurements name the commit that was built rather than the
# workflow event that happened to run them.
function Write-LayerCBuildIdentity {
    param(
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][string]$Configuration
    )
    if ($env:GITHUB_RUN_ID -notmatch '^[0-9]+$' -or $env:GITHUB_RUN_ATTEMPT -notmatch '^[0-9]+$') {
        throw 'A build identity is recorded only by a GitHub Actions build.'
    }
    $repository = Split-Path -Parent $PSScriptRoot
    $changes = @(& git -C $repository status --porcelain --untracked-files=no)
    Assert-NativeSuccess 'Checking the built source tree'
    $identity = [ordered]@{
        schemaVersion = 1
        source = 'ci-artifact'
        commitSha = Get-LayerCCommitSha
        dirty = $changes.Count -ne 0
        workflow = $env:GITHUB_WORKFLOW
        runId = $env:GITHUB_RUN_ID
        runAttempt = $env:GITHUB_RUN_ATTEMPT
        configuration = $Configuration
    }
    $path = Join-Path $BuildDirectory $script:BuildIdentityFile
    [IO.File]::WriteAllText($path, ($identity | ConvertTo-Json) + "`n",
        (New-Object System.Text.UTF8Encoding($false)))
    return $identity
}

# The identity of the binaries in BuildDirectory. Without a CI identity file
# the binaries are a local build, recorded against the checkout (and whether
# it was dirty) without claiming they were verified to come from it.
function Get-LayerCBuildIdentity {
    param([Parameter(Mandatory)][string]$BuildDirectory)
    $path = Join-Path $BuildDirectory $script:BuildIdentityFile
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $file = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if ($file.source -ne 'ci-artifact' -or $file.commitSha -notmatch '^[0-9a-f]{40}$' -or
            [string]$file.runId -notmatch '^[0-9]+$') {
            throw "Invalid build identity: $path"
        }
        return [ordered]@{
            source = 'ci-artifact'
            commitSha = [string]$file.commitSha
            dirty = [bool]$file.dirty
            workflow = [string]$file.workflow
            runId = [string]$file.runId
            runAttempt = [string]$file.runAttempt
            configuration = [string]$file.configuration
        }
    }
    $repository = Split-Path -Parent $PSScriptRoot
    $changes = @(& git -C $repository status --porcelain --untracked-files=no)
    Assert-NativeSuccess 'Checking the local source tree'
    return [ordered]@{
        source = 'local-build'
        commitSha = Get-LayerCCommitSha
        dirty = $changes.Count -ne 0
        buildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    }
}

# Refuses to measure binaries whose recorded build is not the resolved
# candidate, or a checkout whose scripts are not that candidate's.
function Assert-LayerCBuildIdentity {
    param(
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][string]$CommitSha,
        [Parameter(Mandatory)][string]$RunId
    )
    $identity = Get-LayerCBuildIdentity -BuildDirectory $BuildDirectory
    $expected = $CommitSha.ToLowerInvariant()
    if ($identity.source -ne 'ci-artifact') {
        throw "$BuildDirectory has no CI build identity; refusing to attribute it to $expected."
    }
    if ($identity.commitSha -ne $expected -or $identity.runId -ne $RunId) {
        throw ("Downloaded binaries were built from $($identity.commitSha) in run " +
            "$($identity.runId), not candidate $expected in run $RunId.")
    }
    $checkout = Get-LayerCCommitSha
    if ($checkout -ne $expected) {
        throw "The checkout is $checkout, not candidate $expected."
    }
    return $identity
}

# Parameters for run-hardware-benchmarks.ps1, keyed by name. Splat the
# hashtable: an array splats positionally, so '-CandidateBuildDirectory' would
# bind as a value instead of naming the parameter.
function New-LayerCHardwareBenchmarkArguments {
    param(
        [Parameter(Mandatory)][string]$CandidateBuildDirectory,
        [string]$BaselineBuildDirectory = '',
        [switch]$VisualOnly
    )
    $arguments = @{ CandidateBuildDirectory = $CandidateBuildDirectory }
    if ($VisualOnly) {
        $arguments.VisualOnly = $true
    } elseif (-not [string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
        $arguments.BaselineBuildDirectory = $BaselineBuildDirectory
    }
    return $arguments
}

function New-LayerCBenchmarkResult {
    param(
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][string]$BuildDirectory,
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
    $build = Get-LayerCBuildIdentity -BuildDirectory $BuildDirectory
    $result = [ordered]@{
        schemaVersion = 1
        packageVersion = Get-LayerCPackageVersion
        commitSha = $build.commitSha
        build = $build
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

Export-ModuleMember -Function Assert-NativeSuccess, Assert-LayerCExecutables,
    Resolve-LayerCBuildRoot, New-LayerCBenchmarkContext,
    Assert-LayerCQuiescent, Enter-LayerCProfileSandbox, Exit-LayerCProfileSandbox,
    Set-LayerCSandboxSetting,
    Start-LayerCDaemon, Stop-LayerCBenchmarkProcess, Get-LayerCPackageVersion,
    Get-LayerCCommitSha, Write-LayerCBuildIdentity, Get-LayerCBuildIdentity,
    Assert-LayerCBuildIdentity, New-LayerCHardwareBenchmarkArguments,
    New-LayerCBenchmarkResult, Measure-LayerCPlateau, Set-BenchmarkForeground
