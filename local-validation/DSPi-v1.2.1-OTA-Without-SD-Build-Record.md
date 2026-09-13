# SD-independent Wi-Fi OTA — 2026-09-06 r2

Local test build based on BLE reconnect commit 4225e7c, retaining the subsequent notification-shortcut and decimal fixes. Nothing flashed, pushed, or removed. Earlier build outputs retained.

## Implementation

- When mounting storage for transfer fails and no card is mounted, enter explicit update-only mode. Playback stop, settings flush, route restoration and BLE shutdown remain in the entry sequence.
- Update-only service allocates its workspace but leaves the storage pointer null. Normal transfer sessions retain read/write preflight and SD synchronization.
- OTA can start without SD synchronization only when no storage pointer is attached. Existing image validation, inactive-slot writes and update verification remain unchanged.
- Existing filesystem endpoint guards reject requests without storage, including upload planning. Portal status reports storageAvailable; SD controls are disabled when false and initial directory browsing is skipped. Firmware, network and Finish Safely controls remain available.
- Card-free exit skips remount attempts/errors and uses a truthful completion message. Existing BLE power-cycle guidance remains unchanged.
- No hot insertion support added: insert a card and reopen transfer mode to enable music transfers. A mounted card that fails write preflight still follows the existing error path.
- Audio pipeline and S/PDIF implementation unchanged.

## Changed files for this stage

- firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino
- firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferMode.cpp
- firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferMode.h
- firmware/DSPi_ESP32_Front_Panel_v1_1_2/WifiTransferWeb.h
- local-validation/Test-DSPi-v1.2.1-OTA-Without-SD.py
- local-validation/Test-OTA-Without-SD-Web.cjs
- This record

## Verification

101 Python tests/source contracts passed across 11 scripts. The complete browser script parsed successfully; 18 assertions executed its actual control-state function with DOM stubs for unknown, present and absent storage. Git whitespace check passed. Hardware no-card connection, OTA installation, card-present transfers and safe exit still require user testing.

Build r1 missed the final portal adjustment made while compilation ran. It is not the delivery artifact. Incremental build r2 completed and its binary was inspected to confirm the final portal control code is embedded.

ESP32-S3 build: 240 MHz, 16 MB flash, app3M_fat9M_16MB, OPI PSRAM, hardware USB CDC; SDFAT_FILE_TYPE=3, USE_UTF8_LONG_NAMES=1, DISABLE_FS_H_WARNING. Compiler storage: 1,915,297 / 3,145,728 bytes (60%). Static RAM: 77,388 / 327,680 bytes (23%).

Folder: local-validation/builds/v1.2.1-ota-without-sd-20260906-r2

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| DSPi-ESP32-Front-Panel-v1.2.1-OTA-Without-SD.bin | 1,915,440 | AE86F36B3169CCA66CB8D25D3B52E9F676DFE6BF067A16CE1DB013DCAC59A677 |
| DSPi-ESP32-Front-Panel-v1.2.1-OTA-Without-SD-Full.bin | 16,777,216 | DE492683812423DF74AA05FF2E283AEEDF559DF86B9BCD76F4C81C83195D62E5 |

Use the application-only file on Wi-Fi Transfer/Update. The currently installed old firmware still requires an SD card to install this update through Wi-Fi; alternatively installation can be performed through USB. After installation, test powering up without a card and opening the portal. Do not remove a mounted card during transfer mode.
