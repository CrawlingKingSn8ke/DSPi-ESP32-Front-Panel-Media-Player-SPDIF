# DSPi ESP32 Front Panel — Media Player S/PDIF v1.5.0

## New since v1.4.0

- SMB network music playback from Windows and NAS folder shares, with album artwork, scrolling filenames, familiar Now Playing controls and saved share settings.
- DSPi Link for local-network Console access through the panel, with PIN pairing and browser access. Panel, encoder and BLE controls remain usable with external-source audio.
- QR-guided Wi-Fi setup, network selection, connected signal display and home-screen Wi-Fi indicator. Network playback selects the strongest matching access point first, with fallback.
- Retro theme with seven-segment volume, pixel-style text, faded inactive segments and a separate volume colour.
- Selectable User or Master Volume display/control under Screen Settings.
- Shared DSPi physical-input names with compatible v1.1.6 Console/firmware, plus local visibility/order controls.
- Explicit Back entry beneath Preset Tools.
- Current DSPi v1.1.6 control-interface version 22 integration.

Existing SD playback, S/PDIF output, BLE pairing/mappings, output and EQ controls, effects, analogue VU, stereo Spectrum and Wi-Fi transfer/update remain included.

## Downloads

- **OTA:** `DSPi-ESP32-Front-Panel-v1.5.0-OTA.bin` — upload through **System > WiFi > Transfer/Update**. Preserves panel settings and BLE bonds.
- **Full USB:** `DSPi-ESP32-Front-Panel-v1.5.0-Full.bin` — 16 MB image for first installation/recovery at `0x0`; never upload it through OTA.
- **Source:** `DSPi-ESP32-Front-Panel-v1.5.0-Source.zip` — complete release source and build scripts, including bundled dependencies and licences.
- **Checksums:** `SHA256SUMS-v1.5.0.txt`.

After OTA restarts, disconnect all power for 10 seconds before using SD playback. Follow the panel's stop/restart prompts when leaving Network or DSPi Link. Link and local music playback are mutually exclusive.

Use compatible DSPi v1.1.6 firmware for current source names, Link and extended controls. The ESP firmware does not install or modify DSPi firmware. The direct audio connection remains 3.3 V GPIO-level S/PDIF, 24-bit at 44.1/48 kHz, not consumer coaxial S/PDIF.
