#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [ValidateRange(2, 60)]
    [int]$WarmupSeconds = 4,
    [ValidateRange(4, 120)]
    [int]$DurationSeconds = 8,
    [string]$OutputPath = '',
    [switch]$KeepTemporary
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repository 'build\mingw-preset'
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
foreach ($name in @('layerd.exe', 'layerc_automation_benchmark.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $name))) {
        throw "Missing $name in $BuildDirectory. Build Release with tests first."
    }
}
if (Get-Process -Name layerd -ErrorAction SilentlyContinue) {
    throw 'Close the existing LayerD process before running the V2.3 benchmark.'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $outputDirectory = Join-Path $repository 'out\benchmarks'
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $OutputPath = Join-Path $outputDirectory "v23-automation-$stamp.json"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) "LayerC-v23-benchmark-$PID-$stamp"
$profileDirectory = Join-Path $temporary 'profiles'
$automationDirectory = Join-Path $profileDirectory '_automation'
$stateDirectory = Join-Path $profileDirectory '_state'
[IO.Directory]::CreateDirectory($automationDirectory) | Out-Null
[IO.Directory]::CreateDirectory($stateDirectory) | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Wait-AutomationActive {
    for ($attempt = 0; $attempt -lt 120; ++$attempt) {
        Start-Sleep -Milliseconds 100
        try {
            $active = [Threading.EventWaitHandle]::OpenExisting(
                'Local\LayerC.LayerD.AutomationActive.v1')
            try {
                if ($active.WaitOne(0)) { return $true }
            } finally {
                $active.Dispose()
            }
        } catch [Threading.WaitHandleCannotBeOpenedException] {
        }
    }
    return $false
}

function Measure-Plateau([Diagnostics.Process]$Process, [int]$Seconds) {
    $private = [Collections.Generic.List[long]]::new()
    $working = [Collections.Generic.List[long]]::new()
    [int]$maxThreads = 0
    $Process.Refresh()
    $startCpu = $Process.TotalProcessorTime.TotalMilliseconds
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds -and -not $Process.HasExited) {
        $Process.Refresh()
        $private.Add($Process.PrivateMemorySize64)
        $working.Add($Process.WorkingSet64)
        $maxThreads = [Math]::Max($maxThreads, $Process.Threads.Count)
        Start-Sleep -Milliseconds 200
    }
    if ($Process.HasExited -or $private.Count -eq 0) {
        throw 'LayerD exited during the V2.3 resource measurement.'
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
        medianWorkingSetMiB = [Math]::Round($sortedWorking[$middle] / 1MB, 3)
        maxThreadCount = $maxThreads
    }
}

$profilePath = Join-Path $profileDirectory 'automation-benchmark.json'
$sidecarPath = Join-Path $automationDirectory 'automation-benchmark.json'
$statePath = Join-Path $stateDirectory 'automation-benchmark.json.automation-state.json'
$baseWithoutAutomation = @'
{
  "schemaVersion": 4,
  "id": "automation-benchmark",
  "name": "Automation benchmark",
  "enabled": true,
  "game": {"global": true},
  "visualRuntime": "inline",
  "modules": [],
  "cooldowns": [
    {"id":"dash","name":"Dash","durationMs":1000,"maxCharges":1,
     "retrigger":"ignore","recovery":"single"}
  ],
  "cooldownGroups": [],
  "hidBindings": [],
  "panels": [
    {"id":"main","name":"Main","layout":"row","style":"bar",
     "bar":{"width":240,"height":24,"fillColor":"#50BAC8",
       "backgroundColor":"#202A31","borderColor":"#80D8E8",
       "textColor":"#F1F5F7","showText":true,"hideWhenReady":true},
     "anchorX":0.5,"anchorY":0.75,"cooldowns":["dash"]}
  ],
  "bindings": [
    {"cooldownId":"dash","chord":[{"device":"keyboard","code":18}]}
  ]
}
'@
$baseWithAutomation = $baseWithoutAutomation.Replace(
    '"modules": []',
    '"modules": [{"kind":"automation","schemaVersion":1}]')
$automation = @'
{
  "schemaVersion": 1,
  "profileId": "automation-benchmark",
  "continuousInput": {"deadzone":0.15,"changeThreshold":0.02,
                       "maxEventsPerSecond":60},
  "variables": [
    {"id":"ticks","type":"int","persistent":true,"default":0}
  ],
  "automations": [
    {"id":"heartbeat","name":"Benchmark heartbeat","enabled":true,
     "rules":[
       {"id":"start","priority":0,"event":"sessionStarted",
        "condition":{"op":"literal","valueType":"bool","value":true},
        "actions":[
          {"type":"incrementVariable","target":"ticks",
           "value":{"op":"literal","valueType":"int","value":1}},
          {"type":"createTimer","target":"heartbeat","repeating":true,
           "value":{"op":"literal","valueType":"duration","value":100}}
        ]},
       {"id":"tick","priority":0,"event":"timerFired","source":"heartbeat",
        "condition":{"op":"literal","valueType":"bool","value":true},
        "actions":[
          {"type":"incrementVariable","target":"ticks",
           "value":{"op":"literal","valueType":"int","value":1}}
        ]}
     ]}
  ]
}
'@

$oldProfileDirectory = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
$daemon = $null
try {
    [IO.File]::WriteAllText($profilePath, $baseWithoutAutomation, $utf8)
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $profileDirectory, 'Process')
    $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds $WarmupSeconds
    if ($daemon.HasExited) { throw "LayerD exited during warmup with code $($daemon.ExitCode)." }
    $withoutAutomation = Measure-Plateau $daemon $DurationSeconds

    Stop-Process -Id $daemon.Id -Force
    [void]$daemon.WaitForExit(5000)
    $daemon = $null
    [IO.File]::WriteAllText($sidecarPath, $automation, $utf8)
    [IO.File]::WriteAllText($profilePath, $baseWithAutomation, $utf8)
    & (Join-Path $BuildDirectory 'layerc_automation_benchmark.exe') `
        --validate $profilePath $sidecarPath
    if ($LASTEXITCODE -ne 0) {
        throw 'The disposable V2.3 benchmark bundle is invalid.'
    }
    $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
    $activated = Wait-AutomationActive
    if (-not $activated) {
        throw 'LayerD did not activate AutomationService; refusing an idle-service false pass.'
    }
    $checkpointed = $false
    for ($attempt = 0; $attempt -lt 120; ++$attempt) {
        Start-Sleep -Milliseconds 100
        if (Test-Path -LiteralPath $statePath) {
            $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
            if ($null -ne $state.values.ticks -and [int64]$state.values.ticks -gt 0) {
                $checkpointed = $true
                break
            }
        }
    }
    if (-not $checkpointed) {
        throw 'LayerD did not checkpoint the persistent tick; refusing a persistence false pass.'
    }
    Start-Sleep -Seconds $WarmupSeconds
    $withAutomation = Measure-Plateau $daemon $DurationSeconds

    Stop-Process -Id $daemon.Id -Force
    [void]$daemon.WaitForExit(5000)
    $daemon = $null
    [IO.File]::WriteAllText($profilePath, $baseWithoutAutomation, $utf8)
    $daemon = Start-Process -FilePath (Join-Path $BuildDirectory 'layerd.exe') `
        -WorkingDirectory $BuildDirectory -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds $WarmupSeconds
    if ($daemon.HasExited) { throw "LayerD exited after automation was disabled." }
    $afterDisable = Measure-Plateau $daemon $DurationSeconds

    $microJson = & (Join-Path $BuildDirectory 'layerc_automation_benchmark.exe')
    if ($LASTEXITCODE -ne 0) { throw 'The automation evaluator benchmark failed.' }
    $micro = $microJson | ConvertFrom-Json

    $privateDelta = $withAutomation.medianPrivateMiB - $withoutAutomation.medianPrivateMiB
    $workingDelta = $withAutomation.medianWorkingSetMiB - $withoutAutomation.medianWorkingSetMiB
    $threadDelta = $withAutomation.maxThreadCount - $withoutAutomation.maxThreadCount
    $recoveredPrivate = $afterDisable.medianPrivateMiB - $withoutAutomation.medianPrivateMiB
    $recoveredWorking = $afterDisable.medianWorkingSetMiB - $withoutAutomation.medianWorkingSetMiB
    $thresholds = [ordered]@{
        automationMaxPrivateDeltaMiB = 1.0
        automationMaxWorkingSetDeltaMiB = 2.0
        automationMaxThreadDelta = 0
        disabledMaxPrivateDeltaMiB = 1.0
        disabledMaxWorkingSetDeltaMiB = 2.0
        normalAverageMicroseconds = 250.0
        hardMaximumMicroseconds = 2000.0
    }
    $passed = $privateDelta -le $thresholds.automationMaxPrivateDeltaMiB -and
        $workingDelta -le $thresholds.automationMaxWorkingSetDeltaMiB -and
        $threadDelta -le $thresholds.automationMaxThreadDelta -and
        $recoveredPrivate -le $thresholds.disabledMaxPrivateDeltaMiB -and
        $recoveredWorking -le $thresholds.disabledMaxWorkingSetDeltaMiB -and
        [bool]$micro.passed
    $result = [ordered]@{
        schemaVersion = 1
        packageVersion = '2.3.0'
        measuredAt = (Get-Date).ToUniversalTime().ToString('o')
        buildDirectory = $BuildDirectory
        automationServiceActivationVerified = $activated
        persistentCheckpointVerified = $checkpointed
        withoutAutomation = $withoutAutomation
        withAutomation = $withAutomation
        afterDisable = $afterDisable
        deltas = [ordered]@{
            automationPrivateMiB = [Math]::Round($privateDelta, 3)
            automationWorkingSetMiB = [Math]::Round($workingDelta, 3)
            automationThreads = $threadDelta
            disabledPrivateMiB = [Math]::Round($recoveredPrivate, 3)
            disabledWorkingSetMiB = [Math]::Round($recoveredWorking, 3)
        }
        evaluator = $micro
        thresholds = $thresholds
        passed = $passed
    }
    [IO.File]::WriteAllText($OutputPath, ($result | ConvertTo-Json -Depth 8), $utf8)
    $result | ConvertTo-Json -Depth 8
    if (-not $passed) { exit 1 }
} finally {
    if ($null -ne $daemon -and -not $daemon.HasExited) {
        Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue
        [void]$daemon.WaitForExit(5000)
    }
    [Environment]::SetEnvironmentVariable('LAYERC_PROFILE_DIR', $oldProfileDirectory, 'Process')
    if ($KeepTemporary) {
        Write-Warning "Preserved V2.3 benchmark files at $temporary"
    } elseif (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
