# Spectrum bar colours r6 — local build record

Base: `dfd9b52` (previous spectrum UI stage). Branch: `local/v1.2.1-stage-spectrum-colours-r6`.

Changed files:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `local-validation/Test-DSPi-v1.2.1-Spectrum-Bar-Colours.py`
- This build record

Spectrum now has an independent Bar Colours submenu. Enabled output channels are listed first, followed by available input channels. Each has its own right-hand swatch and uses the existing 30-colour palette editor. Colour choices are stored separately from theme, analogue VU and Home volume-meter colours. A versioned, validated record is persisted globally and per preset; absent or invalid records load safe cyan/blue defaults. During media playback, changes apply to the display immediately but flash persistence waits until playback stops, following the existing audio-safe deferred-write policy.

The visualizer retains one bar per real DSPi band: 31 bars for a 34-band 44.1/48 kHz frame, from 20 Hz onward. Bottom labels are `20 50 100 200 500 1k 2k 5k 10k`, omitting `20k` as requested. No audio, S/PDIF, DSPi command, or scheduler code was changed.

Verification: all 23 Python test files passed; `git diff --check` passed. ESP32-S3 Arduino build succeeded (core 3.3.11, GFX 1.6.5, NimBLE 2.5.0, SdFat 2.3.0). Sketch 1,940,157 bytes (61%); globals 78,548 bytes (23%).

| Image | Size | SHA-256 |
| --- | ---: | --- |
| `local-validation/builds/v1.2.1-spectrum-bar-colours-20261001-r6/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Bar-Colours-r6-OTA.bin` | 1,940,304 bytes | `0CE7FBD238D632508F100FBCB205287D63C9ABC57B5320E56016ECF1C1A7EAA9` |
| `local-validation/builds/v1.2.1-spectrum-bar-colours-20261001-r6/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Bar-Colours-r6-Full.bin` | 16,777,216 bytes | `09D150FBCE802F743D13DC07A293B6D40EE149D8ABB9B3891C8A6DD8D541F7E3` |

No hardware was flashed. Nothing was pushed. Prior uniquely named builds remain intact.
