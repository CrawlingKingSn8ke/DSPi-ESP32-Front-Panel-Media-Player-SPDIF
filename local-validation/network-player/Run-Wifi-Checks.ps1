param([Parameter(Mandatory=$true)][string]$Zig)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$out=Join-Path $repo 'build/wifi-checks'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$exe=Join-Path $out 'Test-NetworkSleep.exe'
& $Zig c++ -std=c++17 -O0 -UNDEBUG (Join-Path $PSScriptRoot 'Test-NetworkSleep.cpp') -I (Join-Path $repo 'local-validation/link-host') (Join-Path $repo 'firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelWifi.cpp') -o $exe
if($LASTEXITCODE){throw 'Wi-Fi regression compile failed'}
& $exe
if($LASTEXITCODE){throw 'Wi-Fi regression failed'}
