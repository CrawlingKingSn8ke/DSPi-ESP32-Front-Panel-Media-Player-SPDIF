# Experimental S/PDIF media-player build

## Purpose

This fork replaces the earlier three-wire ESP-to-DSPi I2S media link with a single-wire consumer-PCM S/PDIF transmitter implemented through the ESP32-S3 I2S peripheral.

- ESP output: GPIO13
- DSPi input: S/PDIF input 1 on Pico GPIO5
- Supported rates: 44.1 kHz and 48 kHz
- Encoded depth: 24-bit PCM
- DSPi control UART: unchanged

## Current hardening

- DMA auto-clear is disabled because raw zero DMA words are not valid biphase-mark S/PDIF silence.
- Sixteen DMA descriptors are retained instead of eight.
- Each descriptor contains exactly one complete 192-frame S/PDIF block.
- Every descriptor is primed with valid encoded silence before route activation.
- All 49,152 bytes are preloaded while the I2S channel is still stopped; the
  transmitter is enabled only after the complete DMA ring is valid.
- IEC 60958 consumer-PCM channel status identifies 24-bit 44.1/48 kHz audio,
  with the same status and correct parity on the left and right subframes.
- DMA reserve is approximately 64 ms at 48 kHz and 70 ms at 44.1 kHz.
- The output task and I2S interrupt have elevated, deterministic priorities.
- Same-rate track changes retain the live transmitter and fill the complete DMA
  ring with block-aligned encoded silence, avoiding receiver reacquisition.
- A 44.1/48 kHz change still performs a controlled transmitter restart.
- GPIO13 is explicitly driven low after playback stops.

## Observed hardware behaviour

Two DSPi systems have behaved differently:

1. A system with four PCM5102 DAC boards is stable with this S/PDIF music-player firmware, with no observed dropout or distortion.
2. A second system uses a TV S/PDIF input, a DSPi S/PDIF output feeding an external DAC, and a PCM5102 on a DSPi I2S output. The ESP source originally exposed receiver/output dropouts that were not present with the older inputs. Testing the experimental DSPi `spdif_rx_overhaul` receiver code removed those recurring dropouts. The ESP transmitter has remained stable with that receiver build.

Both the ESP and an independent TV S/PDIF transmitter can produce one small click roughly ten seconds after the first fresh receiver lock. It does not repeat while the receiver remains locked. That shared behaviour points to receiver clock settling rather than the ESP encoder or SD/audio pipeline.

Keep the internal 3.3 V logic connection and its ground return short. A tested
68-ohm series resistor did not improve this installation and is not required by
the current wiring guidance.

## Verified OTA application

- File: `DSPi-ESP32-Front-Panel-v1.2.1-Continuous-SPDIF-Carrier-OTA.bin`
- Size: 1,911,808 bytes
- SHA-256: `1A77C60E10644145CEC09822BD6A337D2EE6BF5E51A65B626843E262D6132B44`
- Focused contracts: 58 passed
- ESP32 Arduino core: 3.3.11

Use the application-only file for browser OTA. Do not upload the 16 MB full-flash image through the browser updater.
