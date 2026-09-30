# DSPi ESP32 Front Panel — UI Polish r2 build record

Build date: 2026-09-30. Local branch: `local/v1.2.1-stage-ui-polish-r2`.
Base: `aea0e3a` (Feature Menu Layout r1). This is an isolated follow-up build; the r1 images remain untouched.

## Changes

- Remote list, shortcut, and confirmation navigation now uses Up/Down, including held-key repeat.
- D-pad shortcut values use a fixed left edge; Remove/Cancel confirmation uses the normal medium font.
- System menu order is Remote, Volume Limit, Screen Settings, WiFi Transfer/Update, Status. The matching selection and Volume Limit editing paths were updated together.
- The top-level Features label is now `DSP Config` and is measured/scaled to fit the existing large menu font.
- The preset-save hint uses medium text and names the Select button.
- The seven feature icons are inline with the Home source and preset labels. Both Home icons and matching feature-list icons use the selected Volume Meters colour. The Home source text is width-bounded so active icons cannot overlap source or preset.
- Paged settings, presets, and Remote lists show a mirrored footer up arrow on their final page.
- Music browser, now-playing renderer, playback pipeline, S/PDIF output, and task scheduling were not changed.

Changed files:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiMenuLayout.h`
3. `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`
4. `local-validation/Test-DSPi-v1.2.1-Wifi-System-Entry.py`
5. `local-validation/Test-DSPi-v1.2.1-UI-Polish-r2.py`
6. This build record.

## Verification

- 158/158 local tests passed across 20 scripts, including new focused r2 contracts and footer-arrow boundary checks.
- `git diff --check`: passed.
- ESP32-S3 Arduino CLI compile passed (Espressif core 3.3.11). FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none`; extra flags `-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING`.
- Compiler reported 1,934,409 sketch bytes (61% of 3,145,728-byte app partition) and 78,388 global bytes (23% of 327,680 bytes).

## Isolated build images

Both are under `local-validation/builds/ui-polish-20260930-r2/`.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.2.1-UI-Polish-r2-OTA.bin` | 1,934,560 | `815CBF12895098AB69B1EC3F814478F04D91548E98745879779C9CD327646601` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.2.1-UI-Polish-r2-Full.bin` | 16,777,216 | `C667D9CBA675127C35F2B313B8B9B774DEFC605BD400A77BD597501D44D767DC` |

No device was flashed, no OTA upload was made, and nothing was pushed. Hardware visual acceptance remains necessary for the new one-line Home icon layout and menu alignment.
