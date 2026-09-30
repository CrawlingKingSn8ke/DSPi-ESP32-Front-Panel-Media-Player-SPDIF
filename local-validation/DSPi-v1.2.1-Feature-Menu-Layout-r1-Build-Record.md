# DSPi ESP32 Front Panel — Feature Menu Layout r1 build record

Build date: 2026-09-30. Local branch: `local/v1.2.1-stage-feature-menu-remote-ui`.
Source base: `8c6664a`. Final product-code commit: `0f755ab`.

## Scope

- Top level: Input, Music, Preset, Features, System; System is last.
- Feature, System, Remote, D-pad, and Preset lists use four visible rows with a down chevron when more entries remain.
- Tube Drive and Mix show full-size numbers and compact `dB`/`%` units.
- Remote key map and D-pad shortcuts are readable lists. The live connection state is distinct from the saved device name. Key capture waits for a release; Back and disconnect preserve the previous mapping. Removal names the device and defaults to Cancel.
- Seven Home feature icons fit in a centred second row. Tube can be assigned to a D-pad shortcut and has the existing on/off notification path. Limiter is an engaged-status icon only, read from the DSPi status block.
- No Music browser/now-playing renderer, playback, S/PDIF, task-priority, or shared-SPI source file was changed. The focused test compares the three Music renderer bodies with `8c6664a`.

Changed files relative to `8c6664a`:

1. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
2. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/TubeLimiter.h`
3. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiMenuLayout.h`
4. `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h`
5. `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`
6. `local-validation/Test-DSPi-v1.2.1-Glyph-Keymap-r2.py`
7. `local-validation/Test-TubeLimiterContracts.py`

This build record is the eighth changed file. The former glyph and Tube contracts were updated to check the new shared list renderers rather than their superseded carousel/special-case implementation.

## Verification

- Focused feature-layout tests: 12/12 passed.
- Full local Python test suite: 154/154 passed across 19 scripts, run individually with Python 3.13 because these hyphenated filenames are not discovered by `unittest discover` on this system.
- `git diff --check 8c6664a..HEAD`: passed with no whitespace errors.
- ESP32-S3 Arduino CLI 3.3.11 compile: passed. FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none`; extra flags `-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING`.
- Compiler reported sketch use: 1,933,849 bytes (61% of 3,145,728-byte app partition). Global use: 78,388 bytes (23% of 327,680 bytes).

## Isolated artifacts

Both are in `local-validation/builds/feature-menu-layout-20260930-r1/`; earlier build/release files were not replaced.

| Image | File bytes | SHA-256 |
|---|---:|---|
| OTA application `DSPi-ESP32-Front-Panel-v1.2.1-Feature-Menu-Layout-r1-OTA.bin` | 1,934,000 | `30312F9CA858CFFE58641A98B8D9F775BDCEA412334DA1D8F317F37D42B80CBB` |
| Full USB image `DSPi-ESP32-Front-Panel-v1.2.1-Feature-Menu-Layout-r1-Full.bin` | 16,777,216 | `BDCED51BB16099FA6E4FCB1EF5613D90927440D30E6C8BA0DB02E5AB5E3ED5F5` |

No device was flashed, no OTA upload was made, and nothing was pushed. Hardware acceptance is still required for visual spacing, Remote capture/reconnect, all-icon Home display, Tube local/Console overlays, and ordinary playback.
