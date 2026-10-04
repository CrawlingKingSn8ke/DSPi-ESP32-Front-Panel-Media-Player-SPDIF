# DSPi ESP32 Front Panel — Media Player S/PDIF v1.3.0

This release advances the v1.2.1 r2 front panel and music player with DSPi v1.1.6 controls and a new visualiser. The S/PDIF wiring and playback route are unchanged.

## New since v1.2.1 r2

- Tube Modeller Basic controls: enable, tube selection, Drive, Mix and enabled outputs. Advanced settings configured in DSPi Console are preserved.
- Output Limiter controls under Setup, using the available DSPi outputs.
- Stereo Spectrum replaces the digital bar VU page. Choose an enabled input or output pair, customise each channel's bar colour and see animated peak markers. It is active only while the Spectrum view is selected; analogue VU is retained.
- Reorganised Setup and System menus, clearer Remote key mapping, continuation arrows for long lists, larger Home feature indicators and full-screen notifications. The Spectrum page also supports volume, mute and feature notifications.

Tube Modeller, Output Limiter and Spectrum require a compatible DSPi v1.1.6 firmware that exposes their commands and telemetry. The earlier `v1.2.1-r2` release remains available for rollback.

## Firmware files

- `DSPi-ESP32-Front-Panel-v1.3.0-OTA.bin` — application-only image for the local Wi-Fi updater; preserves settings.
- `DSPi-ESP32-Front-Panel-v1.3.0-Full.bin` — complete 16 MB image for first-time USB installation or recovery; writing it at `0x0` replaces existing flash contents and settings.
- `SHA256SUMS-v1.3.0.txt` — SHA-256 checksums for both firmware images.

Never upload the 16 MB Full image through the browser updater.

## Build verification

- 24 local test scripts passed.
- ESP32 Arduino core 3.3.11 build passed: 1,941,577 bytes of program storage and 78,924 bytes of global RAM.
- OTA image: 1,941,728 bytes; SHA-256 `1C88EB4F7D20816180B55525584E9C9726EED0193EC40FB1D798BA6EA1E2EF9B`.
- Full image: 16,777,216 bytes; SHA-256 `7012E68A379B82087A8AF104EEFF4FC04D8E2BCFC62D1C7BDD1548EA9DE06FAE`.

The images were built locally and were not flashed as part of release preparation.

## Local Wi-Fi OTA update

1. Stop or pause music, then open **System > Wi-Fi Transfer/Update** and confirm **Start**.
2. Connect to the displayed Wi-Fi network and open the address shown on the panel.
3. Select the application `-OTA.bin` under **Local firmware update** and install it. Keep power connected until verification and restart complete.
4. Switch off all power for at least 10 seconds, then power the unit back on before using the media player. A software restart does not power-cycle the SD card.

OTA does not require an inserted SD card. File transfer does.
