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
