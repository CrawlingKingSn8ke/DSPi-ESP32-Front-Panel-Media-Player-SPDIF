# Spectrum polish r3 — final local test build

Base: `282c5d6` on `local/v1.2.1-stage-spectrum-polish-r2`.

This is the r2 Spectrum layout and channel-selection update with one additional
menu guard: a failed DSPi availability query now clears the known-channel flag,
so the selector cannot display stale channel choices. The r2 binaries remain
preserved separately and were not overwritten.

The Spectrum page uses the Menu font and tapered rules, Home-sized Source and
Preset header labels, larger L/R labels, themed frequency text, and visible
0/-20/-40/-60 dB guide lines. Screen Settings → Spectrum selects Inputs or
Outputs and up to two available channels. Inputs follow the DSPi active input
count; outputs follow the matrix mixer enabled state. Selection is saved in a
versioned separate NVS record and with ESP preset panel settings. Only the
selected pair is polled while Spectrum is visible. Manual Spectrum and analogue
VU selection do not dim; automatic idle-screen behavior remains unchanged.

Changed tracked files:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`
- this build record and the earlier r2 record

All 22 Python test scripts passed. Build-only passed with ESP32 core 3.3.11,
GFX 1.6.5, NimBLE 2.5.0, and SdFat 2.3.0. Sketch: 1,938,885 bytes (61%) of
app space; global data: 78,516 bytes (23%) of dynamic memory. No device was
flashed. The music decoder, SD access, and S/PDIF transmitter were not edited.

| Image | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r3-OTA.bin` | 1,939,040 | `23D8E849599C7958C30749B2220A075A23CA59D73047E7A34C1E62D4D0CF736C` |
| `DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Polish-r3-Full.bin` | 16,777,216 | `A4A32512C18C1790DA7942B258E94C0691539E9FDD6784C115219A282A7B2748` |

Images are in `local-validation/builds/v1.2.1-spectrum-polish-20260930-r3/`
and are ignored by Git. Use the OTA image on the local Wi-Fi update page for
hardware testing; if the SD card is not accessible afterwards, power-cycle
the unit before testing the music player, per the existing portal lifecycle.
