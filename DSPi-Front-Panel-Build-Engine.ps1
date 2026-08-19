[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$ReleaseVersion,
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

$ProjectName = "DSPi-ESP32-Front-Panel"
$SketchName = "DSPi_ESP32_Front_Panel_v1_1_2"
$RequiredEsp32Core = "3.3.11"
$RequiredGfxVersion = "1.6.5"
$RequiredNimbleVersion = "2.5.0"
$PreferredSdFatVersion = "2.3.0"
$FallbackSdFatVersion = ""

if ($BuildOnly -and $FlashOnly) {
    throw "Use either -BuildOnly or -FlashOnly, not both."
}
if ($Erase -and $PreserveSettings) {
    throw "Use either -Erase or -PreserveSettings, not both."
}

# A clean full flash is the safe default for a first installation because it
# writes the correct bootloader and 16 MB partition table. Use
# -PreserveSettings only for a deliberate application-only update.
$CleanFlash = -not $PreserveSettings
if ($Erase) { $CleanFlash = $true }

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$SketchDir = Join-Path $Root "firmware\$SketchName"
$SketchFile = Join-Path $SketchDir ($SketchName + ".ino")
$BuildDir = Join-Path $Root "build\esp32s3"
$WorkDir = Join-Path $BuildDir "work"
$ReleaseDir = Join-Path $Root "release"
# Waveshare ESP32-S3-LCD-2 / ESP32S3 Dev Module hardware profile.
$Fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none"

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$Quiet
    )

    $savedPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5.1 converts native stderr into ErrorRecord values.
        # Keep it non-terminating and trust the real process exit code.
        $ErrorActionPreference = "Continue"
        if ($Quiet) {
            & $Command @Arguments *> $null
        }
        else {
            & $Command @Arguments
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }

    if ($exitCode -ne 0) {
        throw "$Command failed with exit code $exitCode."
    }
}

function Get-NativeText {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $text = (& $Command @Arguments 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($exitCode -ne 0) {
        throw "$Command failed with exit code $exitCode."
    }
    return $text
}

function Ensure-ArduinoLibrary {
    param(
        [Parameter(Mandatory = $true)][string]$LibraryName,
        [Parameter(Mandatory = $true)][string]$RequiredVersion,
        [string]$FallbackVersion = ""
    )

    $libraryList = Get-NativeText -Command "arduino-cli" -Arguments @("lib", "list")
    $escapedName = [Regex]::Escape($LibraryName)
    $match = [Regex]::Match(
        $libraryList,
        "(?m)^\s*$escapedName\s+([^\s]+)\b"
    )

    if ($match.Success -and $match.Groups[1].Value -eq $RequiredVersion) {
        Write-Host "Using $LibraryName $RequiredVersion." -ForegroundColor Green
        return $RequiredVersion
    }

    if ($match.Success) {
        Write-Warning "$LibraryName $($match.Groups[1].Value) is installed; this release was validated with $RequiredVersion."
    }

    if ($SkipLibraryInstall) {
        if ($match.Success) { return $match.Groups[1].Value }
        throw "$LibraryName $RequiredVersion is missing."
    }

    Write-Host "Installing $LibraryName $RequiredVersion..." -ForegroundColor Cyan
    try {
        Invoke-External -Command "arduino-cli" -Arguments @(
            "lib", "install", "$LibraryName@$RequiredVersion"
        )
        return $RequiredVersion
    }
    catch {
        if ([string]::IsNullOrWhiteSpace($FallbackVersion)) { throw }
        Write-Warning "$LibraryName $RequiredVersion was unavailable. Falling back to $FallbackVersion."
        Invoke-External -Command "arduino-cli" -Arguments @(
            "lib", "install", "$LibraryName@$FallbackVersion"
        )
        return $FallbackVersion
    }
}

if (-not (Test-Path -LiteralPath $SketchFile -PathType Leaf)) {
    throw "Sketch not found: $SketchFile"
}

if (-not $FlashOnly) {
    if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
        if (Get-Command winget -ErrorAction SilentlyContinue) {
            Write-Host "Installing Arduino CLI..." -ForegroundColor Cyan
            Invoke-External -Command "winget" -Arguments @(
                "install", "--id", "ArduinoSA.CLI", "--exact",
                "--accept-package-agreements", "--accept-source-agreements"
            )
            $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
            $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
            $env:Path = "$machinePath;$userPath"
        }
        if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
            throw "Arduino CLI is required. Install it from https://arduino.github.io/arduino-cli/ and rerun this script."
        }
    }

    $coreList = Get-NativeText -Command "arduino-cli" -Arguments @("core", "list")
    $exactCorePattern = "(?m)^esp32:esp32\s+$([Regex]::Escape($RequiredEsp32Core))\b"
    if ($coreList -notmatch $exactCorePattern) {
        if ($SkipLibraryInstall) {
            throw "ESP32 Arduino core $RequiredEsp32Core is missing. Install it with: arduino-cli core install esp32:esp32@$RequiredEsp32Core"
        }
        Write-Host "Installing ESP32 Arduino core $RequiredEsp32Core..." -ForegroundColor Cyan
        Invoke-External -Command "arduino-cli" -Arguments @(
            "core", "install", "esp32:esp32@$RequiredEsp32Core"
        )
        $coreList = Get-NativeText -Command "arduino-cli" -Arguments @("core", "list")
        if ($coreList -notmatch $exactCorePattern) {
            throw "ESP32 Arduino core $RequiredEsp32Core was not installed successfully."
        }
    }
    Write-Host "Using ESP32 Arduino core $RequiredEsp32Core." -ForegroundColor Green

    $gfxVersion = Ensure-ArduinoLibrary -LibraryName "GFX Library for Arduino" -RequiredVersion $RequiredGfxVersion
    $nimbleVersion = Ensure-ArduinoLibrary -LibraryName "NimBLE-Arduino" -RequiredVersion $RequiredNimbleVersion
    $sdFatVersion = Ensure-ArduinoLibrary -LibraryName "SdFat" -RequiredVersion $PreferredSdFatVersion -FallbackVersion $FallbackSdFatVersion

    if (Test-Path -LiteralPath $BuildDir) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
    New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null

    Write-Host ""
    Write-Host "Compiling DSPi ESP32 Front Panel v$ReleaseVersion..." -ForegroundColor Cyan
    Write-Host "Board: ESP32S3 Dev Module / Waveshare ESP32-S3-LCD-2"
    Write-Host "Flash: QIO, 16 MB; partition: 3 MB app / 9.9 MB FATFS"
    Write-Host "PSRAM: OPI"
    Write-Host "ESP32 core: $RequiredEsp32Core"
    Write-Host "GFX Library for Arduino: $gfxVersion"
    Write-Host "NimBLE-Arduino: $nimbleVersion"
    Write-Host "SdFat: $sdFatVersion (FAT16/FAT32/exFAT)"
    Write-Host "SD path: playback-safe shared SPI, 20 MHz preferred with 10/4 MHz fallback"
    Write-Host ""

    Invoke-External -Command "arduino-cli" -Arguments @(
        "compile",
        "--fqbn", $Fqbn,
        "--build-property", "compiler.cpp.extra_flags=-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING",
        "--export-binaries",
        "--build-path", $WorkDir,
        "--output-dir", $BuildDir,
        $SketchDir
    )
}

$MergedBin = Get-ChildItem -LiteralPath $BuildDir -Filter "*.ino.merged.bin" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
$AppBin = Get-ChildItem -LiteralPath $BuildDir -Filter "*.ino.bin" -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notmatch "bootloader|partitions|merged" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
$ElfFile = Get-ChildItem -LiteralPath $BuildDir -Filter "*.ino.elf" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
$MapFile = Get-ChildItem -LiteralPath $BuildDir -Filter "*.ino.map" -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $MergedBin) {
    $releaseFull = Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion-Full.bin"
    if (Test-Path -LiteralPath $releaseFull) { $MergedBin = Get-Item -LiteralPath $releaseFull }
}
if (-not $AppBin) {
    $releaseApp = Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion.bin"
    if (Test-Path -LiteralPath $releaseApp) { $AppBin = Get-Item -LiteralPath $releaseApp }
}

if (-not $MergedBin) {
    throw "Merged firmware was not found. Run this script without -FlashOnly first."
}
if (-not $AppBin) {
    throw "Application firmware was not found. Run this script without -FlashOnly first."
}
if ($MergedBin.Length -lt 16000000) {
    throw "Merged image is only $($MergedBin.Length) bytes. Refusing to flash an image that is not the expected 16 MB build."
}

if (-not $FlashOnly) {
    $ReleaseFull = Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion-Full.bin"
    $ReleaseApp = Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion.bin"
    Copy-Item -LiteralPath $MergedBin.FullName -Destination $ReleaseFull -Force
    Copy-Item -LiteralPath $AppBin.FullName -Destination $ReleaseApp -Force
    if ($ElfFile) {
        Copy-Item -LiteralPath $ElfFile.FullName -Destination (Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion.elf") -Force
    }
    if ($MapFile) {
        Copy-Item -LiteralPath $MapFile.FullName -Destination (Join-Path $ReleaseDir "$ProjectName-v$ReleaseVersion.map") -Force
    }
    $MergedBin = Get-Item -LiteralPath $ReleaseFull
    $AppBin = Get-Item -LiteralPath $ReleaseApp

    $checksumRows = New-Object System.Collections.Generic.List[string]
    $checksumRows.Add(("{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $ReleaseFull).Hash, (Split-Path -Leaf $ReleaseFull)))
    $checksumRows.Add(("{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $ReleaseApp).Hash, (Split-Path -Leaf $ReleaseApp)))
    $checksumRows | Set-Content -LiteralPath (Join-Path $ReleaseDir "SHA256SUMS-v$ReleaseVersion.txt") -Encoding ASCII
}

Write-Host ""
Write-Host "Firmware ready:" -ForegroundColor Green
Write-Host "  Full:   $($MergedBin.FullName)"
Write-Host "  Bytes:  $($MergedBin.Length)"
Write-Host "  SHA256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $MergedBin.FullName).Hash)"
Write-Host "  App:    $($AppBin.FullName)"
Write-Host "  SHA256: $((Get-FileHash -Algorithm SHA256 -LiteralPath $AppBin.FullName).Hash)"
if ($ElfFile) { Write-Host "  ELF:    $($ElfFile.FullName)" }
if ($MapFile) { Write-Host "  MAP:    $($MapFile.FullName)" }

if ($BuildOnly) {
    Write-Host ""
    Write-Host "Build completed; no serial port was opened and nothing was flashed." -ForegroundColor Green
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "No COM port was supplied."
}

$Python = $null
if (Get-Command py -ErrorAction SilentlyContinue) {
    $Python = "py"
}
elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $Python = "python"
}
else {
    throw "Python was not found."
}

try {
    Invoke-External -Command $Python -Arguments @("-m", "esptool", "version") -Quiet
}
catch {
    if ($Python -eq "py") {
        Invoke-External -Command $Python -Arguments @("-m", "pip", "install", "--user", "esptool")
    }
    else {
        Invoke-External -Command $Python -Arguments @("-m", "pip", "install", "--user", "esptool")
    }
}

Write-Host ""
Write-Host "Port: $Port"
Write-Host "Baud: $Baud"

if ($CleanFlash) {
    Write-Host "Mode: clean full flash with the correct bootloader and 16 MB partition table." -ForegroundColor Yellow
    Invoke-External -Command $Python -Arguments @(
        "-m", "esptool",
        "--chip", "esp32s3",
        "--port", $Port,
        "--baud", "$Baud",
        "--before", "default-reset",
        "--after", "no-reset",
        "erase-flash"
    )

    Invoke-External -Command $Python -Arguments @(
        "-m", "esptool",
        "--chip", "esp32s3",
        "--port", $Port,
        "--baud", "$Baud",
        "--before", "default-reset",
        "--after", "hard-reset",
        "write-flash",
        "0x0", $MergedBin.FullName
    )
}
else {
    Write-Host "Mode: application-only update; NVS settings are preserved." -ForegroundColor Green
    Invoke-External -Command $Python -Arguments @(
        "-m", "esptool",
        "--chip", "esp32s3",
        "--port", $Port,
        "--baud", "$Baud",
        "--before", "default-reset",
        "--after", "hard-reset",
        "write-flash",
        "0x10000", $AppBin.FullName
    )
}

Write-Host ""
Write-Host "DSPi ESP32 Front Panel v$ReleaseVersion flashed successfully." -ForegroundColor Green
if ($CleanFlash) {
    Write-Host "IMPORTANT: unplug all power for at least 10 seconds, then reconnect before testing." -ForegroundColor Yellow
}
