# DSPi ESP32 Front Panel — Spectrum Interpolation r1 build record

Build date: 2026-10-02. Local branch: `local/v1.3.0-spectrum-interpolation-r1`.
Source base: `de6deb5`.

## Scope

- For external DSPi sources, request alternating spectrum channels every 30 ms, targeting one L/R pair per approximately 60 ms, close to the DSPi's approximately 17 Hz RTA updates. The actual hardware cadence is not measured.
- Interpolate each bar from its currently rendered height to the next real DSPi sample over 60 ms. Repeated reads of the same frame do not restart the transition. The 37 wire-format bands and the existing displayed bar layout are unchanged; interpolation does not create additional frequency detail.
- Attempt external-source graph redraws at no more than 25 frames/s. Intermediate frames transfer only rows 54–216 (104,320 RGB565 bytes), with a full 153,600-byte canvas refresh every second for static labels and status.
- Preserve the existing 80 ms/channel polling, paired full-frame redraw, and low-buffer/transition guard for local SD playback. Audio decoding, S/PDIF transmission, task priorities, and the music-player UI were not changed.
- Keep the existing shared-SPI try-lock; a busy bus skips a visual frame rather than waiting behind SD activity.

Changed files relative to `de6deb5`:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h`
3. `local-validation/Test-DSPi-v1.2.1-Spectrum-Peaks-Overlays.py`
4. `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`
5. `local-validation/Test-DSPi-v1.3.0-Spectrum-Interpolation.py`
6. This build record.

## Verification

- All 181 local tests passed across 25 individually executed scripts using the bundled Python runtime. The hyphenated test filenames are not suitable for `unittest discover` here.
- `git diff --check`: passed.
- ESP32-S3 Arduino CLI build only: passed. FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none`; extra flags `-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING`.
- Compiler reported sketch use: 1,942,669 bytes (61% of 3,145,728-byte app partition). Global use: 79,100 bytes (24% of 327,680 bytes).
- The application payload at offset `0x10000` in the full USB image has the same SHA-256 as the standalone OTA image.

## Isolated artifacts

Both are in `local-validation/builds/v1.3.0-spectrum-interpolation-20261002-r1/`; earlier build and release files were not replaced.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Interpolation-r1-OTA.bin` | 1,942,816 | `4A8A09B75180255A4EA139F6E01D876166625125934295678537BD88808B34AC` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.3.0-Spectrum-Interpolation-r1-Full.bin` | 16,777,216 | `F43A56F997C494603197009FB8A1E06B1E32BE3FFEA5D4F4CC0809D4562C28EE` |

No device was flashed, no OTA upload was made, and nothing was pushed. Confirm external-input smoothness and actual frame rate on hardware before considering release; verify SD playback remains free of stutters.
