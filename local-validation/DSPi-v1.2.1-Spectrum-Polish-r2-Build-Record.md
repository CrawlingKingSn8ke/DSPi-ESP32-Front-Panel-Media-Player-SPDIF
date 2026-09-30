# Spectrum polish r2 — local build record

Base: `282c5d6` (`local/v1.2.1-stage-spectrum-stereo-r1`)

Stage branch: `local/v1.2.1-stage-spectrum-polish-r2`

Changed source and contract:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`

The Spectrum page now uses the established medium menu heading and tapered
rules. It presents DSPi Source and Preset in the same header positions and
size as Home, larger L/R labels, themed frequency labels, a visible dB scale,
and three 20 dB bar intervals from 0 to -60 dBFS. The previous digital VU
remains removed; the analogue VU, Home meters, music decoder, SD access, and
S/PDIF transmitter were not changed.

Screen Settings → Spectrum provides Source (Inputs/Outputs), Upper Channel,
and Lower Channel. Input choices follow the DSPi active-input count. Output
choices follow the matrix mixer's enabled outputs. The selected pair is saved
in a versioned, separate NVS record; the existing panel-settings record is
unchanged for migration safety. A separate preset key keeps this selection
with saved panel settings. DSPi RTA V3 is polled only while Spectrum is visible,
only for the selected channels. The ESP checks the live channel state and
never stops a Console-owned RTA configuration. Manual Spectrum and analogue
VU views stay at the selected brightness; timeout-invoked views retain the
existing idle-screen policy.

Validation: all 22 Python test scripts passed. Build-only passed with ESP32
core 3.3.11, GFX 1.6.5, NimBLE 2.5.0, and SdFat 2.3.0. The sketch uses
1,938,841 bytes (61%) of the app partition and 78,516 bytes (23%) of dynamic
memory. No serial port was opened, nothing was flashed, and nothing was pushed.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r2-OTA.bin` | 1,938,992 | `CAD34E444539855DFBD06297E58423955344862F1FBB6F11A5187894ECEAA9A0` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r2-Full.bin` | 16,777,216 | `3E61A7DB2F3301412156B2C2CB5862941770CFD3FB7604448BE6C9237747D79B` |

Images are kept under
`local-validation/builds/v1.2.1-spectrum-polish-20260930-r2/` and are not
part of the tracked source commit. Use the OTA image on the existing local
Wi-Fi update page for hardware testing. A power cycle may be needed before
the Media player can read the SD card again after Wi-Fi update, per the
existing portal lifecycle.
