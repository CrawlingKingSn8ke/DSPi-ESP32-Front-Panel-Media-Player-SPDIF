# Spectrum volume card r8 — local build record

Base: `d053413` (r7 Spectrum peak markers and notifications). Branch: `local/v1.2.1-stage-spectrum-volume-card-r8`.

Changes:

- Spectrum's full-screen volume notification now calls the same number-only card renderer used by Now Playing. There is no `Volume` heading; held repeats use the exact same bounded number strip.
- The top-level `DSP Setup` item and its page title are now `Setup`, retaining the native menu font, position and navigation.
- Updated the affected UI/static contracts. The music-player volume renderer itself is unchanged. No audio, S/PDIF, Spectrum bar or peak-ballistics code changed.

Verification: all 24 Python test files passed; `git diff --check` passed. Build-only succeeded with ESP32 core 3.3.11, GFX 1.6.5, NimBLE 2.5.0 and SdFat 2.3.0. Sketch: 1,941,577 bytes (61%); globals: 78,924 bytes (24%).

| Image | Bytes | SHA-256 |
|---|---:|---|
| `local-validation/builds/v1.2.1-spectrum-volume-card-20261001-r8/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Volume-Card-r8-OTA.bin` | 1,941,728 | `D4BA0F939EDBD28150F94BA8643D10D305AC2B6BEEC4279B8572FED256E34120` |
| `local-validation/builds/v1.2.1-spectrum-volume-card-20261001-r8/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Volume-Card-r8-Full.bin` | 16,777,216 | `1DB76D45A0C68AF3C22AA923AF6D2B229A39D8649C2BF0550FA57EF6472C9166` |

Nothing was flashed or pushed. The separate r7 images remain available for rollback.
