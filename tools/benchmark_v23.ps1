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
Import-Module (Join-Path $PSScriptRoot 'LayerC.Tools.psm1') -Force
$context = New-LayerCBenchmarkContext -BuildDirectory $BuildDirectory `
    -OutputPath $OutputPath -OutputStem 'v23-automation' `
    -TemporaryStem 'LayerC-v23-benchmark'
$BuildDirectory = $context.BuildDirectory
$OutputPath = $context.OutputPath
$temporary = $context.Temporary
$profileDirectory = $context.ProfileDirectory
$utf8 = $context.Utf8
Assert-LayerCExecutables $BuildDirectory @(
    'layerd.exe', 'layerc_automation_benchmark.exe') `
    -Hint 'Build Release with tests first.'
Assert-LayerCQuiescent -ProcessNames layerd `
    -FailureMessage 'Close the existing LayerD process before running the V2.3 benchmark.'
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$automationDirectory = Join-Path $profileDirectory '_automation'
$stateDirectory = Join-Path $profileDirectory '_state'
[IO.Directory]::CreateDirectory($automationDirectory) | Out-Null
[IO.Directory]::CreateDirectory($stateDirectory) | Out-Null
$measure = @{
    Seconds = $DurationSeconds
    SampleIntervalMilliseconds = 200
    FailureMessage = 'LayerD exited during the V2.3 resource measurement.'
}

function Request-ProfileReload {
    $reload = [Threading.EventWaitHandle]::OpenExisting(
        'Local\LayerC.LayerD.ProfileReload')
    try {
        if (-not $reload.Set()) { throw 'Could not request a profile reload.' }
    } finally {
        $reload.Dispose()
    }
}

function Wait-AutomationActive {
    param(
        $Daemon,
        [string]$EventName = 'Local\LayerC.LayerD.AutomationActive.v1',
        [int]$TimeoutMilliseconds = 12000
    )
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalMilliseconds -lt $TimeoutMilliseconds) {
        if ($Daemon.HasExited) { throw 'LayerD exited while activating automation.' }
        try {
            $active = [Threading.EventWaitHandle]::OpenExisting($EventName)
            if ($active.WaitOne(0)) { return $active }
            $active.Dispose()
        } catch [Threading.WaitHandleCannotBeOpenedException] {
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'LayerD did not activate AutomationService; refusing an idle-service false pass.'
}

function Wait-AutomationInactive {
    param($Daemon, $Active, [int]$TimeoutMilliseconds = 12000)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalMilliseconds -lt $TimeoutMilliseconds) {
        if ($Daemon.HasExited) { throw 'LayerD exited while disabling automation.' }
        # Keep the handle acquired at activation: disappearance of the named
        # event is not evidence, but observing its reset is explicit teardown.
        if (-not $Active.WaitOne(0)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Measure-AutomationLifecycle {
    param($Daemon, [int]$CheckpointTimeoutMilliseconds = 12000)
    if (Test-Path -LiteralPath $sidecarPath) {
        throw 'Baseline automation sidecar must be absent; refusing an active-service baseline.'
    }
    Start-Sleep -Seconds $WarmupSeconds
    if ($Daemon.HasExited) { throw 'LayerD exited during warmup.' }
    $without = Measure-LayerCPlateau -Process $Daemon @measure

    [IO.File]::WriteAllText($sidecarPath, $automation, $utf8)
    [IO.File]::WriteAllText($profilePath, $baseWithAutomation, $utf8)
    Request-ProfileReload
    $active = Wait-AutomationActive -Daemon $Daemon
    try {
        $checkpointed = $false
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($clock.Elapsed.TotalMilliseconds -lt $CheckpointTimeoutMilliseconds) {
            if ($Daemon.HasExited) { throw 'LayerD exited before its persistent checkpoint.' }
            if (Test-Path -LiteralPath $statePath) {
                $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
                if ($null -ne $state.values.ticks -and [int64]$state.values.ticks -gt 0) {
                    $checkpointed = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $checkpointed) {
            throw 'LayerD did not checkpoint the persistent tick; refusing a persistence false pass.'
        }
        Start-Sleep -Seconds $WarmupSeconds
        $with = Measure-LayerCPlateau -Process $Daemon @measure

        # Sidecar existence, not the base modules metadata, is the loader's
        # authority. The disposable fixture has no sidecar backup/marker.
        [IO.File]::Delete($sidecarPath)
        [IO.File]::WriteAllText($profilePath, $baseWithoutAutomation, $utf8)
        Request-ProfileReload
        $deactivated = Wait-AutomationInactive -Daemon $Daemon -Active $active
        if (-not $deactivated) {
            throw 'LayerD did not deactivate AutomationService; refusing a teardown false pass.'
        }
        Start-Sleep -Seconds $WarmupSeconds
        $after = Measure-LayerCPlateau -Process $Daemon @measure
        return [pscustomobject]@{
            processId = $Daemon.Id
            withoutAutomation = $without
            withAutomation = $with
            afterDisable = $after
            activationVerified = $true
            persistentCheckpointVerified = $checkpointed
            deactivationVerified = $deactivated
        }
    } finally {
        $active.Dispose()
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
    [IO.File]::WriteAllText($sidecarPath, $automation, $utf8)
    [IO.File]::WriteAllText($profilePath, $baseWithAutomation, $utf8)
    & (Join-Path $BuildDirectory 'layerc_automation_benchmark.exe') `
        --validate $profilePath $sidecarPath
    if ($LASTEXITCODE -ne 0) {
        throw 'The disposable V2.3 benchmark bundle is invalid.'
    }
    [IO.File]::Delete($sidecarPath)
    [IO.File]::WriteAllText($profilePath, $baseWithoutAutomation, $utf8)
    Enter-LayerCProfileSandbox -ProfileDirectory $profileDirectory
    # One owned PID supplies all three plateaus; reload, never restart.
    $daemon = Start-LayerCDaemon -BuildDirectory $BuildDirectory
    $lifecycle = Measure-AutomationLifecycle -Daemon $daemon
    $withoutAutomation = $lifecycle.withoutAutomation
    $withAutomation = $lifecycle.withAutomation
    $afterDisable = $lifecycle.afterDisable

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
        normalAverageMicroseconds = $micro.normalGateMicroseconds
        hardP99Microseconds = $micro.hardGateMicroseconds
    }
    $passed = $privateDelta -le $thresholds.automationMaxPrivateDeltaMiB -and
        $workingDelta -le $thresholds.automationMaxWorkingSetDeltaMiB -and
        $threadDelta -le $thresholds.automationMaxThreadDelta -and
        $recoveredPrivate -le $thresholds.disabledMaxPrivateDeltaMiB -and
        $recoveredWorking -le $thresholds.disabledMaxWorkingSetDeltaMiB -and
        [bool]$micro.passed
    $data = [ordered]@{
        buildDirectory = $BuildDirectory
        automationServiceActivationVerified = $lifecycle.activationVerified
        persistentCheckpointVerified = $lifecycle.persistentCheckpointVerified
        automationServiceDeactivationVerified = $lifecycle.deactivationVerified
        lifecycleProcessId = $lifecycle.processId
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
    }
    $result = New-LayerCBenchmarkResult -OutputPath $OutputPath `
        -BuildDirectory $context.BuildDirectory -Data $data `
        -Thresholds $thresholds -Passed $passed -Depth 8
    $result | ConvertTo-Json -Depth 8
    if (-not $passed) { exit 1 }
} finally {
    Stop-LayerCBenchmarkProcess -Process $daemon
    Exit-LayerCProfileSandbox -PreviousDirectory $oldProfileDirectory `
        -TemporaryDirectory $temporary -KeepTemporary:$KeepTemporary
    if ($KeepTemporary) {
        Write-Warning "Preserved V2.3 benchmark files at $temporary"
    }
}
