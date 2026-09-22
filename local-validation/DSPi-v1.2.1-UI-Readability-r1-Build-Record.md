# DSPi ESP32 Front Panel v1.2.1 — UI Readability r1

## Scope

Isolated local stage based on commit `5305145`. No firmware was flashed and
nothing was pushed.

The stage adds the native menu-font `b`, repairs the stray pixels in the native
menu-font `R`, standardises list values and save notifications on the medium
font, gives Analog VU colour a swatch-based selector, enlarges and simplifies
Status and Wi-Fi Transfer, and gives the Psy Bass/Sub Synth status icons measured
widths with fixed gaps.

Audio decoding, S/PDIF transmission, DSPi polling, BLE behaviour, menu actions,
preference storage, and media routing are unchanged.

## Changed source and contracts

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h`
- `local-validation/Test-DSPi-v1.2.1-Ui-Readability.py`
- `local-validation/Test-UiReadability.cpp`
- `local-validation/Test-DSPi-v1.2.1-Render-Efficiency.py`
- `local-validation/Test-DSPi-v1.2.1-Sub-Synth.py`

## Verification

- 134 Python contract tests passed.
- All four native C++ contract sources passed ESP32-S3 compiler syntax checks.
- Arduino ESP32-S3 production build completed successfully.
- Program storage: 1,925,565 bytes (61% of 3,145,728 bytes).
- Dynamic memory: 78,156 bytes (23%), leaving 249,524 bytes.
- `git diff --check` completed without whitespace errors.

## Artifacts

`DSPi-ESP32-Front-Panel-v1.2.1-UI-Readability-r1-OTA.bin`

- Size: 1,925,712 bytes
- SHA-256: `BEFB78DB35FE507756A8028C72C13742E2E0D7FD6FA1F39BE5B6DBB6E09D9AF2`

`DSPi-ESP32-Front-Panel-v1.2.1-UI-Readability-r1-Full.bin`

- Size: 16,777,216 bytes
- SHA-256: `7261CF81BF05439C13E9E6698A1DA0BC04E66157DDA92E64F41D664257D576F1`

Both images are stored in
`local-validation/builds/v1.2.1-ui-readability-20260922-r1/` so prior validation
builds remain untouched.
