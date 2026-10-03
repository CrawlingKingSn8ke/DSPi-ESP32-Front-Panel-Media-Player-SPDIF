# DSPi ESP32 Front Panel — Spectrum Peak Hold 440 ms r1

Build date: 2026-10-03. Local branch: `local/v1.3.0-spectrum-peak-hold-r1`.
Source base: `4f55bbf` (Spectrum Interpolation r1).

## Scope

- Thicken each spectrum peak marker from one to three pixels, keeping a one-pixel gap above an active bar.
- Hold each marker at its latest peak for 440 ms (previously 240 ms), then retain the existing slower fall rate. A new peak still lifts the marker immediately.
- No changes to bar interpolation, DSPi polling, music playback, S/PDIF output, task priorities, or the shared-SPI transfer path.

Changed files relative to `4f55bbf`:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `local-validation/Test-DSPi-v1.2.1-Spectrum-Peaks-Overlays.py`
3. This build record.

## Verification

- Focused peak-marker test failed against the old 240 ms / one-pixel code, then passed after the change.
- All 181 local tests passed across 25 scripts.
- `git diff --check` passed.
- ESP32-S3 Arduino CLI build only passed with the existing 16 MB flash, `app3M_fat9M_16MB`, OPI PSRAM, hardware USB CDC, and SdFat compiler flags.
- Compiler reported 1,942,669 bytes (61%) of the 3,145,728-byte app partition and 79,100 bytes (24%) of 327,680 bytes static RAM.
- The application payload at offset `0x10000` in the full USB image has the same SHA-256 as the standalone OTA image.

## Isolated artifacts

Both are in `local-validation/builds/v1.3.0-spectrum-peak-hold-20261003-r1/`. Earlier images were not overwritten.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Peak-Hold-440ms-r1-OTA.bin` | 1,942,816 | `3A015E9A4279D26FCCE4071690019DDF637B7B94BC00F03424C30512EF4289E5` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Peak-Hold-440ms-r1-Full.bin` | 16,777,216 | `E39BA399595DB0AC2288B9D9E6954AF2B437C1ABE969A1A976713AEF30D1EDCC` |

Nothing was flashed or pushed.
