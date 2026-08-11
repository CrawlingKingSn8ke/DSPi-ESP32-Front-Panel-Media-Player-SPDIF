# DSPi ESP32 Front Panel v1.2.1 S/PDIF 24-bit Test

- Baseline: public v1.2.1 commit `aa9c30f8772fd043ebdc7c47a153d31d5b500f22`
- Local source commit: `a94d7c0` (`local/v1.2.1-spdif-output-test`)
- Remote push: none
- Flash performed: none
- Supported media rates: 44.1 kHz and 48 kHz only
- PCM depth carried into the encoder: 24 significant bits

## Test wiring

1. Disconnect any other transmitter, including the WiiM S/PDIF output, from DSPi S/PDIF input 1.
2. Connect ESP32-S3 GPIO13 through a 47-100 ohm series resistor to the DSPi Pico GPIO5 S/PDIF input.
3. Connect ESP32 ground to DSPi ground.
4. The old media BCLK and LRCLK wires are not used by this build.

This is a direct 3.3 V logic-level test link, not a consumer coaxial S/PDIF electrical output.

## Changed source files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/MediaPlayerPoC.cpp`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/MediaPlayerPoC.h`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpdifBlockEncoder.cpp`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpdifBlockEncoder.h`
- `local-validation/Test-DSPi-v1.2.1-SPDIF-Output.py`

## Verification

- Arduino ESP32-S3 compile: passed
- Existing regression contracts: 118 passed
- Focused S/PDIF contracts: 14 passed
- Total: 132 passed
- Program storage reported by Arduino: 1,896,197 bytes of 3,145,728 (60%)
- Globals reported by Arduino: 76,596 bytes of 327,680 (23%)
- The exact 256-entry 24-bit BMC lookup table was compared with the tested MIT-licensed `sle118/squeezelite-esp32` implementation.
- Route contracts verify that the experiment selects DSPi S/PDIF input 1, validates Pico GPIO5, and does not alter DSPi I2S rate or clock-mode settings.

## Artifacts

### Application-only image

- File: `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-24bit-Test-Application.bin`
- Size: 1,896,352 bytes
- SHA-256: `171B2FBE1310185F198E3DC5E699AC28FB048EF33B9B1FA275FBCEFDB21F2D1E`
- Flash offset: `0x10000` (preserves the settings partitions)

### Full 16 MB image

- File: `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-24bit-Test-Full.bin`
- Size: 16,777,216 bytes
- SHA-256: `0BB403ED25061A2818FF69078A50B0F69B627EC4CEB911D08FA2B22EE61B48F1`
- Flash offset: `0x0` (full-flash test image; overwrites settings)

## Implementation notes

- The ESP32 I2S peripheral is used internally as a stable DMA-backed master serializer at twice the audio sample rate; externally, only the encoded S/PDIF data wire is connected.
- The existing high-priority isolated output task and large PCM reserve remain in use.
- Pause, seek, underrun, route hold, startup lead-in and track-tail paths emit valid encoded digital silence rather than raw zero words.
- This is an experimental hardware-validation build. Successful compilation and static contracts cannot replace a real DSPi lock and long-play test.
