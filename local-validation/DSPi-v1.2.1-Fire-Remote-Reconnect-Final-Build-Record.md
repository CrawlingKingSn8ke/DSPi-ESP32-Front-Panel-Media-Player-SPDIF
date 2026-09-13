# Fire Remote Reconnect Final — 2026-09-13

This final local build retains the hardware-confirmed G0G1W directed-wake
reconnect fix. Nothing was flashed, committed, pushed, or overwritten.

## Finalisation

- A zero-address directed wake can initiate reconnect only when the panel has
  a valid profile, exactly one controller bond exists, and the saved identity
  is confirmed in that bond store. Connection uses the saved identity rather
  than the unusable zero address.
- The verbose per-advertisement and scan-end capture diagnostics were removed.
- The reconnect watchdog no longer reports or schedules an unexpected scan
  retry while the intentional connection worker is already active.
- Concise normal boot, reconnect, directed-wake, connection, security and
  subscription messages remain available.
- Audio scan suspension, UI, media, S/PDIF, DSPi protocol, Wi-Fi, storage and
  OTA-write behavior are unchanged.

## Verification

- Regression contracts were observed failing before both finalisation changes
  and passing afterward.
- 113 Python firmware tests passed.
- 18 Wi-Fi portal assertions passed; the complete portal JavaScript parsed.
- ESP32-S3 build succeeded with Arduino ESP32 core 3.3.11,
  GFX Library for Arduino 1.6.5, NimBLE-Arduino 2.5.0 and SdFat 2.3.0.
- Program storage: 1,917,229 / 3,145,728 bytes (60%).
- Static RAM: 77,388 / 327,680 bytes (23%).

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| DSPi-ESP32-Front-Panel-v1.2.1-Fire-Remote-Reconnect-Final-OTA.bin | 1,917,376 | `7114EB7B34567901A8D24F64CAD36DF6553A96A6C79F0714E087EE4772BBE9BD` |
| DSPi-ESP32-Front-Panel-v1.2.1-Fire-Remote-Reconnect-Final-Full.bin | 16,777,216 | `81215910F230EC241DE025208A42A11E4683F0A2247A00158E91654D258FFAA4` |
