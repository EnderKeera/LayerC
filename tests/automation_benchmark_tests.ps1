# Exercise the real orchestration without starting a daemon, touching HKCU,
# or synthesizing input. A separate Windows check uses only private events.
param([string]$BenchmarkScript = (Join-Path $PSScriptRoot '../tools/benchmark_v23.ps1'))
$ErrorActionPreference = 'Stop'
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $BenchmarkScript, [ref]$null, [ref]$errors)
if ($errors) { throw "Could not parse ${BenchmarkScript}: $errors" }
foreach ($name in @('Wait-AutomationActive', 'Wait-AutomationInactive', 'Measure-AutomationLifecycle')) {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if ($null -eq $definition) { throw "Missing automation benchmark function: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$waitActive = (Get-Command Wait-AutomationActive).ScriptBlock
$waitInactive = (Get-Command Wait-AutomationInactive).ScriptBlock
$starts = $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -eq 'Start-LayerCDaemon'
}, $true)
if ($starts.Count -ne 1) { throw 'All lifecycle plateaus must use one daemon start.' }
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "Unexpected error: $($_.Exception.Message)" }
        return
    }
    throw "Expected an error matching '$Pattern'."
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('LayerC-lifecycle-test-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $temporary
$profilePath = Join-Path $temporary 'profile.json'
$sidecarPath = Join-Path $temporary 'automation.json'
$statePath = Join-Path $temporary 'state.json'
$utf8 = New-Object Text.UTF8Encoding($false)
$baseWithoutAutomation = '{"modules":[]}'
$baseWithAutomation = '{"modules":[{"kind":"automation"}]}'
$automation = '{"automations":[{"enabled":true}]}'
# Script-scoped fixture values are consumed by the AST-loaded orchestration.
$script:WarmupSeconds = 0
$script:measure = @{ Seconds = 0; FailureMessage = 'test' }
$daemon = [pscustomobject]@{ Id = 12345; HasExited = $false }
$script:ExpectedDaemon = $daemon
$script:Active = $false
$script:DisposeCount = 0
$activityHandle = [pscustomobject]@{}
$activityHandle | Add-Member ScriptMethod WaitOne { param($milliseconds) return $script:Active }
$activityHandle | Add-Member ScriptMethod Dispose { ++$script:DisposeCount }
$script:Plateaus = [Collections.Generic.List[object]]::new()
$script:Reloads = 0
$script:RefuseActivation = $false
$script:RefuseDeactivation = $false

function Start-Sleep { param($Seconds, $Milliseconds) }
function Start-LayerCDaemon { throw 'A lifecycle measurement must not restart the daemon.' }
function Stop-Process { throw 'A lifecycle measurement must not destroy the daemon.' }
function Stop-LayerCBenchmarkProcess { throw 'A lifecycle measurement must not destroy the daemon.' }
function Request-ProfileReload {
    ++$script:Reloads
    $profileJson = [IO.File]::ReadAllText($profilePath)
    if ($profileJson -eq $baseWithAutomation) {
        if (-not (Test-Path -LiteralPath $sidecarPath) -or
            [IO.File]::ReadAllText($sidecarPath) -ne $automation) {
            throw 'Activation did not install the sidecar.'
        }
        $script:Active = $true
        [IO.File]::WriteAllText($statePath, '{"values":{"ticks":1}}', $utf8)
    } elseif ($profileJson -eq $baseWithoutAutomation) {
        # Model the real loader: modules:[] alone does not disable a sidecar.
        if (-not $script:RefuseDeactivation) {
            $script:Active = Test-Path -LiteralPath $sidecarPath
        }
    } else { throw 'Reload did not see the expected profile.' }
}
function Wait-AutomationActive {
    param($Daemon)
    if ($script:RefuseActivation) { throw 'activation refused' }
    if (-not $script:Active) { throw 'Activation was not requested.' }
    return $activityHandle
}
function Wait-AutomationInactive {
    param($Daemon, $Active)
    if (-not [object]::ReferenceEquals($Active, $activityHandle)) {
        throw 'Inactive verification did not retain the activation handle.'
    }
    if ($script:RefuseDeactivation) {
        return & $waitInactive -Daemon $Daemon -Active $Active -TimeoutMilliseconds 0
    }
    return & $waitInactive -Daemon $Daemon -Active $Active
}
function Measure-LayerCPlateau {
    param($Process, $Seconds, $FailureMessage)
    if (-not [object]::ReferenceEquals($Process, $script:ExpectedDaemon)) {
        throw 'A plateau measured a different daemon.'
    }
    $script:Plateaus.Add([pscustomobject]@{
        processId = $Process.Id; active = $script:Active
        profile = [IO.File]::ReadAllText($profilePath)
    })
    return [pscustomobject]@{ medianPrivateMiB = 10; medianWorkingSetMiB = 20; maxThreadCount = 1 }
}
function Reset-LifecycleFixture {
    $script:Plateaus.Clear()
    $script:Reloads = 0
    $script:Active = $false
    $script:DisposeCount = 0
    $script:RefuseActivation = $false
    $script:RefuseDeactivation = $false
    [IO.File]::WriteAllText($profilePath, $baseWithoutAutomation, $utf8)
    Remove-Item -LiteralPath $statePath, $sidecarPath -ErrorAction SilentlyContinue
}
try {
    Reset-LifecycleFixture
    $result = Measure-AutomationLifecycle -Daemon $daemon
    if ($result.processId -ne $daemon.Id -or $script:Plateaus.Count -ne 3 -or
        $script:Reloads -ne 2 -or $script:DisposeCount -ne 1 -or
        -not $result.activationVerified -or -not $result.persistentCheckpointVerified -or
        -not $result.deactivationVerified) { throw 'Lifecycle evidence is incomplete.' }
    if ($script:Plateaus[0].active -or -not $script:Plateaus[1].active -or
        $script:Plateaus[2].active -or $script:Plateaus[0].profile -ne $baseWithoutAutomation -or
        $script:Plateaus[1].profile -ne $baseWithAutomation -or
        $script:Plateaus[2].profile -ne $baseWithoutAutomation) {
        throw 'Baseline, active, and inactive plateaus were not ordered correctly.'
    }

    Reset-LifecycleFixture
    [IO.File]::WriteAllText($sidecarPath, $automation, $utf8)
    Assert-Throws { Measure-AutomationLifecycle -Daemon $daemon } 'active-service baseline'
    if ($script:Plateaus.Count -ne 0) { throw 'An active baseline was still measured.' }

    Reset-LifecycleFixture
    $script:RefuseDeactivation = $true
    Assert-Throws { Measure-AutomationLifecycle -Daemon $daemon } 'teardown false pass'
    if ($script:Plateaus.Count -ne 2 -or $script:DisposeCount -ne 1) {
        throw 'A failed teardown measured recovery or leaked the event handle.'
    }
    Reset-LifecycleFixture
    Assert-Throws { Measure-AutomationLifecycle -Daemon $daemon -CheckpointTimeoutMilliseconds 0 } 'persistence false pass'
    if ($script:Plateaus.Count -ne 1 -or $script:DisposeCount -ne 1) {
        throw 'A failed checkpoint measured automation or leaked the event handle.'
    }
    Reset-LifecycleFixture
    $script:RefuseActivation = $true
    Assert-Throws { Measure-AutomationLifecycle -Daemon $daemon } 'activation refused'
    if ($script:Plateaus.Count -ne 1) { throw 'Failed activation still measured automation.' }

    $dead = [pscustomobject]@{ Id = $daemon.Id; HasExited = $true }
    Assert-Throws { & $waitActive -Daemon $dead } 'exited while activating'
    Assert-Throws { & $waitInactive -Daemon $dead -Active $activityHandle } 'exited while disabling'
    Assert-Throws { & $waitActive -Daemon $daemon -TimeoutMilliseconds 0 } 'idle-service false pass'

    if ($IsWindows) {
        $eventName = 'Local\LayerC-lifecycle-test-' + [guid]::NewGuid()
        $producer = [Threading.EventWaitHandle]::new($true,
            [Threading.EventResetMode]::ManualReset, $eventName)
        $retained = $null
        try {
            $retained = & $waitActive -Daemon $daemon -EventName $eventName
            if (-not $retained.WaitOne(0)) { throw 'Activation was not observed.' }
            $null = $producer.Reset()
            $producer.Dispose()
            if (-not (& $waitInactive -Daemon $daemon -Active $retained)) {
                throw 'Reset on the retained handle was not observed after producer close.'
            }
        } finally {
            if ($retained) { $retained.Dispose() }
            $producer.Dispose()
        }
    }
    Write-Output 'Automation benchmark same-PID lifecycle and refusal controls passed.'
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
