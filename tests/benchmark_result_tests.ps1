$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../tools/LayerC.Tools.psm1') -Force
$path = Join-Path ([IO.Path]::GetTempPath()) ('layerc-benchmark-' + [guid]::NewGuid() + '.json')
try {
    $result = New-LayerCBenchmarkResult -OutputPath $path `
        -Data ([ordered]@{ evaluator = @{ p99Microseconds = 42 } }) `
        -Thresholds ([ordered]@{ hardP99Microseconds = 2000 }) `
        -Passed $true -HostData ([ordered]@{ gpuAdapters = @('test GPU') })
    $json = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    foreach ($key in @('schemaVersion', 'packageVersion', 'commitSha', 'measuredAt',
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
    Write-Output 'Benchmark result serialization passed.'
} finally {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
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
