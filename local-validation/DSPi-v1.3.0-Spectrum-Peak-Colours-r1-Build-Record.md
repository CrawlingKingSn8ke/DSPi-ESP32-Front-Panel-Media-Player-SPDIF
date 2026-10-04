# DSPi ESP32 Front Panel — Spectrum Peak Colours r1

Build date: 2026-10-04. Branch: `local/v1.3.0-spectrum-peak-colours-r1`.
Source base: `8425bbb` (Spectrum Segments and Fast Fall r1).

## Changes

- Spectrum > Bar Colours lists adjacent Bar and Peak rows for each enabled input/output channel, each with its own palette swatch.
- Peak markers use an independent per-channel colour, including their resting baseline. The three-pixel thickness, 440 ms hold, fall behaviour and segmented bar rendering are retained.
- Separate 33-byte peak-colour records use `sppeak_cfg` globally and `sppeak_pN` for presets. Existing bar-colour records are unchanged. Missing, malformed or unsupported peak records inherit the corresponding loaded bar colours, preserving the previous appearance.
- Apply and cancel use the existing palette editor and guarded deferred preference path. Saved presets include both colour records.

Changed files:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `local-validation/Test-DSPi-v1.2.1-Spectrum-Bar-Colours.py`
3. `local-validation/Test-DSPi-v1.2.1-Spectrum-Peaks-Overlays.py`
4. This build record.

## Verification

- Three focused contracts failed against the previous implementation and passed after the change.
- All 187 tests passed across 25 local scripts. `git diff --check` passed.
- ESP32-S3 Arduino CLI build passed using the existing 16 MB flash, 3 MB app partition, OPI PSRAM and SdFat compiler flags.
- Compiler: 1,943,469 bytes program storage, 79,132 bytes static RAM (32 more than the preceding build).
- Full USB image application payload at `0x10000` matches the OTA file byte for byte.
- Audio decoding, S/PDIF transmission, spectrum transport, display-transfer cadence and music-player UI were not changed.

## Artifacts

Directory: `local-validation/builds/v1.3.0-spectrum-peak-colours-20261004-r1/`.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Peak-Colours-r1-OTA.bin` | 1,943,616 | `CECDA4509AA467FCA7FC8CCD363F88DBCB9489DBE3B850925FF3BA5BEDDC74D3` |
| `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Peak-Colours-r1-Full.bin` | 16,777,216 | `6945A37773D1EB29BBD1A8D4D7E21FAC5776AAB23B9FBD95441CBF048246E750` |

Earlier images are preserved. Nothing was flashed or pushed.
