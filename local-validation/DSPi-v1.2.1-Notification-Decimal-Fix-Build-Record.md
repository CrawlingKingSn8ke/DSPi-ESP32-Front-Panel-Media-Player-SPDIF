# v1.2.1 notification shortcut and decimal fix — 2026-09-06 r1

Based on local BLE reconnect test commit 4225e7c. Includes that existing fix.

## Changes

- Home shortcut mapping now also applies during Home feature confirmations and input/preset change overlays, before the resolved held action is stored. Previously raw Up/Down could dismiss the notification and fall through to volume navigation. Feature confirmations returning to another view are not treated as Home.
- Home volume decimal is drawn once at the central position, with its existing round shape and halo. Previously every offset digit glow pass redrew the decimal at full brightness, creating protruding pixels. Digit glow, positioning, spacing and theme roles are unchanged.
- No audio pipeline, S/PDIF, preference, or release-file changes.

Changed files: firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino; local-validation/Test-DSPi-v1.2.1-Notification-Shortcut-Decimal.py; this record.

## Validation

All 10 local-validation test scripts executed individually: 95 tests passed, including three new source contracts. These are not hardware tests. Git diff whitespace check passed.

Arduino ESP32-S3 build succeeded with existing 16 MB flash / app3M_fat9M_16MB partition settings, OPI PSRAM, 240 MHz CPU and hardware USB CDC. Extra flags: SDFAT_FILE_TYPE=3, USE_UTF8_LONG_NAMES=1, DISABLE_FS_H_WARNING.

Program storage: 1,914,253 / 3,145,728 bytes (60%). Static RAM: 77,388 / 327,680 bytes (23%).

Output folder: local-validation/builds/v1.2.1-notification-decimal-fix-20260906-r1

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| DSPi-ESP32-Front-Panel-v1.2.1-Notification-Decimal-Fix-OTA.bin | 1,914,400 | 69F9C1CFAD57E376DE9A8987F38EF5C1A752F7C4C32423AA0F34E54D5DD60E52 |
| DSPi-ESP32-Front-Panel-v1.2.1-Notification-Decimal-Fix-Full.bin | 16,777,216 | ADFE29E9216CBFFBB3E24842C5B190BF3F46CFA62E18C25777CF621DF4CD6349 |

Nothing flashed or pushed; previous builds preserved. Use only the OTA application on Wi-Fi Transfer/Update, not the full USB image.

Hardware acceptance: repeatedly tap loudness and leveller during and immediately after notifications, verifying volume remains unchanged. Check dedicated volume buttons and menu navigation still work. Inspect the Home decimal with multiple text colours. Recheck the earlier BLE reconnect test over power cycles.
