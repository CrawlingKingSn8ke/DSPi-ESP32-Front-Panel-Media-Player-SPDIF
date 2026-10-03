# DSPi ESP32 Front Panel — Spectrum Segments and Fast Fall r1

Build date: 2026-10-03. Local branch: `local/v1.3.0-spectrum-segments-fast-fall-r1`.
Source base: `0b72cdd` (Spectrum Peak Hold 440 ms r1).

## Scope

- External-source spectrum bars fall immediately to the newest DSPi reading instead of interpolating downward for another 60 ms. Rising bars retain 60 ms interpolation; SD music remains on the existing non-interpolated path. DSPi averaging, spectrum transport and audio tasks are unchanged.
- Draw each active spectrum bar as two lit pixel rows separated by one dark row, preserving channel colours and the existing 440 ms continuous peak markers. No additional display transfers are introduced.
- Rename the main `Setup` entry and page title to `Effects`, adding a matching large-menu `E` glyph so the label retains the established font.
- Remove the `Waiting for audio` spectrum message.

Changed files relative to `0b72cdd`:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`
3. `local-validation/Test-DSPi-v1.2.1-Render-Efficiency.py`
4. `local-validation/Test-DSPi-v1.2.1-Spectrum-Bar-Colours.py`
5. `local-validation/Test-DSPi-v1.2.1-Spectrum-Peaks-Overlays.py`
6. `local-validation/Test-DSPi-v1.2.1-UI-Polish-r2.py`
7. `local-validation/Test-DSPi-v1.2.1-UI-Polish-r3.py`
8. `local-validation/Test-DSPi-v1.3.0-Spectrum-Interpolation.py`
9. This build record.

## Verification

- Focused tests first failed on the old implementation, then passed after the change.
- All 184 tests passed across 25 local scripts.
- `git diff --check` passed.
- The new large-menu `E` glyph has 364 encoded bytes and decodes to exactly 34 × 45 pixels.
- ESP32-S3 Arduino CLI build only passed with the existing 16 MB flash, `app3M_fat9M_16MB`, OPI PSRAM, hardware USB CDC and SdFat compiler flags.
- Compiler reported 1,942,897 bytes (61%) of the 3,145,728-byte app partition and 79,100 bytes (24%) of 327,680 bytes static RAM.
- The application payload at offset `0x10000` in the full USB image matches the standalone OTA image byte for byte.

## Isolated artifacts

Both are in `local-validation/builds/v1.3.0-spectrum-segments-fast-fall-20261003-r1/`. Earlier images were not overwritten.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Segments-Fast-Fall-r1-OTA.bin` | 1,943,040 | `5751BB536EA3383AE154BA79B7914AB325B1C2F7C48560887BAFDCE2F1DE1762` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Segments-Fast-Fall-r1-Full.bin` | 16,777,216 | `8129BF238F19AD9474277DEE3CC5B6D81848930AFB07F7BA63CFD3B5AE80E1C4` |

Nothing was flashed or pushed.
