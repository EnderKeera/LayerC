$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../tools/LayerC.Tools.psm1') -Force
$path = Join-Path ([IO.Path]::GetTempPath()) ('layerc-benchmark-' + [guid]::NewGuid() + '.json')
$buildDirectory = Join-Path ([IO.Path]::GetTempPath()) ('layerc-build-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $buildDirectory
$savedEnvironment = @{}
foreach ($name in @('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_WORKFLOW')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "Unexpected error: $($_.Exception.Message)" }
        return
    }
    throw "Expected an error matching '$Pattern'."
}
try {
    $head = Get-LayerCCommitSha
    # An unrelated workflow-event SHA must never name the measured build.
    $env:GITHUB_SHA = 'f' * 40

    $local = Get-LayerCBuildIdentity -BuildDirectory $buildDirectory
    if ($local.source -ne 'local-build' -or $local.commitSha -ne $head -or $local.dirty -isnot [bool]) {
        throw "Local build identity is wrong: $($local | ConvertTo-Json)"
    }

    $env:GITHUB_RUN_ID = '123'
    $env:GITHUB_RUN_ATTEMPT = '2'
    $env:GITHUB_WORKFLOW = 'CI'
    $written = Write-LayerCBuildIdentity -BuildDirectory $buildDirectory -Configuration ci
    $read = Get-LayerCBuildIdentity -BuildDirectory $buildDirectory
    if ($read.source -ne 'ci-artifact' -or $read.commitSha -ne $head -or $read.runId -ne '123' -or
        $read.runAttempt -ne '2' -or $read.configuration -ne 'ci' -or $read.dirty -ne $written.dirty) {
        throw "CI build identity did not round-trip: $($read | ConvertTo-Json)"
    }
    $null = Assert-LayerCBuildIdentity -BuildDirectory $buildDirectory -CommitSha $head -RunId '123'
    Assert-Throws { Assert-LayerCBuildIdentity -BuildDirectory $buildDirectory -CommitSha ('a' * 40) -RunId '123' } 'not candidate'
    Assert-Throws { Assert-LayerCBuildIdentity -BuildDirectory $buildDirectory -CommitSha $head -RunId '999' } 'not candidate'
    $other = Join-Path $buildDirectory 'other'
    $null = New-Item -ItemType Directory -Path $other
    Assert-Throws { Assert-LayerCBuildIdentity -BuildDirectory $other -CommitSha $head -RunId '123' } 'no CI build identity'

    # Downloaded binaries keep the commit that built them, not the checkout.
    $artifact = [ordered]@{ schemaVersion = 1; source = 'ci-artifact'; commitSha = 'a' * 40
        dirty = $false; workflow = 'CI'; runId = '77'; runAttempt = '1'; configuration = 'ci' }
    Set-Content -LiteralPath (Join-Path $other 'layerc-build-identity.json') -Value ($artifact | ConvertTo-Json)
    Assert-Throws { Assert-LayerCBuildIdentity -BuildDirectory $other -CommitSha ('a' * 40) -RunId '77' } 'checkout is'

    $result = New-LayerCBenchmarkResult -OutputPath $path -BuildDirectory $other `
        -Data ([ordered]@{ evaluator = @{ p99Microseconds = 42 } }) `
        -Thresholds ([ordered]@{ hardP99Microseconds = 2000 }) `
        -Passed $true -HostData ([ordered]@{ gpuAdapters = @('test GPU') })
    $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($json.commitSha -ne ('a' * 40) -or $json.build.source -ne 'ci-artifact' -or $json.build.runId -ne '77') {
        throw "Benchmark result names the wrong build: $($json.build | ConvertTo-Json)"
    }
    foreach ($key in @('schemaVersion', 'packageVersion', 'commitSha', 'build', 'measuredAt',
            'host', 'evaluator', 'thresholds', 'passed')) {
        if ($null -eq $json.PSObject.Properties[$key]) { throw "Missing benchmark key: $key" }
    }
    foreach ($key in @('computerName', 'os', 'logicalProcessors', 'gpuAdapters')) {
        if ($null -eq $json.host.PSObject.Properties[$key]) { throw "Missing host key: $key" }
    }
    if ($json.packageVersion -ne (Get-LayerCPackageVersion) -or $json.evaluator.p99Microseconds -ne 42 -or
        $json.thresholds.hardP99Microseconds -ne 2000 -or -not $json.passed -or
        $json.host.gpuAdapters[0] -ne 'test GPU' -or $result.host -isnot [Collections.IDictionary]) {
        throw 'Benchmark result values were not preserved.'
    }
    Write-Output 'Benchmark result serialization and build identity passed.'
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value)
    }
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

# push-and-test.ps1 forwards to run-hardware-benchmarks.ps1; bind its real
# param block without running any benchmark.
$runner = Join-Path $PSScriptRoot '../tools/run-hardware-benchmarks.ps1'
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($runner, [ref]$null, [ref]$parseErrors)
if ($parseErrors) { throw "Could not parse ${runner}: $parseErrors" }
$capture = [scriptblock]::Create($ast.ParamBlock.Extent.Text + "`n`$PSBoundParameters")
$visual = New-LayerCHardwareBenchmarkArguments -CandidateBuildDirectory 'C:\candidate' `
    -BaselineBuildDirectory 'C:\ignored' -VisualOnly
$bound = & $capture @visual
if ($bound.CandidateBuildDirectory -ne 'C:\candidate' -or -not $bound.VisualOnly -or
    $bound.ContainsKey('BaselineBuildDirectory')) {
    throw "Visual-only benchmark arguments bound incorrectly: $($bound | Out-String)"
}
$compare = New-LayerCHardwareBenchmarkArguments -CandidateBuildDirectory 'C:\candidate' `
    -BaselineBuildDirectory 'C:\baseline'
$bound = & $capture @compare
if ($bound.CandidateBuildDirectory -ne 'C:\candidate' -or
    $bound.BaselineBuildDirectory -ne 'C:\baseline' -or $bound.ContainsKey('VisualOnly')) {
    throw "Baseline benchmark arguments bound incorrectly: $($bound | Out-String)"
}
Write-Output 'Hardware benchmark argument forwarding passed.'

# Benchmarks keep settings in a disposable key and never name the user's.
$repository = Split-Path -Parent $PSScriptRoot
$production = Get-ChildItem -Path (Join-Path $repository 'tools') -Include *.ps1, *.psm1 -Recurse |
    Select-String -Pattern 'HKCU:\\Software\\LayerC([''"\\]|$)'
if ($production) { throw "Benchmark tooling addresses the user's settings key: $production" }
if ($IsWindows) {
    $sandbox = Join-Path ([IO.Path]::GetTempPath()) ('LayerC-sandbox-test-' + [guid]::NewGuid())
    $previousProfiles = [Environment]::GetEnvironmentVariable('LAYERC_PROFILE_DIR', 'Process')
    $null = New-Item -ItemType Directory -Path $sandbox
    Assert-Throws { Set-LayerCSandboxSetting -Name 'VisualRecognitionEnabled' -Value 1 } 'only inside a benchmark sandbox'
    Enter-LayerCProfileSandbox -ProfileDirectory (Join-Path $sandbox 'profiles')
    try {
        $key = [Environment]::GetEnvironmentVariable('LAYERC_SETTINGS_KEY', 'Process')
        if ($key -ne ('Software\LayerC.Benchmark\' + (Split-Path -Leaf $sandbox))) {
            throw "Sandbox settings key is wrong: $key"
        }
        Set-LayerCSandboxSetting -Name 'VisualRecognitionEnabled' -Value 1
        $value = (Get-ItemProperty -LiteralPath "HKCU:\$key" -Name 'VisualRecognitionEnabled').VisualRecognitionEnabled
        if ($value -ne 1) { throw 'Sandbox setting was not written to the sandbox key.' }
    } finally {
        Exit-LayerCProfileSandbox -PreviousDirectory $previousProfiles -TemporaryDirectory $sandbox
    }
    if (Test-Path -LiteralPath "HKCU:\$key") { throw 'Sandbox settings key outlived the sandbox.' }
    if ($null -ne [Environment]::GetEnvironmentVariable('LAYERC_SETTINGS_KEY', 'Process')) {
        throw 'LAYERC_SETTINGS_KEY outlived the sandbox.'
    }
    Write-Output 'Benchmark settings sandbox passed.'
}
