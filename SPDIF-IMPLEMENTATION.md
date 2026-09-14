# S/PDIF media-player implementation

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
- The output task remains elevated and pinned to CPU1. On ESP32-S3, IDF 5.5.5
  uses an IRAM-safe GDMA callback selected from the driver's low/medium
  interrupt pool; the public driver does not provide an exact level-2 override.
- Same-rate track changes retain the live transmitter and fill the complete DMA
  ring with block-aligned encoded silence, avoiding receiver reacquisition.
- Retention uses atomic Off/Requested/Ready/Faulted ownership. A transition is
  reusable only after the in-flight encoded buffer, current IEC block and
  complete DMA ring have all been finished successfully. Genuine stop and error
  paths cancel it and cannot be resurrected by a later transition request.
- A 44.1/48 kHz change still performs a controlled transmitter restart.
- GPIO13 is explicitly driven low after playback stops.
- RAM-only lifetime counters record write timeouts, partial writes, write/encode
  errors, successful live-carrier replacements (not cold starts), and successful
  retained transitions without loop-time logging.

## ESP32-S3 clock and framing

The I2S peripheral serializes four 32-bit BMC words for each original stereo
S/PDIF frame. A 32-bit stereo I2S frame carries two of those words, so the
configured I2S rate is twice the source rate:

| Source PCM | Configured I2S | S/PDIF line rate |
|---|---:|---:|
| 44.1 kHz | 88.2 kHz | 5.6448 MHz |
| 48 kHz | 96 kHz | 6.144 MHz |

Arduino-ESP32 3.3.11 uses ESP-IDF 5.5.5. On ESP32-S3 this path uses the 160 MHz
PLL with its fractional divider, not APLL. Both required clocks are exactly
representable (divider denominators 441 and 96 respectively). The ESP remains a
free-running source; it does not dither, resample or servo its clock to DSPi.

## Observed hardware behaviour

Two DSPi systems have behaved differently:

1. A system with four PCM5102 DAC boards is stable with this S/PDIF music-player firmware, with no observed dropout or distortion.
2. A second system uses a TV S/PDIF input, a DSPi S/PDIF output feeding an
   external DAC, and a PCM5102 on a DSPi I2S output. The ESP source originally
   exposed recurring downstream S/PDIF dropouts while the DSPi I2S output
   remained stable.

The ESP hardening, standards-correct channel status and true full-ring preload
substantially improved behaviour but did not eliminate that recurring dropout.
A physical 68-ohm GPIO source resistor also did not cure it. Testing current
WeebLabs main combined with the DSPi `spdif_rx_overhaul` development work removed
the recurring dropout without further ESP clock compensation.

The strongest current evidence therefore points to the older DSPi
external-clock tracking/output-servo behaviour. In old main, the servo changes
live S/PDIF TX PIO dividers; the overhaul instead combines long-window input
measurement, fractional consumer-fill measurement, smoothing/PI control, slew
limiting and a soft VCXO that trims the common system PLL in small increments.
The overhaul documentation records external DAC PLL unlocks caused by the old
clock slews. This is strong causal evidence, but it does not prove that one
individual DSPi source line was the sole cause.

The forensic ESP review did find a separate rare transition correctness issue:
a retained stop arriving after a partial DMA write could previously advance the
encoder phase beyond the bytes actually queued. That lifecycle race is now
guarded independently. It must not be described as the fix for the historical
recurring DSPi dropout.

Both the ESP and an independent TV S/PDIF transmitter can produce one small click roughly ten seconds after the first fresh receiver lock. It does not repeat while the receiver remains locked. That shared behaviour points to receiver clock settling rather than the ESP encoder or SD/audio pipeline.

Keep the internal 3.3 V logic connection and its ground return short. A series
resistor is not a mandatory requirement for this tested wiring.

## Current verified v1.2.1 build

- OTA: `DSPi-ESP32-Front-Panel-v1.2.1-OTA.bin`
- OTA size: 1,917,392 bytes
- OTA SHA-256: `4F8595B6C152CBC96C06B34ECADDFDAC1369B392D8BD864501A92251BFD84AB3`
- Full USB: `DSPi-ESP32-Front-Panel-v1.2.1-Full.bin`
- Full USB size: 16,777,216 bytes
- Full USB SHA-256: `AB31406C065863E3C49EE0E125748A6BFA4789D3056CC3E53FC6A902C85F5EF3`
- Firmware contracts: 123 passed
- Wi-Fi portal assertions: 18 passed
- ESP32 Arduino core: 3.3.11

Use the application-only file for browser OTA. Do not upload the 16 MB full-flash image through the browser updater.
