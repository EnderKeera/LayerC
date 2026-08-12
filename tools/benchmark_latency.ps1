#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BaselineBuildDirectory = '',
    [string]$CandidateBuildDirectory = '',
    [ValidateRange(10, 200)]
    [int]$Samples = 40,
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BaselineBuildDirectory)) {
    $BaselineBuildDirectory = Join-Path $repository 'build\mingw-v11'
}
if ([string]::IsNullOrWhiteSpace($CandidateBuildDirectory)) {
    $CandidateBuildDirectory = Join-Path $repository 'build\mingw-preset'
}
$BaselineBuildDirectory = [IO.Path]::GetFullPath($BaselineBuildDirectory)
$CandidateBuildDirectory = [IO.Path]::GetFullPath($CandidateBuildDirectory)
foreach ($directory in @($BaselineBuildDirectory, $CandidateBuildDirectory)) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory 'layerd.exe'))) {
        throw "Missing layerd.exe in $directory."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $CandidateBuildDirectory 'lctest.exe'))) {
    throw "Missing lctest.exe in $CandidateBuildDirectory."
}
if (Get-Process -Name layerd, lctest -ErrorAction SilentlyContinue) {
    throw 'Close existing LayerD and lctest processes before the latency benchmark.'
}

if (-not ('LayerCLatencyNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LayerCLatencyNative {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
}
'@
}

function Send-TriggerChord {
    $scanCode = 0x0008 # KEYEVENTF_SCANCODE
    $keyUp = 0x0002
    [LayerCLatencyNative]::keybd_event(0x10, 42, $scanCode, [UIntPtr]::Zero)
    [LayerCLatencyNative]::keybd_event(0x45, 18, $scanCode, [UIntPtr]::Zero)
    [LayerCLatencyNative]::keybd_event(0x45, 18, $scanCode -bor $keyUp, [UIntPtr]::Zero)
    [LayerCLatencyNative]::keybd_event(0x10, 42, $scanCode -bor $keyUp, [UIntPtr]::Zero)
}

function Percentile([double[]]$Values, [double]$P) {
    $ordered = @($Values | Sort-Object)
    $index = [Math]::Ceiling($P * $ordered.Count) - 1
    return $ordered[[Math]::Max(0, [Math]::Min($ordered.Count - 1, $index))]
}

function Measure-Version([string]$BuildDirectory, [Diagnostics.Process]$Game) {
    $daemon = $null
    $activator = New-Object -ComObject WScript.Shell
    try {
        $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
            -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
        $overlay = [IntPtr]::Zero
        for ($attempt = 0; $attempt -lt 80; ++$attempt) {
            if ($daemon.HasExited) { throw "LayerD exited with code $($daemon.ExitCode)." }
            $Game.Refresh()
            [void][LayerCLatencyNative]::ShowWindowAsync($Game.MainWindowHandle, 9)
            [void]$activator.AppActivate($Game.Id)
            [void][LayerCLatencyNative]::SetForegroundWindow($Game.MainWindowHandle)
            # Overlay HWNDs are created lazily on the first visible cooldown.
            # Warm the real Raw Input path after giving profile/window matching
            # time to settle; the timed samples below begin only after it hides.
            if ($attempt -ge 10 -and ($attempt % 5) -eq 0) {
                Send-TriggerChord
            }
            $overlay = [LayerCLatencyNative]::FindWindow('LayerDOverlayWindow', 'LayerD Overlay')
            if ($overlay -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($overlay -eq [IntPtr]::Zero) { throw 'LayerD did not create the benchmark overlay.' }
        $values = [Collections.Generic.List[double]]::new()
        for ($sample = 0; $sample -lt $Samples; ++$sample) {
            for ($attempt = 0; $attempt -lt 200 -and
                    [LayerCLatencyNative]::IsWindowVisible($overlay); ++$attempt) {
                Start-Sleep -Milliseconds 2
            }
            if ([LayerCLatencyNative]::IsWindowVisible($overlay)) {
                throw 'Overlay did not return to its ready-hidden state.'
            }
            [void]$activator.AppActivate($Game.Id)
            [void][LayerCLatencyNative]::SetForegroundWindow($Game.MainWindowHandle)
            $clock = [Diagnostics.Stopwatch]::StartNew()
            Send-TriggerChord
            while (-not [LayerCLatencyNative]::IsWindowVisible($overlay) -and
                   $clock.Elapsed.TotalMilliseconds -lt 500.0) {
                [Threading.Thread]::SpinWait(64)
            }
            $clock.Stop()
            if (-not [LayerCLatencyNative]::IsWindowVisible($overlay)) {
                throw 'Injected benchmark chord did not produce an overlay transition.'
            }
            $values.Add($clock.Elapsed.TotalMilliseconds)
        }
        return [ordered]@{
            samples = $values.Count
            medianMs = [Math]::Round((Percentile $values.ToArray() 0.5), 3)
            p95Ms = [Math]::Round((Percentile $values.ToArray() 0.95), 3)
            minimumMs = [Math]::Round(($values | Measure-Object -Minimum).Minimum, 3)
            maximumMs = [Math]::Round(($values | Measure-Object -Maximum).Maximum, 3)
        }
    } finally {
        if ($null -ne $daemon -and -not $daemon.HasExited) {
            Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue
            [void]$daemon.WaitForExit(5000)
        }
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputDirectory = Join-Path $repository 'out\benchmarks'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $OutputPath = Join-Path $outputDirectory "v22-input-overlay-latency-$stamp.json"
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) "LayerC-latency-$PID-$stamp"
$profiles = Join-Path $temporary 'profiles'
[IO.Directory]::CreateDirectory($profiles) | Out-Null
$profile = @'
{
  "schemaVersion": 1,
  "id": "latency", "name": "Latency gate",
  "game": {"executableName":"lctest.exe"},
  "cooldowns": [{"id":"pulse","name":"Pulse","durationMs":120}],
  "panels": [{"id":"pulse-bar","name":"Pulse","style":"bar",
    "bar":{"width":240,"height":20,"fillColor":"#50BAC8",
      "backgroundColor":"#202A31","borderColor":"#80D8E8",
      "textColor":"#F1F5F7","hideWhenReady":true},
    "anchorX":0.5,"anchorY":0.75,"cooldowns":["pulse"]}],
  "bindings": [{"cooldownId":"pulse","chord":[
    {"device":"keyboard","code":42},{"device":"keyboard","code":18}]}]
}
'@
[IO.File]::WriteAllText((Join-Path $profiles 'latency.json'), $profile,
    (New-Object Text.UTF8Encoding($false)))
$oldProfiles = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
$game = $null
try {
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $profiles, 'Process')
    $game = Start-Process -FilePath (Join-Path $CandidateBuildDirectory 'lctest.exe') -PassThru
    for ($attempt = 0; $attempt -lt 80; ++$attempt) {
        Start-Sleep -Milliseconds 50
        $game.Refresh()
        if ($game.MainWindowHandle -ne 0) { break }
    }
    if ($game.MainWindowHandle -eq 0) { throw 'lctest did not create its window.' }
    $baseline = Measure-Version $BaselineBuildDirectory $game
    Start-Sleep -Milliseconds 500
    $candidate = Measure-Version $CandidateBuildDirectory $game
    $medianRegression = $candidate.medianMs - $baseline.medianMs
    $p95Regression = $candidate.p95Ms - $baseline.p95Ms
    $passed = $medianRegression -le 1.0 -and $p95Regression -le 1.0
    $result = [ordered]@{
        schemaVersion = 1
        measuredAt = (Get-Date).ToUniversalTime().ToString('o')
        samplesPerVersion = $Samples
        baselineBuild = $BaselineBuildDirectory
        candidateBuild = $CandidateBuildDirectory
        baseline = $baseline
        candidate = $candidate
        medianRegressionMs = [Math]::Round($medianRegression, 3)
        p95RegressionMs = [Math]::Round($p95Regression, 3)
        maximumAllowedRegressionMs = 1.0
        passed = $passed
        note = 'Same-machine A/B through the real Raw Input chord and hidden-to-visible overlay transition.'
    }
    [IO.File]::WriteAllText($OutputPath, ($result | ConvertTo-Json -Depth 6),
        (New-Object Text.UTF8Encoding($false)))
    $result | ConvertTo-Json -Depth 6
    if (-not $passed) { exit 1 }
} finally {
    if ($null -ne $game -and -not $game.HasExited) {
        [void]$game.CloseMainWindow()
        if (-not $game.WaitForExit(3000)) {
            Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
        }
    }
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $oldProfiles, 'Process')
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
