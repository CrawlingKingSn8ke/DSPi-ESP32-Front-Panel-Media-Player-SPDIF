# DSPi ESP32 Front Panel v1.2.1

## S/PDIF Forensic r2

This release packages the current experimental S/PDIF media-player firmware as a clean, traceable build. It preserves the complete front-panel feature set and the earlier SD/FLAC playback work.

### S/PDIF transmitter hardening

- Verify the 24-bit consumer encoder, B/M/W cadence, parity, channel-status bytes and split-call BMC continuity at both supported rates.
- Preload all sixteen 3,072-byte DMA descriptors with valid encoded silence before transmitter enable.
- Keep a valid same-rate carrier across track transitions while still stopping cleanly for input changes, errors and genuine playback stops.
- Finish partially written encoded buffers before retaining a carrier so the encoder and queued DMA phase cannot diverge.
- Use atomic Off, Requested, Ready and Faulted retention states so errors and ordinary stops cannot preserve or resurrect an unsafe carrier.
- Add lightweight RAM-only counters for timeouts, partial writes, write/encode errors, live-carrier replacements and retained transitions.

### Music-player reliability

- Include the post-v1.2.0 SD/FLAC playback stabilisation from commit `fa5ac846818a0387da7b86320035135c5716f6da`.
- Increase decoded PCM reserve capacity with preferred, fallback and emergency PSRAM ring sizes.
- Keep I2S output isolated on CPU1 while allowing decoder/SD work to run on either core.
- Use 4 KiB decoder SD read slices for more efficient contiguous reads.
- Raise low-ring protection thresholds and add detailed SD/decoder/ring telemetry to serial command `s`.

### Other fixes carried forward

- Document the tested ESP32-to-DSPi I2S wiring.
- Fix occupied preset overwrite acknowledgement timing on compatible DSPi firmware.

## Build environment

- ESP32 Arduino core 3.3.11
- GFX Library for Arduino 1.6.5
- NimBLE-Arduino 2.5.0
- SdFat 2.3.0
- Waveshare ESP32-S3-LCD-2 / ESP32S3 Dev Module
- QIO, 16 MB flash, 3 MB application / 9.9 MB FATFS, OPI PSRAM

## Upgrade

Use the application-only image at offset `0x10000` to preserve BLE pairing, learned mappings and panel settings. Use the full merged image at offset `0x0` for a clean installation or recovery.

The release binaries were generated with ESP32 Arduino core 3.3.11. Eighty-five focused contracts and the Arduino build passed.
