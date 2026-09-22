$ErrorActionPreference = 'Stop'
$checker = Join-Path $PSScriptRoot '../tools/check-test-results.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('layerc-test-report-' + [guid]::NewGuid() + '.xml')
$scenarios = @(
    @{ Xml = '<testsuite><testcase name="required" status="run" /></testsuite>'; Pass = $true },
    @{ Xml = '<testsuite />'; Pass = $false },
    @{ Xml = '<testsuite><testcase name="other" status="run" /></testsuite>'; Pass = $false },
    @{ Xml = '<testsuite><testcase name="required"><failure message="broken" /></testcase></testsuite>'; Pass = $false },
    @{ Xml = '<testsuite><testcase name="required"><skipped /></testcase></testsuite>'; Pass = $false },
    @{ Xml = '<testsuite><testcase name="required" status="notrun" /></testsuite>'; Pass = $false },
    @{ Xml = '<testsuite><testcase name="required"><skipped /></testcase></testsuite>'; Pass = $true; Allowed = @('required') }
)
try {
    foreach ($scenario in $scenarios) {
        Set-Content -LiteralPath $fixture -Value $scenario.Xml
        $accepted = $true
        try {
            & $checker -Path $fixture -RequiredTests @('required') -AllowedSkips @($scenario.Allowed) | Out-Null
        } catch {
            $accepted = $false
        }
        if ($accepted -ne $scenario.Pass) {
            throw "CTest result policy returned an incorrect verdict for: $($scenario.Xml)"
        }
    }
    Write-Output "Passed $($scenarios.Count) CTest result policy scenarios."
} finally {
    Remove-Item -LiteralPath $fixture -Force -ErrorAction SilentlyContinue
}
