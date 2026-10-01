[CmdletBinding()]
param(
    [switch]$SkipLibraryInstall
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Engine = Join-Path $Root "Build-and-Flash-DSPi-Front-Panel-v1.3.0.ps1"

if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
    throw "v1.3.0 build engine not found: $Engine"
}

$arguments = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $Engine,
    "-BuildOnly"
)
if ($SkipLibraryInstall) { $arguments += "-SkipLibraryInstall" }

& powershell.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw "DSPi ESP32 Front Panel v1.3.0 build failed with exit code $LASTEXITCODE."
}
