# DSPi v1.2.1 Render Efficiency r1 Build Record

- Date: 2026-09-14
- Baseline: public `main` commit `765f494`
- Source branch: `local/v1.2.1-glyph-rle-blend-hoist`
- ESP32 Arduino core: 3.3.11
- GFX Library for Arduino: 1.6.5
- NimBLE-Arduino: 2.5.0
- SdFat: 2.3.0
- Flashing: not performed

## Changes

- Hoist full-precision RGB565 blending once per visible glyph RLE run.
- Skip transparent RLE runs without entering the pixel loop.
- Walk visible glyph coordinates incrementally rather than dividing each pixel position.
- Use verified direct lookup for common Small, Medium and Large font characters.
- Precompute album-art source columns without changing nearest-neighbour output.
- Retain the existing artwork delay, audio-ring admission rules, SD access and JPEG decoding.

## Static analysis

- Glyph blend calls across all 230 stored glyphs: 92,740 to 35,906 (61.3% reduction).
- Transparent decoded glyph positions skipped: 146,537 of 239,277 (61.2%).
- Album-art scaling coordinate divisions: approximately 98.2% fewer for a 112 x 112 image.
- Coordinate and alpha output equivalence checked across all 230 stored glyphs.

## Verification

- 123 focused/static/behavioural firmware contracts passed.
- 18 Wi-Fi portal assertions passed; complete JavaScript parsed.
- Arduino build passed; no serial port was opened.
- Program storage: 1,917,249 bytes (60% of 3,145,728).
- Static RAM: 78,028 bytes (23% of 327,680).

## Release artifacts

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-OTA.bin` | 1,917,392 | `4F8595B6C152CBC96C06B34ECADDFDAC1369B392D8BD864501A92251BFD84AB3` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Full.bin` | 16,777,216 | `AB31406C065863E3C49EE0E125748A6BFA4789D3056CC3E53FC6A902C85F5EF3` |
