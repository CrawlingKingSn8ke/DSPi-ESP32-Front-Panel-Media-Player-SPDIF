# Spectrum peaks and notifications r7 — local test build

Base: `b576430` (r6 bar-colour stage). Branch: `local/v1.2.1-stage-spectrum-peaks-overlays-r7`.

Changed tracked files:

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `local-validation/Test-DSPi-v1.2.1-Notification-Shortcut-Decimal.py`
- `local-validation/Test-DSPi-v1.2.1-Spectrum-Bar-Colours.py`
- `local-validation/Test-DSPi-v1.2.1-Stereo-Spectrum.py`
- `local-validation/Test-DSPi-v1.2.1-Spectrum-Peaks-Overlays.py`
- This record

Spectrum now calls the same top-status renderer as Home, so the source, preset and currently enabled feature icons retain their Home positions, sizes, spacing and colour role. Each actual DSPi band has a one-pixel marker in its independently selected channel colour, with the same inter-bar gap as the bar. The marker rests at the −60 dB baseline, follows a new high point immediately, holds for 240 ms and then descends at 42 pixels/s until caught by another hit or the baseline. Peak state updates only when a valid band frame arrives, not on each LCD redraw.

The bonded remote's Home D-pad shortcuts also apply on Spectrum. Loudness, Crossfeed, Leveller, Psy Bass, Sub Synth, Tube and Mute use the existing full-screen On/Off card and return to Spectrum. Volume uses a full-screen card; held repeats redraw only the numeric strip. Confirmed external volume, feature, preset and input changes are announced on Spectrum. Temporary cards pause RTA polling without relinquishing its configuration or peak state. If a preset changes the selected Spectrum channels, the analyser verifies ownership before reconfiguring; it never overwrites a Console-owned analyser session. Existing automatic screen-timeout wake/consume behaviour remains intact.

No music decoding, S/PDIF output, DSP audio path or DSPi meter protocol was changed. All 24 Python test files passed; `git diff --check` passed. Build-only succeeded with ESP32 core 3.3.11, GFX 1.6.5, NimBLE 2.5.0 and SdFat 2.3.0. Sketch: 1,941,617 bytes (61%); globals: 78,924 bytes (24%).

| Image | Bytes | SHA-256 |
|---|---:|---|
| `local-validation/builds/v1.2.1-spectrum-peaks-overlays-20261001-r7/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Peaks-Overlays-r7-OTA.bin` | 1,941,760 | `CD9528D594EEEC4DEC7415AFFB1DF774FCA99B995AACA8D03376110546BFEC85` |
| `local-validation/builds/v1.2.1-spectrum-peaks-overlays-20261001-r7/DSPi-ESP32-Front-Panel-v1.2.1-Spectrum-Peaks-Overlays-r7-Full.bin` | 16,777,216 | `F0612A54C7087F51CCECFD917D4A7C4527FC4BC7E850BEF8E4F54329ABD75CD3` |

No device was flashed and nothing was pushed. The r6 images remain available for rollback. Hardware animation and audio stability still require listening/display testing on the actual panel.
