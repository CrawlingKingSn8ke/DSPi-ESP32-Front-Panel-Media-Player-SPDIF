# v1.2.1 S/PDIF media-player test

Experimental ESP32-S3 front-panel firmware that routes the integrated SD-card music player to DSPi S/PDIF input 1 over ESP GPIO13 to Pico GPIO5.

This build is published for testing and developer review. It adds continuous same-rate S/PDIF carrier retention across track transitions to the existing DMA and framing hardening. Current testing is stable when paired with the experimental DSPi S/PDIF receiver overhaul; official DSPi integration remains pending. Read `EXPERIMENTAL-SPDIF.md` for the full comparison and current evidence.

## OTA asset

- `DSPi-ESP32-Front-Panel-v1.2.1-Continuous-SPDIF-Carrier-OTA.bin`
- Application-only browser OTA image
- Size: 1,911,808 bytes
- SHA-256: `1A77C60E10644145CEC09822BD6A337D2EE6BF5E51A65B626843E262D6132B44`

## Full USB image

- `DSPi-ESP32-Front-Panel-v1.2.1-Continuous-SPDIF-Carrier-Full.bin`
- Complete 16 MB first-install/recovery image
- Size: 16,777,216 bytes
- SHA-256: `192A218CB26F89D930C7D2AEFF3F1E42B12C6C4B2AE7CB1FAFAC0DEE51350822`

Do not upload a 16 MB full-flash image through the browser updater.

## Validation

- 58 focused contracts passed
- ESP32 Arduino core 3.3.11 build passed
- Program storage: 60%
