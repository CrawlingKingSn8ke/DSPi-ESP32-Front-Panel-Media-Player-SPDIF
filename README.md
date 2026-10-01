# DSPi ESP32 Front Panel — Media Player S/PDIF

Stable ESP32-S3 front-panel firmware with an integrated SD-card music player and a single-wire, 24-bit, 44.1/48 kHz logic-level S/PDIF connection to a WeebLabs DSPi.

> [!IMPORTANT]
> The ESP32-to-DSPi audio link is direct 3.3 V GPIO-level S/PDIF. It is not a consumer coaxial or optical connection. Read [S/PDIF implementation and wiring](SPDIF-IMPLEMENTATION.md) before connecting the hardware.

The complete front-panel interface, themes, presets, BLE remote support, local Wi-Fi transfer/update portal and DSPi v1.1.5/V28 support remain included.

## Version 1.3.0

This release builds on the stable v1.2.1 r2 S/PDIF media player. The audio route, album artwork, presets, BLE remote, analogue VU and local Wi-Fi updater remain available.

### New since v1.2.1 r2

- **Tube Modeller** Basic controls: enable, tube choice, Drive, Mix and DSPi-enabled output selection. Advanced values saved in DSPi Console remain owned by DSPi and are not reset by the panel.
- **Output Limiter** controls in the Setup menu, with DSPi-enabled outputs. Tube and Limiter settings follow the connected DSPi's state and presets.
- **Stereo Spectrum** replaces the old digital bar VU page. Select the input or output channel pair from channels enabled in the DSPi matrix; only the visible pair is requested. Each channel has its own colour palette, with animated peak markers. The analogue VU page remains available.
- A reorganised **Setup** menu, clearer Remote key mapping, continuation arrows for longer lists, and updated Home feature icons and notifications. Spectrum retains source/preset information and supports the normal volume, mute and feature overlays.

Tube Modeller, Output Limiter and Spectrum require a compatible DSPi v1.1.6 firmware exposing those controls. The previous [v1.2.1 r2 release](https://github.com/CrawlingKingSn8ke/DSPi-ESP32-Front-Panel-Media-Player-SPDIF/releases/tag/v1.2.1-r2) remains available for rollback.

See [v1.3.0 release notes](RELEASE-NOTES-v1.3.0.md) for the feature summary and [S/PDIF implementation and wiring](SPDIF-IMPLEMENTATION.md) for the audio connection. The release includes `SHA256SUMS-v1.3.0.txt` for verifying the firmware images.

## Hardware

- Waveshare ESP32-S3-LCD-2, 320 x 240.
- Raspberry Pi Pico or Pico 2 running compatible DSPi firmware.
- Mechanical rotary encoder with push switch.
- Optional BLE HID remote.
- microSD card formatted as FAT32 or exFAT.

## DSPi compatibility

- DSPi firmware v1.1.5/V28 and compatible v1.1.6 builds: four S/PDIF inputs and verified runtime notifications are supported.
- Sub Synth requires compatible DSPi v1.1.6-beta3 or newer firmware with the extended control commands.
- Tube Modeller, Output Limiter and Spectrum require compatible DSPi v1.1.6 firmware exposing their commands and telemetry.
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

Configure DSPi S/PDIF input 1 on Pico GPIO5. The firmware validates this setting before starting playback. Keep the direct 3.3 V logic connection and common-ground return short; no coaxial or optical line driver is implied. Do not connect this GPIO-level signal to consumer coaxial S/PDIF equipment.

### Rotary encoder

| Encoder | ESP32-S3-LCD-2 |
|---|---|
| CLK / A | GPIO47 |
| DT / B | GPIO48 |
| Push switch | GPIO21 |
| Common / switch return | GND |

Power encoder modules from 3.3 V, not 5 V.

## Flash on Windows

1. Download the latest v1.3.0 release source or clone `main`.
2. Install Python 3 if `py --version` does not show a version.
3. Connect the ESP32-S3-LCD-2 by USB.
4. Close Arduino Serial Monitor and any program using the COM port.
5. Open PowerShell in the project folder and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Flash-DSPi-Front-Panel-v1.3.0.ps1"
```

The script asks for or uses the supplied COM port, installs the required build tools when needed, compiles the firmware and performs a clean flash.

To update only the application while preserving BLE pairing and panel settings:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Flash-DSPi-Front-Panel-v1.3.0.ps1" -PreserveSettings
```

A clean flash erases BLE pairing, learned key mappings, brightness, screen-power settings and shortcut assignments. After a clean flash, disconnect all power for at least 10 seconds before reconnecting.

## Build from source

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Build-DSPi-Front-Panel-v1.3.0.ps1"
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

The v1.3.0 wrappers use the version-neutral build engine and name generated artifacts as v1.3.0. The historical sketch-directory name is unchanged.

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

The artwork and folder-page caches use bounded PSRAM and fall back to normal reads when memory is unavailable. They improve repeat visits; they do not remove the initial artwork delay or accelerate the first folder scan.

Restrictions:

- Playback and Wi-Fi Music Transfer cannot use the SD card at the same time.
- Stop playback before starting Wi-Fi Music Transfer.
- Very large or unusual artwork may be skipped if it cannot be decoded safely.
- Unsupported, corrupt or unusually encoded files may be skipped.
- Removing the card while mounted or playing is not supported.
- After a genuine SD-card fault, remove all power before retrying.

## Update firmware over local Wi-Fi

The first installation must use the full USB image. After that, future application updates can be installed from the panel's local Wi-Fi page while preserving BLE bonds, remote mappings, presets and screen settings. An SD card is not required for firmware updates.

1. Download `DSPi-ESP32-Front-Panel-v1.3.0-OTA.bin` from the [latest release](https://github.com/CrawlingKingSn8ke/DSPi-ESP32-Front-Panel-Media-Player-SPDIF/releases/latest).
2. Stop or pause local music playback.
3. Open **System > Wi-Fi Transfer/Update** on the panel and confirm **Start**.
4. Connect to the network shown on the panel and open its displayed browser address.
5. Under **Local firmware update**, select **Choose application .bin** and choose the `-OTA.bin` file.
6. Select **Install firmware**, confirm the warning and keep the unit powered while it uploads and verifies the image.
7. Wait for the panel to restart automatically. The browser disconnecting during the restart is normal.
8. After the restart, switch off or disconnect all power for at least 10 seconds, then power the unit back on before using the media player. A software restart does not power-cycle the SD card, so this step is required for a reliable SD remount.

> [!WARNING]
> Never upload `DSPi-ESP32-Front-Panel-v1.3.0-Full.bin` through the browser. The 16 MB full image is only for USB installation or recovery at address `0x0`.

## Wi-Fi Music Transfer

Open **System > Wi-Fi Transfer/Update** and confirm **Start**. The panel displays the Wi-Fi connection details and browser address. Music-file transfer requires a mounted SD card.

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
- BLE reconnect scanning pauses while transfer mode is active and resumes after normal operation is restored.

## Notes

- DSPi control uses UART. Local SD-card music is also encoded by the ESP32 and sent to DSPi over the dedicated GPIO13 S/PDIF link.
- Input choices and DSP features depend on the connected DSPi firmware and configuration.
- The VU meters show DSPi output telemetry, not the volume-control position.
