# Artwork cache r1 — local test, 2026-09-15

Baseline: `5de929a`, branch `local/v1.2.1-artwork-cache`.

## Scope

One worker-owned cache retains exact JPEG bytes and decoded RGB565 pixels.
On an exact byte match it supplies an independently allocated pixel copy and
skips JPEG decoding. Changed embedded covers cannot match merely by folder.
Cache contents remain safe across file replacement because each request reads
and compares the current JPEG bytes. No negative caching is performed.

The cache accepts JPEGs up to 256 KiB and decoded pixels up to 128 KiB;
maximum retained cache allocation is 384 KiB. Cache allocations use only PSRAM
and check for a 512 KiB reserve. Allocation failure uses normal decoding.
The result buffer is separate from the cache and follows existing queue/UI
ownership. Generation checks reject stale results after track changes.

SD reads, the 1500 ms artwork delay, audio-buffer thresholds, screen transfers,
task priorities, BLE, S/PDIF and OTA behaviour are unchanged. This reduces
repeated decoding, not first-cover latency or SD traffic. Physical playback
and latency verification remains for the user.

## Verification

- Native C++ test exercises the production cache: misses, exact byte identity,
  different contents/length, independent buffers, allocation failures,
  replacement, invalid dimensions, oversized input and 1000 replacement cycles
  with no outstanding allocation after destruction.
- Existing 123 Python contracts and 18 portal assertions passed.
- ESP32 Arduino build passed: program 1,917,765 bytes (60%); globals 78,060 (23%).
- No flashing or GitHub publication performed; earlier outputs retained.

Changed files: sketch `.ino`, new `ArtworkCache.h`,
`local-validation/Test-ArtworkCache.cpp`, this record.

Native test can be compiled with a C++11 host compiler and run directly:
`c++ -std=c++11 local-validation/Test-ArtworkCache.cpp -o Test-ArtworkCache`

## Artifacts

Folder: `local-validation/builds/v1.2.1-artwork-cache-20260915-r1`

| File | Bytes | SHA-256 |
|---|---:|---|
| DSPi-ESP32-Front-Panel-v1.2.1-Artwork-Cache-r1-OTA.bin | 1,917,920 | 413E44A9086602ABE63BC2070EAC4F1A556167B21A7BB1A64473FE9C881699B0 |
| DSPi-ESP32-Front-Panel-v1.2.1-Artwork-Cache-r1-Full.bin | 16,777,216 | AE4773F3B2F4C3A859FD3DAC59AA8F5747C72227B1E334FF1C4558D525DCB719 |

Test several tracks sharing artwork, tracks with different embedded covers,
rapid skips, return to Now Playing and normal 44.1/48 kHz playback. A cache hit
prints `MEDIA ART: exact JPEG cache hit; decode skipped` once per loaded cover.
