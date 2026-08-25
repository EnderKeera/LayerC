param(
  [Parameter(Mandatory = $true)] [string]$WidgetOutput,
  [Parameter(Mandatory = $true)] [string]$Destination,
  [Parameter(Mandatory = $true)] [string]$Manifest,
  [Parameter(Mandatory = $true)] [string]$Asset,
  [Parameter(Mandatory = $true)] [string]$GameBarPrivateWinmd,
  [Parameter(Mandatory = $true)] [string]$DesktopRuntimeDirectory,
  [Parameter(Mandatory = $true)] [string]$LayerD,
  [Parameter(Mandatory = $true)] [string]$LayerE,
  [Parameter(Mandatory = $true)] [string]$LayerCal,
  [Parameter(Mandatory = $true)] [string]$LayerWorker
)

$ErrorActionPreference = 'Stop'
if (Test-Path $Destination) { Remove-Item -Recurse -Force $Destination }
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$payload = @(
  (Join-Path $WidgetOutput 'AppX'),
  (Join-Path $WidgetOutput 'LayerC.GameBar'),
  $WidgetOutput
) | Where-Object {
  Test-Path (Join-Path $_ 'layerc-gamebar.exe')
} | Select-Object -First 1
if (-not $payload) {
  throw "UWP widget output does not contain layerc-gamebar.exe: $WidgetOutput"
}
$payloadRoot = (Resolve-Path $payload).Path.TrimEnd([char]'\')

$excluded = @('.appxrecipe', '.exp', '.ilk', '.lib', '.pdb')
Get-ChildItem -Path $payload -Recurse -File | Where-Object {
  $excluded -notcontains $_.Extension.ToLowerInvariant()
} | ForEach-Object {
  $relative = $_.FullName.Substring($payloadRoot.Length).TrimStart([char]'\')
  $target = Join-Path $Destination $relative
  New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($target)) | Out-Null
  Copy-Item -Force $_.FullName $target
}

Copy-Item -Force $Manifest (Join-Path $Destination 'AppxManifest.xml')
$assetDirectory = Join-Path $Destination 'Assets'
New-Item -ItemType Directory -Force -Path $assetDirectory | Out-Null
Copy-Item -Force $Asset (Join-Path $assetDirectory 'StoreLogo.png')
$requiredDesktopRuntime = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
foreach ($name in $requiredDesktopRuntime) {
  $runtimePath = Join-Path $DesktopRuntimeDirectory $name
  if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "MSVC desktop runtime is incomplete: $runtimePath"
  }
}
Get-ChildItem -LiteralPath $DesktopRuntimeDirectory -Filter '*.dll' -File | ForEach-Object {
  Copy-Item -Force -LiteralPath $_.FullName -Destination (Join-Path $Destination $_.Name)
}
Copy-Item -Force $LayerD (Join-Path $Destination 'layerd.exe')
Copy-Item -Force $GameBarPrivateWinmd (Join-Path $Destination 'Microsoft.Gaming.XboxGameBar.Private.winmd')
Copy-Item -Force $LayerE (Join-Path $Destination 'layere.exe')
Copy-Item -Force $LayerCal (Join-Path $Destination 'layercal.exe')
Copy-Item -Force $LayerWorker (Join-Path $Destination 'layerworker.exe')
