# DSPi ESP32 Front Panel — Media Player S/PDIF v1.4.0

Version 1.4.0 extends the front panel with output configuration and precise audio controls. It builds on v1.3.1; the music player, S/PDIF wiring, Spectrum, analogue VU, themes, BLE support and local Wi-Fi updater remain included.

## Changes since v1.3.1

- **Outputs menu:** select a named output to adjust mute, gain, delay, routing, enable, name and EQ/crossover.
- **Equaliser and crossover:** edit all 10 output EQ bands and four crossover slots, including filter-specific fields. Input EQ is available under Effects. Dedicated digit editors provide precise frequency, gain and Q adjustment with clear editing indicators.
- **Copy Parameters:** copy all output EQ bands, crossover slots, gain, delay and mute to a selected destination. The destination is muted during the operation and native readback checks each setting. Routing, names, output enable and hardware assignments stay separate.
- **Editable Mono L+R:** choose the source pair, set Left and Right route gains independently at 0.01 dB resolution and confirm Save. No fixed attenuation is imposed.
- **Quick Controls and groups:** speaker/subwoofer mute, Sub Level, Balance and scoped Restore Saved controls, backed by manually assigned Left/Right/Sub output groups.
- **Preset Tools:** Save Changes, Save As New, New Preset, Rename and Revert, with destination previews and confirmations.
- **Custom inputs:** full and short source names, menu visibility and order under System > Inputs. Renamed inputs appear in panel source notifications and Home labels.
- **Hardware menu:** native output-format information and supported pin, S/PDIF/I2S input, clock, ADAT and preset-mode controls.
- **Menu documentation:** a complete graphical map, individual detailed sheets and an accessible text reference.

## Using the controls

Speaker Groups start unassigned: choose the appropriate outputs under **Outputs > Setup**. Group assignment does not change native routing, output enable or GPIO wiring. Everyday group On/Off uses mute.

**Sum Mode** replaces the routes feeding the chosen output with the selected pair at the gains you specify; other routes to that output are disabled. Summing at high gains can clip. Select suitable headroom rather than assuming the panel adds attenuation.

Configuration changes apply live. Use the normal preset-save action to retain DSPi changes. Input aliases/order/visibility are panel preferences, not native input enable settings. Hardware configuration must match actual wiring: the panel validates GPIO choices and protects UART pins, but does not detect every assignment conflict. Check existing pin ownership in Console.

Multi-setting operations use sequential native commands, not atomic transactions. If interrupted or rejected, some settings may already have changed and a copy destination may remain muted. Inspect the channel before resuming; do not assume automatic rollback.

## Compatibility

Use compatible DSPi firmware exposing the required UART controls. Unavailable controls remain conditional on DSPi capabilities, active inputs, enabled outputs and platform. This release does not include or modify DSPi firmware. Tube Modeller remains Basic on the panel; advanced Console parameters remain DSPi-owned.

The ESP audio transmitter, SD playback implementation and partition layout are unchanged from v1.3.1.

## Files and installation

- `DSPi-ESP32-Front-Panel-v1.4.0-OTA.bin`: application-only image for the local Wi-Fi updater, preserving panel settings.
- `DSPi-ESP32-Front-Panel-v1.4.0-Full.bin`: complete 16 MB image for first USB installation or recovery at address `0x0`. Replaces flash contents and settings.
- `SHA256SUMS-v1.4.0.txt`: checksums for the firmware images.
- `DSPi-ESP32-Menu-Map-v1.4.0.zip`: graphical and text menu reference, also under `docs/menu-map/` in source.
- Source includes versioned Windows build/flash scripts and the pinned dependency versions.

### Local Wi-Fi update

1. Stop or pause music. Open **System > Wi-Fi Transfer/Update** and confirm **Start**.
2. Connect to the displayed network and open the displayed browser address.
3. Upload the **OTA** application under **Local firmware update**. Keep power connected until verification and restart complete.
4. After restart, disconnect all power for at least 10 seconds, then reconnect before using the media player so the SD card is power-cycled.

An SD card is not required for OTA. Never upload the 16 MB Full image through the browser.

See the [README](README.md) for wiring and USB instructions and the [menu map](docs/menu-map/README.md) for navigation.
