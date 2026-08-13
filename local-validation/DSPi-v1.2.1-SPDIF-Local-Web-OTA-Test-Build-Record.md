# DSPi v1.2.1 S/PDIF Local Web OTA Test Build

Date: 2026-08-13

Branch: `local/v1.2.1-spdif-output-test`

Base commit: `24bc132` (`Record colour-updated SPDIF test build`)

## Scope

- Adds manual, local-only application firmware upload to the existing DSPi Music Transfer page.
- Uses the existing dual OTA application slots and writes only `U_FLASH` to the inactive slot.
- Preserves NVS/settings and the SD music library.
- Performs no internet lookup, download, version check, or remote request.
- Keeps the current 24-bit 44.1/48 kHz S/PDIF music path unchanged.

## Safety contracts

- Requires a declared, non-empty HTTP body whose received byte count matches exactly.
- Rejects images below 256 KiB or larger than the inactive OTA partition.
- Validates ESP image magic, ESP32-S3 chip ID, and ESP application descriptor before committing image data.
- Synchronizes the SD device before internal-flash writing.
- Blocks transfers, deletes, Wi-Fi changes, Finish Safely, and competing writers during installation.
- Sends the success response before a delayed restart.
- Aborts a failed or interrupted update and leaves the current bootable application in place.
- The portal warns users never to select the 16 MB Full image.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferMode.cpp`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferMode.h`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferWeb.h`
- `local-validation/Test-DSPi-v1.2.1-Local-Web-OTA.py`
- `local-validation/DSPi-v1.2.1-SPDIF-Local-Web-OTA-Test-Build-Record.md`

## Verification

- Existing focused S/PDIF contracts: 14 passed.
- New local web OTA contracts: 12 passed.
- ESP32 build: passed with Arduino ESP32 core 3.3.11, GFX Library 1.6.5, NimBLE-Arduino 2.5.0, and SdFat 2.3.0.
- Program storage: 1,909,737 bytes of 3,145,728 bytes (60%).
- Global variables: 77,332 bytes of 327,680 bytes (23%).
- No serial port was opened and nothing was flashed.

## Build artifacts

Application-only image for USB at `0x10000` and for later portal updates:

- `release/DSPi-ESP32-Front-Panel-v1.2.1.bin`
- Size: 1,909,888 bytes
- SHA-256: `F445F173AD35D8A3795DDD94E9FB73F0A8BCCD835B639E839ACF7935A9BDDA5E`

Full 16 MB bootstrap image (USB only; never upload this through the portal):

- `release/DSPi-ESP32-Front-Panel-v1.2.1-Full.bin`
- Size: 16,777,216 bytes
- SHA-256: `8FEA842432A02B79A82356E328D8AD24FDF9026BFAEFF6BA7432915CFC1EEF20`
