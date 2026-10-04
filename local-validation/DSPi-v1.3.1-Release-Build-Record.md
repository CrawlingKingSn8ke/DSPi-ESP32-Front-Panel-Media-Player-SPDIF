# DSPi ESP32 Front Panel — v1.3.1 release

Build date: 2026-10-04. Source base: `03b8918`.

This supersedes the v1.3.0 r2 release designation. Firmware behaviour is unchanged; the panel Status label and serial banner now report v1.3.1. VERSION, build/flash wrapper filenames, release documentation and asset names agree with the new version. The original v1.3.0 documentation is retained as release history.

- All 187 tests passed across 25 scripts; the three v1.3.1 PowerShell wrappers parse successfully.
- ESP32-S3 Arduino CLI build passed with the existing 16 MB flash, 3 MB app partition, OPI PSRAM and SdFat compiler settings.
- Program storage: 1,943,469 bytes. Static RAM: 79,132 bytes.
- Full USB image application payload at `0x10000` matches the OTA file byte for byte.
- `git diff --check` passed. Nothing flashed.

Build directory: `local-validation/builds/v1.3.1-release-20261004/`. Previous test images are preserved.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.3.1-OTA.bin` | 1,943,616 | `6A79FE16636A0A05A860A6813B3F9A896655A810DFBB175BB4A08E831B090593` |
| `DSPi-ESP32-Front-Panel-v1.3.1-Full.bin` | 16,777,216 | `D2E796EF333FB2B8E54145AE3F2AC42F1BF58D6F324F8801BD51A53A08D33E2C` |
