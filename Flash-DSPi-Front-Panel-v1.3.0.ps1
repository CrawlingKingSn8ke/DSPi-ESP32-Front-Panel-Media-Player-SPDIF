[CmdletBinding()]
param(
    [string]$Port = "COM10",
    [int]$Baud = 921600,
    [switch]$Erase,
    [switch]$PreserveSettings,
    [switch]$BuildOnly,
    [switch]$FlashOnly,
    [switch]$SkipLibraryInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Engine = Join-Path $Root "Build-and-Flash-DSPi-Front-Panel-v1.3.0.ps1"

if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
    throw "v1.3.0 build and flash engine not found: $Engine"
}

$arguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $Engine,
    "-Port", $Port,
    "-Baud", $Baud
)
if ($Erase) { $arguments += "-Erase" }
if ($PreserveSettings) { $arguments += "-PreserveSettings" }
if ($BuildOnly) { $arguments += "-BuildOnly" }
if ($FlashOnly) { $arguments += "-FlashOnly" }
if ($SkipLibraryInstall) { $arguments += "-SkipLibraryInstall" }

& powershell.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw "DSPi ESP32 Front Panel v1.3.0 failed with exit code $LASTEXITCODE."
}
