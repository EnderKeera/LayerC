param(
  [Parameter(Mandatory = $true)]
  [string]$Source,
  [Parameter(Mandatory = $true)]
  [string]$Destination
)

$ErrorActionPreference = 'Stop'
$encoded = [System.IO.File]::ReadAllText($Source).Trim()
$bytes = [Convert]::FromBase64String($encoded)
[System.IO.File]::WriteAllBytes($Destination, $bytes)
