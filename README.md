# DSPi ESP32 Media Player — S/PDIF Test Firmware

Experimental fork of the DSPi ESP32 Front Panel that sends the integrated SD-card music player to a WeebLabs DSPi over a single-wire, 24-bit, 44.1/48 kHz logic-level S/PDIF connection.

> [!IMPORTANT]
> This repository is an isolated test build, not the stable release of the main front-panel project. The ESP transmitter is stable in current testing when paired with the experimental DSPi S/PDIF receiver overhaul; official DSPi integration is still pending. See [EXPERIMENTAL-SPDIF.md](EXPERIMENTAL-SPDIF.md) before flashing or wiring it.

The complete front-panel interface, themes, presets, BLE remote support, local Wi-Fi transfer/update portal and DSPi v1.1.5/V28 support remain included.

## Version 1.2.1 S/PDIF Forensic r2

Forensic r2 is the current tested build. It retains the complete VU IMP interface, presets, SD music player and local Wi-Fi transfer/update portal while hardening the experimental 24-bit S/PDIF transmitter and continuous same-rate carrier transitions.

### Main changes

- Independent Main Text, Accent, Volume Meters and Analog VU colour roles.
- Thirty-colour text/meter palette plus six shaded analogue VU face choices.
- Cached custom analogue faces for cyan-speed needle animation without changing the approved artwork or calibration.
- Reworked System, Screen, Idle Screen, Theme and ten-slot Preset list interfaces.
- Five-second Select hold saves a complete DSPi/panel preset; normal selection remains in the preset list.
- Confirmed DSPi Console changes update the Home state and use the same full-screen notifications as local controls.
- Music playback uses the normal I2S Home state, with automatic route ownership and reliable 44.1/48 kHz operation.
- Hardened 48 kHz decoder scheduling, BLE reconnect deferral, background-artwork limits and underrun telemetry.
- Standards-checked 24-bit consumer S/PDIF at 44.1/48 kHz with a complete encoded-silence DMA preload.
- Atomic carrier-retention lifecycle and exact continuation after partial DMA writes during track transitions.
- RAM-only S/PDIF timeout, partial-write, error, restart and retained-transition counters.
- Preserves the v1.1.3 S/PDIF 4 compatibility, Wi-Fi Music Transfer, BLE remote and SD music-player features.

See [CHANGELOG-v1.2.1.md](CHANGELOG-v1.2.1.md) for the maintenance summary and [EXPERIMENTAL-SPDIF.md](EXPERIMENTAL-SPDIF.md) for the evidence boundary. The `SHA256SUMS-v1.2.1-SPDIF-Forensic-r2.txt` release asset contains the verified firmware checksums.

## Hardware

- Waveshare ESP32-S3-LCD-2, 320 x 240.
- Raspberry Pi Pico or Pico 2 running compatible DSPi firmware.
- Mechanical rotary encoder with push switch.
- Optional BLE HID remote.
- microSD card formatted as FAT32 or exFAT.

## DSPi compatibility

- DSPi firmware v1.1.5/V28 and compatible v1.1.6 beta builds: four S/PDIF inputs and verified runtime notifications are supported.
- Earlier compatible DSPi firmware: the fourth S/PDIF source remains hidden when only three inputs are reported.

## Wiring

### ESP32 to DSPi UART

| ESP32-S3-LCD-2 | DSPi Pico 2 | Function |
|---|---|---|
| GPIO16 | GPIO16 | ESP32 RX from DSPi TX |
| GPIO17 | GPIO17 | ESP32 TX to DSPi RX |
| GND | GND | Common ground |

The UART runs at 115200 baud. Enable the DSPi UART interface with TX GPIO16 and RX GPIO17.

### ESP32 music-player S/PDIF to DSPi

| ESP32-S3-LCD-2 | DSPi Pico / Pico 2 | Function |
|---|---|---|
| GPIO13 | GPIO5 | Logic-level S/PDIF data to DSPi S/PDIF input 1 |
| GND | GND | Common ground |

Configure DSPi S/PDIF input 1 on Pico GPIO5. The firmware validates this setting before starting playback. The test connection is short, direct 3.3 V logic with a common ground; no coaxial or optical line driver is implied. Do not connect this GPIO-level signal to consumer coaxial S/PDIF equipment.

### Rotary encoder

| Encoder | ESP32-S3-LCD-2 |
|---|---|
| CLK / A | GPIO47 |
| DT / B | GPIO48 |
| Push switch | GPIO21 |
| Common / switch return | GND |

Power encoder modules from 3.3 V, not 5 V.

## Flash on Windows

1. Download the v1.2.1 release source or clone `main`.
2. Install Python 3 if `py --version` does not show a version.
3. Connect the ESP32-S3-LCD-2 by USB.
4. Close Arduino Serial Monitor and any program using the COM port.
5. Open PowerShell in the project folder and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Flash-DSPi-Front-Panel-v1.2.1.ps1"
```

The script asks for or uses the supplied COM port, installs the required build tools when needed, compiles the firmware and performs a clean flash.

To update only the application while preserving BLE pairing and panel settings:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Flash-DSPi-Front-Panel-v1.2.1.ps1" -PreserveSettings
```

A clean flash erases BLE pairing, learned key mappings, brightness, screen-power settings and shortcut assignments. After a clean flash, disconnect all power for at least 10 seconds before reconnecting.

## Build from source

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Build-DSPi-Front-Panel-v1.2.1.ps1"
```

The script installs or verifies:

- ESP32 Arduino core 3.3.11
- GFX Library for Arduino 1.6.5
- NimBLE-Arduino 2.5.0
- SdFat 2.3.0

The Arduino sketch retains its historical v1.1.2 directory name so the existing Arduino project and release history remain stable:

```text
firmware\DSPi_ESP32_Front_Panel_v1_1_2\DSPi_ESP32_Front_Panel_v1_1_2.ino
```

The v1.2.1 wrappers use the version-neutral build engine and name generated artifacts as v1.2.1.

Board profile:

```text
Board: ESP32S3 Dev Module
USB Mode: Hardware CDC and JTAG
USB CDC On Boot: Enabled
CPU Frequency: 240 MHz
Flash Mode: QIO
Flash Size: 16 MB
Partition Scheme: 16M Flash (3MB APP / 9.9MB FATFS)
PSRAM: OPI
```

## Music playback

Supported audio formats:

- WAV
- FLAC
- MP3

The player supports folder browsing, artwork, pause/resume, seeking, previous/next track and automatic track advance. Normal playback uses the shared SD interface at 20 MHz, with lower-speed fallbacks when required.

Restrictions:

- Playback and Wi-Fi Music Transfer cannot use the SD card at the same time.
- Stop playback before starting Wi-Fi Music Transfer.
- Very large or unusual artwork may be skipped if it cannot be decoded safely.
- Unsupported, corrupt or unusually encoded files may be skipped.
- Removing the card while mounted or playing is not supported.
- After a genuine SD-card fault, remove all power before retrying.

## Wi-Fi Music Transfer

Open the Music Transfer option on the panel and confirm Start. The panel displays the Wi-Fi connection details and browser address.

From a phone or computer:

1. Connect to the network shown by the panel, or use the configured home network mode.
2. Open the displayed address in a browser.
3. Browse the SD card, create folders, upload files or delete files and folders.
4. Wait for all transfers to complete.
5. Select **Finish Safely** before returning to normal playback.

Typical transfer performance is approximately **0.45 to 0.60 MB/s**, depending on the microSD card, Wi-Fi conditions, browser and file size. Transfer speed is intentionally limited by the shared SPI SD interface and safe write handling.

Wi-Fi transfer restrictions:

- Only one upload is written at a time.
- Playback is unavailable while transfer mode owns the SD card.
- Do not remove power or the SD card during an upload.
- Use Finish Safely before returning to the player.
- A full power cycle may be required before a previously paired BLE remote reconnects after transfer mode.

## Notes

- DSPi control uses UART. Local SD-card music is also encoded by the ESP32 and sent to DSPi over the dedicated GPIO13 S/PDIF link.
- Input choices and DSP features depend on the connected DSPi firmware and configuration.
- The VU meters show DSPi output telemetry, not the volume-control position.
