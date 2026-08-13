# DSPi ESP32 Front Panel v1.2.1 — S/PDIF transmitter hardening test build

Date: 2026-08-13

Branch: `local/v1.2.1-spdif-output-test`

## Purpose

Prevent a brief ESP output-task delay from replacing the valid biphase-mark carrier with raw zero DMA words while retaining the user's established DSPi input wiring.

## Functional changes

- ESP S/PDIF data remains on ESP32-S3 GPIO13.
- The DSPi receiver route is S/PDIF input 1 on Pico GPIO5.
- Route activation validates the configured S/PDIF1 pin before starting playback.
- I2S DMA auto-clear is disabled because raw zero words are not valid S/PDIF silence.
- All DMA descriptors are primed with six complete 192-frame blocks of encoded digital silence.
- When playback stops, GPIO13 is explicitly driven low.
- Music still stops when the user or DSPi Console selects another input.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/MediaPlayerPoC.cpp`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/MediaPlayerPoC.h`
- `local-validation/Test-DSPi-v1.2.1-SPDIF-Output.py`
- `local-validation/DSPi-v1.2.1-SPDIF2-Isolation-Build-Record.md`

## Verification

- S/PDIF output contracts: 16 passed.
- Local browser OTA contracts: 12 passed.
- Wi-Fi System-menu contracts: 5 passed.
- Total focused contracts: 33 passed.
- Pinned ESP32 Arduino core 3.3.11 build: passed.
- Program use: 1,909,785 bytes (60% of 3,145,728 bytes).
- Global variables: 77,332 bytes (23% of 327,680 bytes).
- Nothing was flashed and no serial port was opened.

## Artifacts

### Browser OTA application

- File: `release/DSPi-ESP32-Front-Panel-v1.2.1.bin`
- Size: 1,909,936 bytes
- SHA-256: `4F4B2357300622FFF9248A1D620EE0CB656B82E40585FB744C450AE03D2558D6`

### Full USB image

- File: `release/DSPi-ESP32-Front-Panel-v1.2.1-Full.bin`
- Size: 16,777,216 bytes
- SHA-256: `8BEBC061566881A8F319A626C827ABAC01EAC4F9C718446C6D72C3AB2B456A44`

## Required test wiring

Keep the existing ESP GPIO13 to Pico GPIO5 connection. DSPi Console must have S/PDIF input 1 assigned to GPIO5. The user's S/PDIF input 2 remains on GPIO4, DSPi S/PDIF output remains on GPIO6, and GPIO20 remains unused. No series resistor is required for this test because the DSPi developer has approved the direct short 3.3 V logic connection.
