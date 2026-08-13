# DSPi v1.2.1 WiFi System Entry Build

Date: 2026-08-13

Branch: `local/v1.2.1-spdif-output-test`

Base commit: `9d2b570` (`Add local browser firmware updates`)

## Scope

- Removes the Wi-Fi Transfer action from the Music browser.
- Adds `WiFi Transfer/Update` directly below `Volume Limit` in System Setup.
- Selecting it opens the existing Wi-Fi transfer confirmation and lifecycle.
- Cancel, Back, recovery acknowledgement and successful completion return to System Setup.
- Does not change S/PDIF playback, OTA, SD ownership, Wi-Fi credentials or the web portal.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `local-validation/Test-DSPi-v1.2.1-Wifi-System-Entry.py`
- `local-validation/DSPi-v1.2.1-Wifi-System-Entry-Build-Record.md`

## Verification

- Existing S/PDIF contracts: 14 passed.
- Existing local web OTA contracts: 12 passed.
- New menu relocation contracts: 5 passed.
- Total focused contracts: 31 passed.
- ESP32 build succeeded with the pinned project toolchain.
- Program storage: 1,909,509 bytes of 3,145,728 bytes (60%).
- Global variables: 77,332 bytes of 327,680 bytes (23%).
- Nothing flashed or pushed.

## Artifacts

Application-only image, suitable for the local web updater:

- `release/DSPi-ESP32-Front-Panel-v1.2.1.bin`
- Size: 1,909,664 bytes
- SHA-256: `70DFA8DC115A13048A90D245DAA96695370292E64EB1E1ED05AA1989FFE82FD3`

Full USB image:

- `release/DSPi-ESP32-Front-Panel-v1.2.1-Full.bin`
- Size: 16,777,216 bytes
- SHA-256: `6990C3BFE0637A52CC826A3DE13247F94DBC91A01AE580DD2B5C6689AC72C9BE`
