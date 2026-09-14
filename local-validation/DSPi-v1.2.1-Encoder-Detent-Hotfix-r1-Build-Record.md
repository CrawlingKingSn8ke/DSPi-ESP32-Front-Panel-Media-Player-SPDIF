# DSPi v1.2.1 Encoder Detent Hotfix r1 Build Record

- Branch: `local/v1.2.1-encoder-detent-hotfix`
- Scope: calibrate the rotary encoder for four quadrature edges per physical detent while preserving one menu/edit action and one 1 dB Home-volume change per physical detent.
- Firmware change: `ENCODER_COUNTS_PER_DETENT` changed from 2 to 4; `ENCODER_MENU_DETENTS_PER_STEP` changed from 2 to 1.
- Focused contract: `local-validation/Test-DSPi-v1.2.1-Encoder-Physical-Detent.py`
- Verification: 116 Python firmware tests passed; 18 Wi-Fi portal assertions passed; firmware build succeeded.
- Application size: 1,917,344 bytes.
- Application SHA-256: `C8DB0F12A0A9087EBDD69266F0079FDDD2A449B4702A47DA82E9BCAC7311C600`
- OTA artifact: `local-validation/builds/v1.2.1-encoder-detent-hotfix-20260913-r1/DSPi-ESP32-Front-Panel-v1.2.1-Encoder-Detent-Hotfix-r1-OTA.bin`
- Nothing was flashed, pushed, published, or overwritten.
