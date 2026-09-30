# DSPi ESP32 Front Panel — UI Polish r3 build record

Build date: 2026-09-30. Branch: `local/v1.2.1-stage-ui-polish-r3`. Base commit: `86c66c5` (UI Polish r2).

## Changes

- Home feature symbols are 125% of their previous width and height, with a seven-pixel gap instead of five. Source and preset remain on the same header line. The source label is width-bounded when many features are enabled, so no symbol or label overlaps.
- Feature-list symbols retain their previous compact size and selected Volume Meters colour.
- The top-level label and page title are now `DSP Setup`. Its 299-pixel native menu-font width fits the 320-pixel display, so it uses the same unscaled renderer and y-position as every other top-level choice.
- No music-player, playback, S/PDIF, DSP command, or task-scheduling source path was changed.

Changed files:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h`
3. `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`
4. `local-validation/Test-DSPi-v1.2.1-UI-Polish-r2.py`
5. `local-validation/Test-DSPi-v1.2.1-UI-Polish-r3.py`
6. This build record.

## Verification

- 161/161 local tests passed across 21 scripts. The new tests check native menu glyph coverage/width, exact menu renderer alignment, enlarged Home icon geometry, and unchanged compact feature-list icons.
- `git diff --check`: no whitespace errors.
- ESP32-S3 Arduino CLI compile passed (Espressif core 3.3.11). FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none`; extra flags `-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING`.
- Compiler reported 1,934,909 sketch bytes (61% of 3,145,728-byte application partition), 78,388 global bytes (23% of 327,680 bytes).

## Isolated images

Both images are under `local-validation/builds/ui-polish-20260930-r3/`; r2 images were not replaced.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.2.1-UI-Polish-r3-OTA.bin` | 1,935,056 | `9BD8C1861CCA88C336205109B77B850C924BD42A0E9D0D462510987EBBCE07FF` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.2.1-UI-Polish-r3-Full.bin` | 16,777,216 | `89E16245FA5B607E235D8A5AF8653D6FD9CDB5F23AB2AFB71086FA237FA6743D` |

Nothing was flashed, uploaded over Wi-Fi, or pushed. The new icon size/spacing still requires visual acceptance on the actual panel.
