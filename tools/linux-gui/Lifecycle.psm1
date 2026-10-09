# The Windows launcher can exit before WSL finishes publishing its evidence.
# A completed, hash-bound bundle is the handoff, not a Windows PID disappearing.
Set-StrictMode -Version Latest

function Wait-LayerCGuiResult {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [ValidateRange(1, 120000)][int]$Milliseconds = 120000
    )
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $completion = Join-Path $Directory 'runner-complete.json'
    while (-not (Test-Path -LiteralPath $completion -PathType Leaf)) {
        if ($watch.ElapsedMilliseconds -ge $Milliseconds) {
            throw "Session evidence was not completed: $Directory"
        }
        Start-Sleep -Milliseconds 25
    }

    # The producer renames this marker into place only after cleanup, result
    # generation, and the manifest. Reject malformed/corrupt publication.
    $record = Get-Content -LiteralPath $completion -Raw | ConvertFrom-Json
    if ($record.version -ne 1 -or
        ($record.exitCode -isnot [long] -and $record.exitCode -isnot [int]) -or
        $record.exitCode -lt 0 -or $record.exitCode -gt 255) {
        throw "Invalid session completion record: $Directory"
    }
    foreach ($entry in @(
        @{ Name = 'result.json'; Digest = $record.resultSha256 },
        @{ Name = 'SHA256SUMS'; Digest = $record.manifestSha256 }
    )) {
        $path = Join-Path $Directory $entry.Name
        if ($entry.Digest -isnot [string] -or $entry.Digest -cnotmatch '^[0-9a-fA-F]{64}$' -or
            -not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Digest) {
            throw "Incomplete or changed session evidence ($($entry.Name)): $Directory"
        }
    }
    $result = Get-Content -LiteralPath (Join-Path $Directory 'result.json') -Raw | ConvertFrom-Json
    if ($result.mode -ne 'session' -or $result.passed -isnot [bool] -or
        ($result.passed -and $record.exitCode -ne 0)) {
        throw "Invalid session result or producer status: $Directory"
    }
    return $result
}

Export-ModuleMember -Function Wait-LayerCGuiResult
