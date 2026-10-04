# DSPi ESP32 Front Panel — Media Player S/PDIF v1.3.1

This release advances the v1.2.1 r2 front panel and music player with DSPi v1.1.6 controls and a new visualiser. The S/PDIF wiring and playback route are unchanged.

## Changes since v1.3.0

- Smoother Spectrum animation for external DSPi sources using interpolation between readings and partial graph redraws.
- Prompt downward bar response, two-pixel segmented bars and three-pixel peak markers with a 440 ms hold.
- Independent per-channel Bar and Peak colour swatches in **Spectrum > Bar Colours**, saved globally and with presets. Existing settings inherit their saved bar colours for peaks.
- **Effects** replaces the Setup menu name. The Spectrum idle view retains the resting peak lines without a waiting message.

## New since v1.2.1 r2

- Tube Modeller Basic controls: enable, tube selection, Drive, Mix and enabled outputs. Advanced settings configured in DSPi Console are preserved.
- Output Limiter controls under Effects, using the available DSPi outputs.
- Stereo Spectrum replaces the digital bar VU page. Choose an enabled input or output pair, customise each channel's bar colour and see animated peak markers. It is active only while the Spectrum view is selected; analogue VU is retained.
- Reorganised Effects and System menus, clearer Remote key mapping, continuation arrows for long lists, larger Home feature indicators and full-screen notifications. The Spectrum page also supports volume, mute and feature notifications.

Tube Modeller, Output Limiter and Spectrum require a compatible DSPi v1.1.6 firmware that exposes their commands and telemetry. The earlier `v1.2.1-r2` release remains available for rollback.

## Firmware files

- `DSPi-ESP32-Front-Panel-v1.3.1-OTA.bin` — application-only image for the local Wi-Fi updater; preserves settings.
- `DSPi-ESP32-Front-Panel-v1.3.1-Full.bin` — complete 16 MB image for first-time USB installation or recovery; writing it at `0x0` replaces existing flash contents and settings.
- `SHA256SUMS-v1.3.1.txt` — SHA-256 checksums for both firmware images.

Never upload the 16 MB Full image through the browser updater.

## Build verification

- 187 tests passed across 25 local scripts.
- ESP32 Arduino core 3.3.11 build passed: 1,943,469 bytes of program storage and 79,132 bytes of global RAM.
- OTA image: 1,943,616 bytes; SHA-256 `6A79FE16636A0A05A860A6813B3F9A896655A810DFBB175BB4A08E831B090593`.
- Full image: 16,777,216 bytes; SHA-256 `D2E796EF333FB2B8E54145AE3F2AC42F1BF58D6F324F8801BD51A53A08D33E2C`.

The images were built locally and were not flashed as part of release preparation.

## Local Wi-Fi OTA update

1. Stop or pause music, then open **System > Wi-Fi Transfer/Update** and confirm **Start**.
2. Connect to the displayed Wi-Fi network and open the address shown on the panel.
3. Select the application `-OTA.bin` under **Local firmware update** and install it. Keep power connected until verification and restart complete.
4. Switch off all power for at least 10 seconds, then power the unit back on before using the media player. A software restart does not power-cycle the SD card.

OTA does not require an inserted SD card. File transfer does.
