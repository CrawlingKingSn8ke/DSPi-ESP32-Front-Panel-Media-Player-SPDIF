# DSPi ESP32 Front Panel v1.2.1 — UI Glyph and Key Map r2

## Base and scope

Local branch `local/v1.2.1-ui-glyph-keymap-r2` builds on UI readability commit
`80781e0`. The earlier r1 artifacts remain available. Nothing was flashed or
pushed.

The photographed `Sub Synth` accent was a second, unintended top stem in the
native `h` bitmap. The `Remote` glyph also contained stray pixels and a bright
cluster inside its counter. Both glyph bitmaps were repaired; the old two-pixel
runtime mask for `R` was removed. The Remote Key Map's colored button label now
uses the medium font with width-aware truncation.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h`
- `local-validation/Test-UiReadability.cpp`
- `local-validation/Test-DSPi-v1.2.1-Glyph-Keymap-r2.py`
- This record.

## Verification

- 137 Python contracts passed.
- Four native C++ contract sources passed ESP32-S3 compiler syntax checks.
- A rendered glyph preview confirmed the `R` counter is clean and the `h` has
  one ascender. The preview is in the isolated build folder.
- Arduino ESP32-S3 production build succeeded.
- Program storage: 1,924,449 bytes (61% of 3,145,728 bytes).
- Dynamic memory: 78,156 bytes (23%), leaving 249,524 bytes.

## Artifacts

`DSPi-ESP32-Front-Panel-v1.2.1-UI-Glyph-Keymap-r2-OTA.bin`

- Size: 1,924,592 bytes
- SHA-256: `EDEC1D5C4899EC8EDD4403AF79EF2F58D5222E73E9B71AE2124A7A73E3323559`

`DSPi-ESP32-Front-Panel-v1.2.1-UI-Glyph-Keymap-r2-Full.bin`

- Size: 16,777,216 bytes
- SHA-256: `22B9A1A14C17BB4EAFD0481ED12346C263DF2819E3CA5EB25E02F7FEEC8A8DB6`

Both images are in `local-validation/builds/v1.2.1-ui-glyph-keymap-20260922-r2/`.
