# DSPi ESP32 Media Player S/PDIF v1.2.1

Current ESP32-S3 front-panel firmware for the integrated SD-card music player
over a single-wire, logic-level S/PDIF connection to DSPi.

This release retains the verified forensic-r2 transmitter hardening and
continuous same-rate carrier. It also includes the finished local Wi-Fi
firmware updater, notification/decimal corrections and the hardware-confirmed
Fire TV remote reconnect fix.

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
- Verbose BLE advertisement-capture diagnostics are not present in the final
  build.

## Firmware files

- `DSPi-ESP32-Front-Panel-v1.2.1-OTA.bin` — browser OTA/application-only image;
  preserves settings.
- `DSPi-ESP32-Front-Panel-v1.2.1-Full.bin` — complete 16 MB USB/recovery image;
  erases existing flash contents when written at `0x0`.
- `SHA256SUMS-v1.2.1.txt` — verified SHA-256 checksums.

Do not upload the 16 MB full image through the browser updater.

## Validation

- 113 focused/static/behavioural contracts passed.
- 18 Wi-Fi portal assertions passed and the complete JavaScript parsed.
- ESP32 Arduino core 3.3.11 build passed.
- Program storage: 1,917,229 bytes.
- Static RAM: 77,388 bytes.
