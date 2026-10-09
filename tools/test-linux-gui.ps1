#Requires -Version 7.3
<#
.SYNOPSIS
Runs the Linux LayerE GUI walkthrough, or an agent bridge session, in WSL.

.DESCRIPTION
Walkthrough (default): records the source revision, mirrors this checkout into
a dedicated Linux directory, builds the editor targets, and runs
tools/linux-editor-gui-tests.sh in a private Xvfb/Openbox session. Evidence
goes to a unique out\linux-gui\<run>\ directory; the exit code is the gate.

Session: the same setup, but the computer-use bridge stays open for one
agent. Drive it with -Mode Call -Request '<json>' (one action or observation
per call; screenshots land in the run's shots\ directory) and finish with
-Mode Stop, which joins the session and prints its result. One session at a
time; Call and Stop reuse the session's recorded distro and directories.

Never starts a model/provider service, and never touches the user's display,
daemon, control socket or profiles. Dependency setup is separate: -EnvScript
names a Linux script to source first (for example an extracted sysroot).
-SourceDir must be a dedicated mirror (new, empty, or previously marked).
PowerShell 7.3+ is required for faithful native argument passing.
#>
[CmdletBinding()]
param(
    [ValidateSet('Walkthrough', 'Resources', 'Session', 'Call', 'Stop')]
    [string]$Mode = 'Walkthrough',
    [string]$Request = '',
    [string]$Distro = 'Debian',
    [string]$EnvScript = '',
    [string]$SourceDir = '~/layerc-gui-src',
    [string]$BuildDir = '~/layerc-gui-build',
    [string[]]$CMakeArgs = @(),
    [int]$IdleSeconds = 60,
    [int]$SessionSeconds = 1800
)

$ErrorActionPreference = 'Stop'
# Faithful argument boundaries for native calls, whatever the caller's setting.
$PSNativeCommandArgumentPassing = 'Windows'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptRoot
Import-Module (Join-Path $scriptRoot 'LayerC.Tools.psm1') -Force
Import-Module (Join-Path $scriptRoot 'linux-gui/Lifecycle.psm1') -Force
$evidenceRoot = Join-Path $repositoryRoot 'out\linux-gui'
$activePath = Join-Path $evidenceRoot 'active-session.json'
New-Item -ItemType Directory -Force $evidenceRoot | Out-Null

$active = $null
if ($Mode -in 'Call', 'Stop') {
    if (-not (Test-Path -LiteralPath $activePath)) { throw 'No active Linux GUI session; start one with -Mode Session.' }
    $active = Get-Content -Raw $activePath | ConvertFrom-Json
    if (-not $active.sessionDir) { throw "The active session is still starting or failed to start; see $activePath." }
    if ($PSBoundParameters.ContainsKey('Distro') -and $Distro -ne $active.distro) {
        throw "The active session runs in $($active.distro), not $Distro."
    }
    $Distro = $active.distro
}

# Arguments go through `wsl.exe --exec` (no Linux shell re-parsing). PowerShell
# 7 expands a leading "~" in native arguments to the Windows profile, so Linux
# "~" paths are resolved here against the WSL account's home first.
$linuxHome = @(& wsl.exe -d $Distro --exec sh -c 'getent passwd "$(id -u)" | cut -d: -f6')
if ($LASTEXITCODE -ne 0 -or $linuxHome.Count -lt 1 -or -not $linuxHome[0]) {
    throw "Cannot resolve the WSL home directory in $Distro."
}
$linuxHome = $linuxHome[0].Trim()
function Resolve-LinuxPath {
    param([string]$Path)
    if ($Path -eq '~' -or $Path.StartsWith('~/')) { return $linuxHome + $Path.Substring(1) }
    return $Path
}
$SourceDir = Resolve-LinuxPath $SourceDir
$BuildDir = Resolve-LinuxPath $BuildDir
$EnvScript = Resolve-LinuxPath $EnvScript
# Never pass an empty positional argument; "-" means "no environment script".
$envArgument = if ($EnvScript) { $EnvScript } else { '-' }

function ConvertTo-WslPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $converted = @(& wsl.exe -d $Distro --exec wslpath -a ($Path -replace '\\', '/'))
    Assert-NativeSuccess "Converting $Path to a WSL path"
    return $converted[0].Trim()
}

function Invoke-Wsl {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    # Stream output to the host so only the exit code is returned.
    & wsl.exe -d $Distro --exec @Arguments | Out-Host
    return $LASTEXITCODE
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Write-JsonAtomic {
    param([Parameter(Mandatory = $true)]$Value, [Parameter(Mandatory = $true)][string]$Path)
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    $Value | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 -LiteralPath $temporary
    Move-Item -Force -LiteralPath $temporary -Destination $Path
}

function Get-Revision {
    param([Parameter(Mandatory = $true)][string]$RunDirectory)
    $head = (git -C $repositoryRoot rev-parse HEAD).Trim()
    Assert-NativeSuccess 'Resolving HEAD'
    $status = @(git -C $repositoryRoot status --porcelain=v1 --untracked-files=all)
    Assert-NativeSuccess 'Reading working-tree status'
    $diffPath = Join-Path $RunDirectory 'source-changes.diff'
    git -C $repositoryRoot diff HEAD --binary "--output=$diffPath"
    Assert-NativeSuccess 'Recording tracked source changes'
    $untracked = @(git -C $repositoryRoot ls-files -o --exclude-standard | Sort-Object)
    Assert-NativeSuccess 'Listing untracked source files'
    $lines = @("diff:$(Get-FileSha256 $diffPath)")
    foreach ($file in $untracked) {
        $lines += "${file}:$(Get-FileSha256 (Join-Path $repositoryRoot $file))"
    }
    $digestInput = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = -join ($sha.ComputeHash($digestInput) | ForEach-Object { $_.ToString('x2') })
    } finally {
        $sha.Dispose()
    }
    return [ordered]@{
        head               = $head
        dirty              = $status.Count -gt 0
        status             = $status
        untrackedFiles     = $untracked
        sourceChangeDigest = $digest
        digestDefinition   = 'SHA-256 of "diff:<sha256 of git diff HEAD --binary>" plus one "<path>:<sha256>" line per untracked file'
        recordedUtc        = (Get-Date).ToUniversalTime().ToString('o')
    }
}

function New-Run {
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    # A random suffix keeps concurrent invocations apart; creation fails
    # rather than reusing an existing directory.
    $name = "$($Mode.ToLowerInvariant())-$stamp-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $runDirectory = Join-Path $evidenceRoot $name
    New-Item -ItemType Directory $runDirectory | Out-Null
    $revision = Get-Revision $runDirectory
    $revision | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $runDirectory 'revision.json')
    # Only files present in the working tree: a tracked file deleted locally
    # must disappear from the mirror instead of surviving as a stale copy.
    $files = Join-Path $runDirectory 'source-files.txt'
    $list = @(git -C $repositoryRoot ls-files -co --exclude-standard |
        Where-Object { Test-Path -LiteralPath (Join-Path $repositoryRoot $_) -PathType Leaf })
    Assert-NativeSuccess 'Listing source files'
    [IO.File]::WriteAllText($files, (($list -join "`n") + "`n"))
    # Always mirror and build: the run must execute exactly the recorded
    # revision. An unchanged tree is an incremental no-op build.
    $repo = ConvertTo-WslPath $repositoryRoot
    $wslRun = ConvertTo-WslPath $runDirectory
    $arguments = @('bash', "$repo/tools/linux-gui/build.sh", '--repo', $repo,
        '--files', (ConvertTo-WslPath $files), '--source', $SourceDir, '--build', $BuildDir,
        '--stamp', "$wslRun/build-stamp.txt")
    if ($EnvScript) { $arguments += @('--env', $EnvScript) }
    if ($CMakeArgs.Count -gt 0) { $arguments += @('--') + $CMakeArgs }
    $code = Invoke-Wsl $arguments
    if ($code -ne 0) { throw "Linux build failed with exit code $code" }
    # The recorded revision must describe the bytes that were copied: refuse
    # a run whose working tree changed while it was mirrored and built.
    $after = Get-Revision $runDirectory
    if ($after.head -ne $revision.head -or $after.sourceChangeDigest -ne $revision.sourceChangeDigest) {
        throw "The working tree changed while it was mirrored; rerun. Evidence: $runDirectory"
    }
    return [pscustomobject]@{ Directory = $runDirectory; WslDirectory = $wslRun }
}

function Get-RunnerArguments {
    param([Parameter(Mandatory = $true)]$Run, [Parameter(Mandatory = $true)][string]$RunnerMode)
    # The mirrored copy runs from Linux storage.
    $arguments = @('bash', "$SourceDir/tools/linux-editor-gui-tests.sh", '--build', $BuildDir,
        '--out', $Run.WslDirectory, '--mode', $RunnerMode,
        '--idle-seconds', "$IdleSeconds", '--session-seconds', "$SessionSeconds",
        '--expect-stamp', "$($Run.WslDirectory)/build-stamp.txt")
    if ($EnvScript) { $arguments += @('--env', $EnvScript) }
    return $arguments
}

function Invoke-BridgeCall {
    param([Parameter(Mandatory = $true)][string]$Json)
    $null = $Json | ConvertFrom-Json  # Reject malformed requests before WSL.
    # A unique, fully written request file per call: concurrent calls cannot
    # execute one another's requests.
    $requests = Join-Path $active.directory 'requests'
    New-Item -ItemType Directory -Force $requests | Out-Null
    $requestPath = Join-Path $requests "$([Guid]::NewGuid().ToString('N')).json"
    [IO.File]::WriteAllText($requestPath, $Json)
    try {
        $command = 'if [ "$2" != - ]; then . "$2"; fi; ' +
            'exec python3 "$1/tools/linux-gui/bridge.py" call --session "$3" "@$4"'
        $output = & wsl.exe -d $Distro --exec bash -c $command call $active.sourceDir `
            $active.envScript $active.sessionDir (ConvertTo-WslPath $requestPath)
        $code = $LASTEXITCODE
    } finally {
        Remove-Item -LiteralPath $requestPath -ErrorAction SilentlyContinue
    }
    $text = $output -join "`n"
    try {
        $response = $text | ConvertFrom-Json
        if ($response.relative) {
            $response | Add-Member -NotePropertyName windowsPath `
                -NotePropertyValue (Join-Path $active.directory $response.relative)
        }
        # Deep enough for the accessibility tree (a11y.py MAX_DEPTH 8).
        Write-Host ($response | ConvertTo-Json -Depth 64)
    } catch {
        Write-Host $text
    }
    return $code
}

function Wait-Runner {
    # Waits only for the recorded runner: same PID and same start time.
    param([Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][string]$StartTime, [int]$Milliseconds = 120000)
    $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $process -or $process.StartTime.ToUniversalTime().ToString('o') -ne $StartTime) {
        return $true  # Already gone; the PID may now belong to something else.
    }
    return $process.WaitForExit($Milliseconds)
}

switch ($Mode) {
    { $_ -in 'Walkthrough', 'Resources' } {
        $run = New-Run
        $code = Invoke-Wsl (Get-RunnerArguments $run $Mode.ToLowerInvariant())
        Write-Host "Evidence: $($run.Directory)"
        exit $code
    }
    'Session' {
        # Reserve the single active slot atomically before doing any work.
        try {
            $reservation = [IO.File]::Open($activePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        } catch [IO.IOException] {
            throw "A Linux GUI session is already active or starting ($activePath); stop it first."
        }
        $reservation.Dispose()
        $process = $null
        $run = $null
        try {
            $run = New-Run
            # Linux-side redirection: Start-Process redirection would make the
            # detached runner inherit (and hold open) this process's own handles.
            $wrapper = @('bash', '-c', 'out=$1; shift; exec "$@" >"$out/runner.out" 2>&1', 'wrap',
                $run.WslDirectory) + (Get-RunnerArguments $run 'session')
            # Start-Process joins one command line, so every argument
            # (including the distro) is quoted for CommandLineToArgvW.
            $arguments = (@('-d', $Distro, '--exec') + $wrapper) | ForEach-Object {
                if ($_ -eq '' -or $_ -match '[\s"$]') {
                    '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
                } else { $_ }
            }
            $process = Start-Process -FilePath 'wsl.exe' -PassThru -WindowStyle Hidden `
                -ArgumentList $arguments
            $sessionFile = Join-Path $run.Directory 'session.json'
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            while (-not (Test-Path -LiteralPath $sessionFile)) {  # Published atomically.
                if ($process.HasExited) { throw "Session runner exited early; see $($run.Directory)" }
                if ($deadline.Elapsed.TotalSeconds -gt $IdleSeconds + 120) {
                    throw "Session did not become ready; see $($run.Directory)"
                }
                Start-Sleep -Milliseconds 250
            }
            $session = Get-Content -Raw $sessionFile | ConvertFrom-Json
            Write-JsonAtomic ([ordered]@{
                    directory  = $run.Directory
                    sessionDir = $session.sessionDir
                    sourceDir  = $SourceDir
                    envScript  = $envArgument
                    distro     = $Distro
                    processId  = $process.Id
                    processStartTime = $process.StartTime.ToUniversalTime().ToString('o')
                }) $activePath
        } catch {
            $startupError = $_
            # Tear down only the runner this invocation launched (via its own
            # process handle, never a numeric PID), then release the slot.
            try {
                if ($process -and -not $process.HasExited) {
                    try {
                        $sessionFile = Join-Path $run.Directory 'session.json'
                        if (Test-Path -LiteralPath $sessionFile) {
                            $active = [pscustomobject]@{
                                directory = $run.Directory; sourceDir = $SourceDir; envScript = $envArgument
                                sessionDir = (Get-Content -Raw $sessionFile | ConvertFrom-Json).sessionDir
                            }
                            $null = Invoke-BridgeCall '{"op":"stop"}'
                        }
                    } catch {
                        Write-Warning "Bridge stop during failed startup: $_"
                    }
                    if (-not $process.WaitForExit(30000)) {
                        $process.Kill($true)
                        if (-not $process.WaitForExit(10000)) {
                            Write-Warning "The session launcher did not exit after a forced stop."
                        }
                        # The Windows launcher's exit does not prove the Linux side
                        # cleaned up; its run directory records what it completed.
                        Write-Warning "Forced stop; inspect $($run.Directory) for cleanup evidence."
                    }
                }
            } finally {
                Remove-Item -LiteralPath $activePath -ErrorAction SilentlyContinue
            }
            throw $startupError
        }
        Write-Host "Session ready. Evidence: $($run.Directory)"
        Write-Host "Next: -Mode Call -Request '{""op"":""screenshot"",""name"":""start""}'"
    }
    'Call' {
        if (-not $Request) { throw '-Request is required for -Mode Call.' }
        exit (Invoke-BridgeCall $Request)
    }
    'Stop' {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $complete = Join-Path $active.directory 'runner-complete.json'
        if (-not (Test-Path -LiteralPath $complete -PathType Leaf)) {
            $null = Invoke-BridgeCall '{"op":"stop"}'
        }
        if (-not (Wait-Runner $active.processId $active.processStartTime)) {
            throw "Session runner did not finish; see $($active.directory)"
        }
        $remaining = [int][Math]::Max(1, 120000 - $watch.ElapsedMilliseconds)
        $summary = Wait-LayerCGuiResult -Directory $active.directory -Milliseconds $remaining
        # Failed or incomplete publication retains the slot and evidence, so a
        # caller can retry Stop rather than losing the ownership record.
        Remove-Item -LiteralPath $activePath
        $summary | ConvertTo-Json -Depth 64
        exit ([int](-not $summary.passed))
    }
}
