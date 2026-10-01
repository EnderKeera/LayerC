#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Objective,

    [ValidateRange(1, 8)]
    [int]$Workers = 3,

    [switch]$Preview
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$worktreeSelector = 'path:' + $projectRoot.Replace('\', '/')
$coordinatorCommand = 'pi --model openai-codex/gpt-6-astra --thinking high'

function Invoke-OrcaJson {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = & orca @Arguments
    $exitCode = $LASTEXITCODE
    $raw = $output -join [Environment]::NewLine
    if ([string]::IsNullOrWhiteSpace($raw)) {
        throw "Orca returned no result (exit $exitCode): $($Arguments -join ' ')"
    }
    try {
        $response = $raw | ConvertFrom-Json
    } catch {
        throw "Orca returned an unreadable result (exit $exitCode): $raw"
    }
    if ($exitCode -ne 0 -or $response.ok -ne $true) {
        throw "Orca failed (exit $exitCode): $raw"
    }
    return $response.result
}

$objectiveText = $Objective.Trim()
if ($objectiveText.Length -eq 0) {
    throw 'Objective must contain a task for the coordinator.'
}

$prompt = @"
You are the Pi coordinator using openai-codex/gpt-6-astra for LayerC in $projectRoot.

Objective: $objectiveText

Use the installed Orca orchestration skill and its version-matched CLI guide. Create one Run in this terminal. Plan $Workers self-contained, non-overlapping Tasks and start $Workers supervised Pi workers using claude-bridge/claude-opus-5-5. Start the independent wave before waiting. If the objective cannot be split safely into $Workers useful Tasks, ask me before launching workers.

Orca's worker-start --model option does not support Pi. For each worker, choose its workspace, create an Orca terminal there with the command pi --model claude-bridge/claude-opus-5-5, wait for tui-idle, then use worker-start --task <task_id> --terminal <handle> --worktree <that exact workspace selector>. Read the orchestration low-level-topology reference before this custom-argv path. Use worker-start --terminal so Orca owns the worker lifecycle; dispatch --inject would leave it unsupervised.

Read AGENTS.md and the relevant LayerC documentation or project skills before assigning work. Preserve existing changes. Use the current workspace for read-only work; use separate Orca worktrees when simultaneous edits would conflict, and remember new worktrees do not inherit uncommitted files. Give every worker a concrete target, change, constraints, ownership boundary, and acceptance evidence.

Follow the Orca coordinator loop through worker_done, review each result, decide whether to reuse, retain, or release every settled worker, and report the outcome and evidence for each Task. Never treat silence or an uncertain start as proof of failure or completion.
"@

if (-not (Get-Command orca -ErrorAction SilentlyContinue)) {
    throw 'The Orca CLI is not on PATH.'
}
if (-not (Get-Command pi -ErrorAction SilentlyContinue)) {
    throw 'The Pi CLI is not on PATH.'
}

Push-Location $projectRoot
try {
    $status = Invoke-OrcaJson @('status', '--json')
    if ($status.runtime.state -ne 'ready') {
        throw "Orca runtime is $($status.runtime.state), not ready."
    }
    $workspace = Invoke-OrcaJson @('worktree', 'show', '--worktree', $worktreeSelector, '--json')
    if ($workspace.worktree.path.Replace('\', '/') -ne $projectRoot.Replace('\', '/')) {
        throw 'Orca did not resolve this script to the LayerC workspace.'
    }

    if ($Preview) {
        Write-Host "Workspace: $($workspace.worktree.path)"
        Write-Host "Coordinator command: $coordinatorCommand"
        Write-Host "Coordinator prompt:`n$prompt"
        return
    }

    $created = Invoke-OrcaJson @(
        'terminal', 'create', '--worktree', $worktreeSelector,
        '--title', 'LayerC Pi Astra coordinator', '--command', $coordinatorCommand,
        '--focus', '--json')
    $handle = [string]$created.terminal.handle
    if ([string]::IsNullOrWhiteSpace($handle)) {
        throw 'Orca created a terminal but returned no handle; inspect the LayerC workspace.'
    }

    try {
        $wait = Invoke-OrcaJson @('terminal', 'wait', '--terminal', $handle,
            '--for', 'tui-idle', '--timeout-ms', '60000', '--json')
        if ($wait.wait.satisfied -ne $true) {
            $wait = Invoke-OrcaJson @('terminal', 'wait', '--terminal', $handle,
                '--for', 'tui-idle', '--timeout-ms', '120000', '--json')
        }
        if ($wait.wait.satisfied -ne $true) {
            throw 'The coordinator did not become ready. No objective was sent.'
        }

        $sent = Invoke-OrcaJson @('terminal', 'send', '--terminal', $handle,
            '--text', $prompt, '--enter', '--wait-submit', '30', '--json')
        if ($sent.send.accepted -ne $true) {
            throw 'Orca did not confirm acceptance of the coordinator objective.'
        }
    } catch {
        throw "Coordinator terminal $handle remains open. Inspect it before retrying. $($_.Exception.Message)"
    }

    $started = @($sent.send.prompt.stages) -contains 'turn_started'
    [pscustomobject]@{
        Workspace = $workspace.worktree.path
        CoordinatorTerminal = $handle
        WorkerModel = 'claude-bridge/claude-opus-5-5'
        WorkersRequested = $Workers
        PromptAccepted = $true
        TurnStarted = $started
    }
} finally {
    Pop-Location
}
