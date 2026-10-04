# DSPi ESP32 Front Panel v1.3.0

Feature release based on v1.2.1 r2.

## r2 — 2026-10-04

- Interpolate external-source spectrum frames between DSPi updates and render intermediate graph frames with partial display transfers.
- Make falling bars follow the newest reading promptly and render bars as two-pixel strips with one-pixel gaps.
- Use three-pixel peak markers with a 440 ms hold and independently persisted input/output channel colours, including preset storage and migration from existing bar colours.
- Rename Setup to Effects and remove the Spectrum waiting message.

## Initial v1.3.0 release

- Add DSPi Tube Modeller Basic and Output Limiter control menus, with matching Home indicators and the existing notification/shortcut flow where applicable.
- Add an on-demand stereo DSPi Spectrum view in place of the old digital bar VU, with selectable enabled input/output channels, independent channel colours and peak markers. Retain the analogue VU.
- Reorganise Setup, System and Remote menus; improve longer-list navigation and the presentation of feature values and notifications.
- Keep the existing SD music player, S/PDIF transmitter, local Wi-Fi updater, presets, BLE support and hardware wiring.

See [release notes](RELEASE-NOTES-v1.3.0.md) for compatibility and update instructions.
