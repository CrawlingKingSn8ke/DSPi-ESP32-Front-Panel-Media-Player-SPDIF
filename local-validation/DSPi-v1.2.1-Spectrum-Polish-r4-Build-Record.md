# Spectrum polish r4 — local test build

Base: `df72fa6` on `local/v1.2.1-stage-spectrum-polish-r4`.

Changed tracked files:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`
- `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`
- this build record

The Spectrum heading and its two decorative divider lines are removed. The
Source/Preset header remains unchanged. The two graphs use more height, with
20 Hz as the first displayed band. L/R and channel numbers sit at the right,
beside the right-side dB scale. dB and frequency labels follow Main Text Colour
(white by default), rather than the dim/accent role. The three 20 dB guide
intervals remain because they identify the 0/-20/-40/-60 dB scale.

The full-screen Tube Modeller notification is shortened to Tube for both local
and Console-originated changes. Spectrum pair polling is 80 ms per channel
(previously 110 ms) and DSPi RTA power averaging is 120 ms (previously
180 ms). The audio decoder, SD access, and S/PDIF transmitter were not edited;
existing low-buffer gating and non-blocking display transfers still protect
music playback. The quicker visual response requires hardware testing.

All 22 Python test scripts passed. Build-only passed with ESP32 core 3.3.11,
GFX 1.6.5, NimBLE 2.5.0, and SdFat 2.3.0. Sketch: 1,938,821 bytes (61%) of
app space; global data: 78,516 bytes (23%) of dynamic memory. No device was
flashed or GitHub repo changed.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r4-OTA.bin` | 1,938,976 | `E437E45B460AF969AEE02C8845DAC40C025D8ECDE97E4118C081113E59C472C3` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r4-Full.bin` | 16,777,216 | `C417C8E4C3AD74559A0A463B83D6D3184112741C3CFE1876283990C9677DDC54` |

The new images are kept separately in
`local-validation/builds/v1.2.1-spectrum-polish-20260930-r4/`; r3 remains
available for rollback. Use the OTA image on the local Wi-Fi update page for
hardware testing. If Media cannot read the SD card after updating, power-cycle
before testing it, per the existing portal lifecycle.
