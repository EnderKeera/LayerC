param(
    [Parameter(Mandatory)][string]$Path,
    [string[]]$RequiredTests = @(),
    [string[]]$AllowedSkips = @()
)

$ErrorActionPreference = 'Stop'
[xml]$report = Get-Content -LiteralPath $Path -Raw
$cases = @($report.SelectNodes('//testcase'))
if ($cases.Count -eq 0) { throw 'CTest report contains no test cases.' }
$names = @($cases | ForEach-Object { $_.GetAttribute('name') })
foreach ($required in $RequiredTests) {
    if ($required -notin $names) { throw "Required suite is missing: $required" }
}
foreach ($case in $cases) {
    $name = $case.GetAttribute('name')
    if ($null -ne $case.SelectSingleNode('failure|error')) {
        throw "Test failed: $name"
    }
    $skipped = $null -ne $case.SelectSingleNode('skipped')
    if ($skipped) {
        if ($name -notin $AllowedSkips) { throw "Unexpected skipped suite: $name" }
        Write-Warning "Interactive coverage unavailable: $name (explicit hosted-runner exception)"
    } elseif ($case.GetAttribute('status') -in @('notrun', 'disabled')) {
        throw "Test did not run: $name"
    }
}
Write-Output "Verified $($cases.Count) CTest results and $($RequiredTests.Count) required suites."
