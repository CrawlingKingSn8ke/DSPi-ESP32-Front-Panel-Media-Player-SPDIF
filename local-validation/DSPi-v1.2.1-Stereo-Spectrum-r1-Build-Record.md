# Stereo Spectrum r1 — local build record

Base: `945f1a6` (`local/v1.2.1-stage-ui-polish-r3`)

Stage branch: `local/v1.2.1-stage-spectrum-stereo-r1`

Changed source and contracts:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`

The former full-screen digital bar VU page is removed. The VU action now cycles
Home → stereo spectrum → analogue VU → Home. Idle Screen option 2 is labelled
Spectrum; its persisted numeric value is unchanged, so an existing saved
digital-VU preference selects Spectrum without an NVS migration. The analogue
VU and the Home output level meters are unchanged.

Only DSPi RTA V3 output channels 0 and 1 (physical outputs 1 and 2, L/R) are
requested. The ESP validates the 82-byte per-channel frame, RTA capabilities,
and active output-pair configuration. It requests the analyser only while the
spectrum page is visible and stops its own configuration on exit. A concurrent
control-surface config change relinquishes ownership instead of displaying
other channels as L/R. Older DSPi firmware shows an Unavailable message.

During Media playback, existing low-buffer/transition suppression applies to
spectrum polling. Spectrum redraws use the bounded shared-SPI try-lock and
are skipped if SD owns the bus. The music decoder and S/PDIF transmitter were
not changed.

Validation: all 22 Python test scripts passed; Arduino ESP32-S3 build-only
passed with ESP32 core 3.3.11, GFX 1.6.5, NimBLE 2.5.0, SdFat 2.3.0.
No serial port was opened, nothing was flashed, and nothing was pushed.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Stereo-Spectrum-r1-OTA.bin` | 1,936,032 | `06F13C5A3E03D33F7B433B85E329F3889779A4DF5F2137837EE979FEA85E4DD8` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Stereo-Spectrum-r1-Full.bin` | 16,777,216 | `BDC0C9702CF405FFCB1A23EF84D1285DCE8B87B28702BD89046DE82E9A8A7ACA` |

Images are kept under
`local-validation/builds/v1.2.1-stereo-spectrum-20260930-r1/` and are not
part of the tracked source commit. Use the OTA image on the existing local
Wi-Fi update page for hardware testing. A power cycle may be needed before
the Media player can read the SD card again after Wi-Fi update, per the
existing portal lifecycle.
