# DSPi ESP32 Front Panel — Media Player S/PDIF v1.2.1 r2

Current ESP32-S3 front-panel firmware for the integrated SD-card music player
over a single-wire, logic-level S/PDIF connection to DSPi.

This release retains the verified forensic-r2 transmitter hardening and
continuous same-rate carrier. It also includes the finished local Wi-Fi
firmware updater, notification/decimal corrections and the hardware-confirmed
Fire TV remote reconnect fix.

This r2 maintenance release also includes five source commits since the
original `v1.2.1` tag: artwork caching (`f30259d`), folder-page caching
(`27d3435`), Sub Synth controls (`5305145`), the menu readability update
(`80781e0`) and the photographed glyph/key-map corrections (`114c94e`).

## Fire TV remote reconnect

Some newer Fire TV remotes wake with a private directed advertisement whose
advertiser address NimBLE reports as `00:00:00:00:00:00`. The firmware now
recognises that wake event only when exactly one valid saved bond exists and
connects using the verified stored identity. Ordinary address matching remains
unchanged.

## Other maintenance

- Browser OTA updates can start without an SD card; music transfer still
  requires one.
- Repeated Loudness/Leveller Home shortcuts remain feature actions while their
  full-screen notification is visible instead of becoming a volume step.
- The large volume decimal point is drawn once, removing loose glow pixels.
- Rotary volume control advances by exactly 1 dB per physical detent while
  retaining one-item-per-detent menu navigation.
- RLE glyph rendering now skips transparent runs, calculates a colour blend
  once per visible run, avoids per-pixel coordinate division and uses direct
  lookup for the common fonts.
- Album-art scaling precomputes source columns. Artwork timing remains governed
  by the existing audio-buffer, SD-card and JPEG-decoding safeguards.
- Verbose BLE advertisement-capture diagnostics are not present in the final
  build.

## New in r2

- Cache exact matching embedded JPEGs and decoded artwork in bounded PSRAM so
  repeated covers avoid decoding; the first load and audio-safety delay remain.
- Cache three exact directory-page requests in bounded PSRAM for repeat folder
  navigation; mount and Wi-Fi transfer transitions invalidate the cache.
- Add DSPi Sub Synth control for compatible v1.1.6-beta3 or newer firmware,
  including the menu, enabled outputs, Link Pairs, shortcut, Home icon and
  local/Console on/off notifications. Existing DSPi settings are preserved.
- Standardise list values and notifications on a readable font, enlarge Status
  and Wi-Fi Transfer information, use a swatch selector for Analog VU colour
  and separate adjacent feature icons.
- Repair the native `h` in Sub Synth and `R` in Remote, and enlarge the Remote
  Key Map's coloured button label.

The original `v1.2.1` release remains available as a rollback. The r2 tag
identifies these maintenance assets; the firmware version stays 1.2.1.

## Firmware files

- `DSPi-ESP32-Front-Panel-v1.2.1-r2-OTA.bin` — browser OTA/application-only image;
  preserves settings.
- `DSPi-ESP32-Front-Panel-v1.2.1-r2-Full.bin` — complete 16 MB USB/recovery image;
  erases existing flash contents when written at `0x0`.
- `SHA256SUMS-v1.2.1-r2.txt` — verified SHA-256 checksums.

Do not upload the 16 MB full image through the browser updater.

## Local Wi-Fi OTA update

1. Download `DSPi-ESP32-Front-Panel-v1.2.1-r2-OTA.bin`.
2. On the panel, open **System > Wi-Fi Transfer/Update** and confirm **Start**.
3. Connect to the displayed network and open the displayed browser address.
4. Under **Local firmware update**, choose the application `-OTA.bin` and select **Install firmware**.
5. Keep power connected until verification finishes and the panel restarts automatically.
6. After it restarts, remove all power for at least 10 seconds, then power it back on before using the media player. This resets the still-powered SD card so it can remount reliably.

The OTA update preserves settings and does not require an SD card. Never select the 16 MB `-Full.bin` file in the browser updater.

## Validation

- 137 focused/static/behavioural contracts passed.
- 18 Wi-Fi portal assertions passed and the complete JavaScript parsed.
- ESP32 Arduino core 3.3.11 build passed.
- Program storage: 1,924,449 bytes (61%).
- Static RAM: 78,156 bytes (23%).
- The corrected `R` and `h` bitmaps were rendered for visual review. The final
  r2 image still needs confirmation on the physical display and DSPi beta3.
