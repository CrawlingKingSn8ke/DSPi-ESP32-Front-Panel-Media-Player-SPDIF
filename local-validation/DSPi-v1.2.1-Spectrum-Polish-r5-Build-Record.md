# Spectrum polish r5 — local test build

Base: `a1a5754` on `local/v1.2.1-stage-spectrum-polish-r5`.

Changed tracked files:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`
- this build record

The Spectrum dB labels are now on the left, right-aligned into a narrow scale
column. L/R and channel number remain on the right. Both plots are 70 pixels
high and are separated by 17 pixels, leaving 9 pixels of clear vertical space
between the upper -60 and lower 0 eight-pixel labels. The first displayed
band and frequency label remain 20 Hz. Source/Preset position, colours,
analyser cadence, averaging, channel selection, and music/audio paths are
unchanged from r4.

All 22 Python test scripts passed. Build-only passed with ESP32 core 3.3.11,
GFX 1.6.5, NimBLE 2.5.0, and SdFat 2.3.0. Sketch: 1,938,833 bytes (61%) of
app space; global data: 78,516 bytes (23%) of dynamic memory. No device was
flashed or GitHub repo changed.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r5-OTA.bin` | 1,938,976 | `60B0A34338D5EE7C0ED342301125C4F4110EEB7105CF7F39BE0348324E97B399` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r5-Full.bin` | 16,777,216 | `8CA5C9DEFFE81EF68C149D8AF4FEA42B56973AFB59F46FA3C666D7D709DA08E0` |

Images are kept separately under
`local-validation/builds/v1.2.1-spectrum-polish-20261001-r5/`; r4 remains
available for rollback. Use the OTA image on the local Wi-Fi update page for
hardware testing. If Media cannot read the SD card after updating, power-cycle
before testing it, per the existing portal lifecycle.
