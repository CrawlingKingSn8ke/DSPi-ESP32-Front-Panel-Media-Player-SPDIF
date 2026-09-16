# Folder-page cache r1 — 16 September 2026

Base source: `f30259d`, artwork-cache r1.
Separate local branch: `local/v1.2.1-folder-page-cache-r1`.

Verified base OTA SHA-256:
`413E44A9086602ABE63BC2070EAC4F1A556167B21A7BB1A64473FE9C881699B0`.

## Change

Optional main/browser-owned cache retains three exact directory-page requests,
each with up to 96 entries and original pagination metadata. Maximum storage
is compile-time bounded to 320 KiB; allocation is PSRAM-only with a 512 KiB
free-space admission reserve. Allocation failure falls back to normal scanning.
Cache results copy into caller-owned buffers. Least-recently-used queries are
replaced. Empty listings are not cached.

Keys include exact directory path, requested capacity, page mode and anchor
fields. This preserves the existing ordering and pagination. It does not cache
an entire unlimited directory or synthesize overlapping pages. A first visit
or a different anchor/mode still scans; repeated matching queries benefit.

Mount/reuse, unmount and storage-ownership changes clear the cache. Browsing
after an observed decoder/card fault also clears it. Wi-Fi transfer mode cannot
reuse normal-browser entries; transitions invalidate before and after writes.
Hits perform a small mounted-card probe before returning, not a full directory
scan. This does not promise detection of every electrically unobserved hot swap.

PSRAM allocation/copy does not hold the shared-SPI mutex. No decoder/output
task code, S/PDIF encoder, task priorities, artwork, display, BLE or OTA code
was changed. Existing artwork cache is retained.

## Verification

- New native C++ production-cache test first failed before implementation,
  then passed: exact-query misses/hits, copy isolation, metadata, all modes,
  anchor differences, 96-entry boundary, invalid/oversized input, LRU eviction,
  low-memory fallback preserving existing entries, clear and 1,000 cycles.
- 123 existing Python tests passed.
- 18 portal assertions passed; portal JavaScript parses.
- Native artwork-cache regression test passed.
- Independent read-only review found no blocking issue. Minor limitation:
  existing filesystem iteration can conflate EOF and malformed directory
  metadata; transport errors prevent caching, but not every filesystem-level
  early termination is distinguishable. No claim of complete SD-error coverage.

Changed firmware: `MediaPlayerPoC.cpp`, new `DirectoryPageCache.h`.
Added test: `local-validation/Test-DirectoryPageCache.cpp`.
No flashing, GitHub publication, previous-artifact replacement or release update.

## Hardware acceptance

Test repeat folder/page navigation, different sort positions, large folders,
playing-track/folder highlights, card removal/remount, Wi-Fi upload/delete then
return to browser, and 44.1/48 kHz playback while scrolling. Initial directory
scans remain unchanged. Timing improvement and audio stability need hardware
confirmation; host tests/build alone cannot establish them.

## Build and recovery artifacts

ESP32 core 3.3.11; same ESP32-S3/16 MB/OPI PSRAM board profile and compiler
flags as the artwork-cache build. Compilation succeeded: program 1,918,925
bytes; static globals 78,084 bytes. Optional dynamic PSRAM cache is additional.

Separate folder: `local-validation/builds/v1.2.1-folder-page-cache-20260916-r1`.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| DSPi-ESP32-Front-Panel-v1.2.1-Folder-Page-Cache-r1-OTA.bin | 1,919,072 | AE5EC921739C4B83D9571B4E88DF706A2574560E0B17E580E19FEED5E156BEBF |
| DSPi-ESP32-Front-Panel-v1.2.1-Folder-Page-Cache-r1-Full.bin | 16,777,216 | 48A2F8C6721BA2AAA42D47A1DCA73A03434F328D25D5C05C07FFF6E207284345 |

Use only the OTA file on the Wi-Fi update page. Previous artwork-cache build
folder remains untouched, and its OTA hash was reverified after this build.
No preferences schema changes; the cache is volatile and starts empty after
reset. Roll back using the retained artwork-cache OTA if needed.
