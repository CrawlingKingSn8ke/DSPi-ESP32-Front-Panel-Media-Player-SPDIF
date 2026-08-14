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
- DMA reserve is approximately 64 ms at 48 kHz and 70 ms at 44.1 kHz.
- The output task and I2S interrupt have elevated, deterministic priorities.
- GPIO13 is explicitly driven low after playback stops.

## Observed hardware behaviour

Two DSPi systems have behaved differently:

1. A system with four PCM5102 DAC boards is stable with this S/PDIF music-player firmware, with no observed dropout or distortion.
2. A second system uses a TV S/PDIF input, a DSPi S/PDIF output feeding an external DAC, and a PCM5102 on a DSPi I2S output. USB input and TV S/PDIF input are stable through the DSPi S/PDIF output. With the ESP as the S/PDIF source, the external DAC on the DSPi S/PDIF output can drop out while the simultaneous PCM5102/I2S output remains stable. Moving the ESP source between DSPi S/PDIF GPIO4 and GPIO5 did not remove the behaviour. DMA/carrier hardening reduced but did not eliminate it.

This evidence suggests a source-dependent interaction affecting the DSPi S/PDIF output path rather than a general failure of the ESP decoder, SD pipeline, DSP processing, or one DSPi receiver GPIO. The exact cause is not yet proven.

## Verified OTA application

- File: `DSPi-ESP32-Front-Panel-v1.2.1.bin`
- Size: 1,909,936 bytes
- SHA-256: `CA693E387D8F08C0C9C3D037AE6FC231F3D8A8D7B239A626156A4B3445EDD110`
- Local source commit: `903de33`
- Focused contracts: 34 passed
- ESP32 Arduino core: 3.3.11

Use the application-only file for browser OTA. Do not upload the 16 MB full-flash image through the browser updater.
