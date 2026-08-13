# DSPi ESP32 Front Panel v1.2.1 — S/PDIF2 isolation test build

Date: 2026-08-13

Branch: `local/v1.2.1-spdif-output-test`

## Purpose

Isolate the ESP music-player S/PDIF link from the DSPi primary S/PDIF input and optical-output pin neighbourhood, and prevent a brief ESP output-task delay from replacing the valid biphase-mark carrier with raw zero DMA words.

## Functional changes

- ESP S/PDIF data remains on ESP32-S3 GPIO13.
- The DSPi receiver route is now S/PDIF input 2 on Pico GPIO20.
- Route activation validates the configured S/PDIF2 pin before starting playback.
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
- Program use: 1,909,789 bytes (60% of 3,145,728 bytes).
- Global variables: 77,332 bytes (23% of 327,680 bytes).
- Nothing was flashed and no serial port was opened.

## Artifacts

### Browser OTA application

- File: `release/DSPi-ESP32-Front-Panel-v1.2.1.bin`
- Size: 1,909,936 bytes
- SHA-256: `20CB016DDE80FCE878075A29CF58BAB42A90BD0FF4895AC9D853912B8277F530`

### Full USB image

- File: `release/DSPi-ESP32-Front-Panel-v1.2.1-Full.bin`
- Size: 16,777,216 bytes
- SHA-256: `2428DA0393FD7109A81E66F9F7FE45B80CD89F42DA59A61DC306DF74D9AD74D5`

## Required test wiring

Move only the Pico end of the ESP S/PDIF data connection from GPIO5 to GPIO20. Keep the ESP end on GPIO13. DSPi Console must have S/PDIF input 2 enabled and assigned to GPIO20. A 47–100 ohm series resistor placed near the ESP output is recommended.
