# DSPi ESP32 S/PDIF forensic audit — 2026-08-17

## Scope and baseline

- Repository: `CrawlingKingSn8ke/DSPi-ESP32-Media-Player-SPDIF`
- Reviewed baseline: `24188d5c54b2a590a2f5916f2a49bf8889d03b0a`
- Preserved designs: `a45f8d6` DMA/framing hardening and `24188d5`
  continuous same-rate carrier
- Arduino-ESP32: 3.3.11
- ESP-IDF: v5.5.5, commit `b774170ff46`
- Target: ESP32-S3, 16 MB flash, OPI PSRAM

The review also compared official WeebLabs DSPi `main` at
`ec26bf0baa54e74b9bb2578d8e7d62a09d4da01a` with experimental
`spdif_rx_overhaul` at `7c3c7bf034e73425538e0169d352993536f3dbdb`.

## Findings

### Confirmed correct and unchanged

- 24-bit consumer PCM encoder and B/M/W preamble cadence
- 192-frame block wrap, C-bit mapping, V/U values and even parity
- identical consumer status on left and right
- 44.1 kHz status `04 00 00 00 0B`
- 48 kHz status `04 00 00 02 0B`
- BMC phase continuity and split-call equivalence
- sixteen DMA descriptors, each exactly 3,072 bytes / one S/PDIF block
- exact 49,152-byte preload before transmitter enable
- valid encoded silence with DMA auto-clear disabled
- failure deletion while the IDF channel is REGISTERED/READY
- data-only GPIO13 output and explicit LOW on genuine shutdown
- same-rate channel reuse and controlled 44.1/48 kHz recreation

Exact IDF 5.5.5 `i2s_channel_preload_data()` semantics were checked. The first
call establishes descriptor zero and queues the remaining descriptors; sixteen
exact 3,072-byte calls fill all sixteen descriptors. Every application call
checks its returned byte count and the total before enabling TX.

### Actual ESP defect found and corrected

The encoder advanced for an entire 256-frame staging chunk before the IDF write.
If a retaining stop arrived after a partial timeout write, the old write loop
could abandon the unwritten suffix. The encoder phase then described more data
than DMA had received, so transition block-alignment arithmetic could join
silence at the wrong IEC/BMC phase.

The corrected path finishes the already-encoded buffer while retention remains
Requested, including exact partial-write continuation. An ordinary STOP clears
the token and may abort because TX will be deleted.

### Lifecycle defects found and corrected

- A boolean request was previously treated as proof that a clean retained ring
  existed. Retention now has atomic `Off`, `Requested`, `Ready` and `Faulted`
  states. A fault or an already-started ordinary stop cannot be resurrected by
  a later track-transition request.
- Only the output task can publish `Requested -> Ready`, after it completes the
  in-flight buffer, current IEC block and one complete 49,152-byte ring.
- Encode/DMA errors invalidate retention, request task shutdown and cannot leave
  an unproven carrier reusable.
- Repeated retention timeouts can now be cancelled by an overlapping ordinary
  STOP instead of retrying indefinitely.
- Genuine stop, park, direct-start and input-change paths are idempotent even in
  the brief `Stopped + retained carrier` transition window.
- Once the output task exits, genuine STOP disables TX immediately even if a
  slow decoder/SD read is still unwinding; shared decoder/ring cleanup waits.
- Task completion handles are release-published and acquire-read before main-loop
  cleanup. Stop/retention signals use atomic acquire/release operations.
- Decoder stop/seek notification through a possibly self-deleting task handle
  was removed; the existing bounded 2–20 ms queue polling remains.

### Lightweight diagnostics

RAM-only lifetime counters now expose:

- S/PDIF write timeouts
- partial writes
- write/encode errors
- successful live-carrier replacements, excluding cold starts
- successfully prepared retained transitions

No logging or Wi-Fi work was added to the time-critical write loop.

## Clock audit

ESP32-S3 does not select APLL in this Arduino/IDF version. It uses the 160 MHz
PLL fractional clock path. Both required rates are exactly representable:

| PCM source | I2S configuration | serialized S/PDIF | 160 MHz divider |
|---|---:|---:|---:|
| 44.1 kHz | 88.2 kHz | 5.6448 MHz | `7 + 38/441` |
| 48 kHz | 96 kHz | 6.144 MHz | `6 + 49/96` |

Two 32-bit stereo I2S frames carry the four 32-bit BMC words for one original
stereo S/PDIF frame, hence `2 × source rate × 64 = 128 × source rate`.

On ESP32-S3 the IDF 5.5.5 I2S path uses GDMA. The public driver does not apply a
fixed level-2 request; it allocates an IRAM-safe LOWMED GDMA interrupt. The
priority request remains for compatible non-GDMA targets, and no private-IDF
patch was introduced.

## DSPi evidence boundary

The historical recurring downstream S/PDIF dropout is not attributed to this
ESP transition fix. ESP hardening improved but did not eliminate it, and a
tested 68-ohm series resistor did not cure it. The dropout disappeared when
current DSPi main was tested with `spdif_rx_overhaul`.

The strongest evidence is the overhaul's external-clock measurement and soft
VCXO/output-servo redesign, including its documented external DAC PLL-unlock
behavior under old clock slews. This does not prove one individual DSPi source
line was the sole root cause.

## Validation

- Focused/static/behavioral tests: 85 passed
- New behavioral reference-model decoder, guarded against the production
  constants and layout: complete blocks at both rates, audio/V/U/C/P, parity,
  BMC continuity, wrap and split-call equivalence
- New transition fault injection: timeout-with-prefix and success-with-prefix
  continuation, transition stop, stop-before-write, full-ring alignment,
  overlapping cancellation, fault non-resurrection, hard error, zero progress,
  and the bounded UI transition-timeout escape
- Arduino compile: passed
- Program storage: 1,912,481 bytes (60% of 3,145,728)
- Static RAM: 77,364 bytes (23% of 327,680)

## Non-overwriting artifacts

Directory: `local-validation/builds/v1.2.1-spdif-forensic-20260817-r2`

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-Forensic-r2-OTA.bin` | 1,912,624 | `8C4DE6295B6299080621401FD8FEB289A114D3DD129D6BF4C5D9A4BA7FEE22D6` |
| `DSPi-ESP32-Front-Panel-v1.2.1-SPDIF-Forensic-r2-Full.bin` | 16,777,216 | `A04E5C920D6FF32D6B98039E6E5060BD9B6B4B18BE98C3E6F2E41BE10D6162B6` |

The earlier forensic `r1` directory remains intact and was not overwritten. The
`20260816-r2` continuous-carrier artifacts were also re-hashed after this build:

- OTA: `1A77C60E10644145CEC09822BD6A337D2EE6BF5E51A65B626843E262D6132B44`
- Full: `192A218CB26F89D930C7D2AEFF3F1E42B12C6C4B2AE7CB1FAFAC0DEE51350822`

No firmware was flashed or pushed.
