# v1.2.1 S/PDIF Forensic r2

Current experimental ESP32-S3 front-panel firmware for the integrated SD-card music player over a single-wire, logic-level S/PDIF connection to DSPi.

This build preserves the continuous same-rate carrier design and fixes rare partial-write and stop/retention lifecycle races found during the forensic review. The encoder, clock configuration and complete DMA preload were checked against the bundled ESP-IDF 5.5.5 implementation. The historical recurring downstream dropout is not attributed to these ESP fixes; current evidence still points toward the older DSPi receiver/output clock-tracking path. See `EXPERIMENTAL-SPDIF.md` for the evidence boundary.

## OTA application

- `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-Forensic-r2-OTA.bin`
- Browser OTA/application-only image
- Size: 1,912,624 bytes
- SHA-256: `8C4DE6295B6299080621401FD8FEB289A114D3DD129D6BF4C5D9A4BA7FEE22D6`

## Full USB image

- `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-Forensic-r2-Full.bin`
- Complete 16 MB first-install/recovery image
- Size: 16,777,216 bytes
- SHA-256: `A04E5C920D6FF32D6B98039E6E5060BD9B6B4B18BE98C3E6F2E41BE10D6162B6`

Do not upload the 16 MB full image through the browser updater.

## Validation

- 85 focused/static/behavioural contracts passed
- ESP32 Arduino core 3.3.11 build passed
- Program storage: 1,912,481 bytes
- Static RAM: 77,364 bytes
