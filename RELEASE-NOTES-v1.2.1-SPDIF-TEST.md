# v1.2.1 S/PDIF media-player test

Experimental ESP32-S3 front-panel firmware that routes the integrated SD-card music player to DSPi S/PDIF input 1 over ESP GPIO13 to Pico GPIO5.

This build is published for testing and developer review. DMA/carrier hardening substantially reduced S/PDIF dropouts, but one tested DSPi hardware configuration still exhibits occasional dropouts on the DSPi S/PDIF output while its simultaneous I2S/PCM5102 output remains stable. Read `EXPERIMENTAL-SPDIF.md` for the full comparison and current evidence.

## OTA asset

- `DSPi-ESP32-Media-Player-SPDIF-v1.2.1-test.bin`
- Application-only browser OTA image
- Size: 1,909,936 bytes
- SHA-256: `CA693E387D8F08C0C9C3D037AE6FC231F3D8A8D7B239A626156A4B3445EDD110`

Do not upload a 16 MB full-flash image through the browser updater.

## Validation

- 34 focused contracts passed
- ESP32 Arduino core 3.3.11 build passed
- Program storage: 60%
- Source hardening commit: `903de33`
