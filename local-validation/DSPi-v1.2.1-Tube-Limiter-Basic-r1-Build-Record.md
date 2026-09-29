# Tube Modeller Basic + Output Limiter r1 — isolated local build

Built 2026-09-29 from `c26ebfa` on branch
`local/v1.2.1-stage-tube-limiter-basic`. No earlier release files were replaced.
Nothing was flashed or pushed.

## Scope

- Adds `Tube` and `Limiter` to the existing top-level carousel without moving
  old entries or changing saved action IDs. All new submenus use the existing
  `FontMedium` four-row list renderer, width-aware value columns and normal
  edit screen.
- `Tube Modeller` exposes only Enable, Tube Type, Drive, Mix and Outputs.
  Custom type is displayed if the Console has set it; selecting one of the 16
  named types deliberately applies DSPi's own type row. No character,
  rectifier, transformer or trim setting is read-modify-written by the ESP.
- `Output Limiter` opens a physical-output list, then a titled detail page for
  Enable, Threshold, Release and Link Group. Writes are followed by native
  DSPi readback, including settings adopted when an output joins a group.
- Both output lists expose only outputs enabled in DSPi's output matrix, in
  keeping with the existing Sub Synth selection rule. Limiter settings for a
  currently disabled output must be pre-armed in DSPi Console instead.
- Uses the v1.1.6 indexed float32 vendor commands, with runtime feature probes.
  Unsupported older DSPi firmware shows Unavailable without breaking existing
  controls. Visible Console edits and output-enable changes refresh while the
  relevant menu is open. No ESP audio, S/PDIF, BLE, display timing, or flash
  storage path was changed.
- DSPi owns persistence. A native preset save stores Tube; limiter settings
  follow DSPi's Output Configuration Mode: With Preset applies slot values,
  Independent requires DSPi's device-wide output-config save to survive reboot.
  This ESP stage does not change that mode or issue a flash save implicitly.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/TubeLimiter.h`
- `local-validation/Test-TubeLimiterContracts.py`
- This build record.

## Verification

- 142 Python regression/contracts passed across 18 files (137 baseline plus
  five focused new contracts).
- Independent read-only code review found no blocking issue after live enabled-
  output refresh and pinned limiter-output identity were fixed.
- `git diff --check` passed.
- Final Arduino ESP32-S3 compile succeeded with core 3.3.11, 16 MB QIO flash,
  `app3M_fat9M_16MB`, OPI PSRAM, hardware USB CDC and the existing SdFat flags.
  Program: 1,931,125 bytes (61% of 3,145,728). Static globals: 78,332 bytes
  (23% of 327,680).

## Unique artifacts

Folder: `local-validation/builds/tube-limiter-stage-20260929`.

| File | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Tube-Limiter-Basic-r1-OTA.bin` | 1,931,280 | `588481C0BB4666F0624E9E756E8E807A70FFAB8AEF69FF81937C58F843DB3FA1` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Tube-Limiter-Basic-r1-Full.bin` | 16,777,216 | `4A2DA6D17AE35B66E069064F2A95E65E0F69F7214898AF6C95E27E3CC1970F41` |

Only the OTA application belongs on the local Wi-Fi update page. The full
image is for USB and may overwrite settings.

## Hardware acceptance still required

On compatible DSPi release/v1.1.6 firmware, check Tube Basic values and a
Console-created Custom/advanced preset; edit and cancel all rows; mask only
enabled outputs; link two limiter outputs and confirm threshold/release ganging;
save/reload presets in both Output Configuration modes; confirm old DSPi
firmware still reports Unavailable; and play 44.1/48 kHz music while entering
and editing the new menus. Host tests and compile do not prove UART, screen, or
audio behaviour on hardware. Retain the previous OTA for rollback.
