# Sub Synth r1 — isolated local test build

Completed 2026-09-17. Branch: `local/v1.2.1-sub-synth-r1`.
Base: `27d3435bd54faa4930a694c1d28136e0fc049161` (folder-page cache r1,
including artwork cache and existing BLE fixes). Previous builds retained.

## Reference and scope

Compared DSPi v1.1.6-beta3 (`6f88ae1cc7248aab192203bd52df4502ef3c33ed`)
and Mac Console release/v1.1.6 (`b48f2d2`). This is ESP control of the DSPi
effect over the existing UART protocol, not ESP audio processing. No spectrum
analyser, decoder, S/PDIF output, SPI timing, task-priority or BLE changes.

Sub Synth list menu provides Enable; three band levels (24–36, 36–56,
56–80 Hz); Selectivity with All material/Percussive/Sustained and conditional
Depth/Hold; Ceiling; LF Boost; Outputs; Link Pairs; and read-only Headroom.
Only matrix-enabled outputs are exposed. Mask updates reread the current mask
and preserve other bits. Pair linking defaults on in the policy state, but
actual DSPi/Console/preset values are read and preserved, never forced on.
The screen uses `pct` because the embedded fonts do not contain a percent sign.

Added a waveform/minus status symbol, appended D-pad shortcut without changing
existing stored action IDs, and normal Home on/off notifications for local
shortcuts and confirmed external changes. Boot synchronisation uses the existing
notification baseline. Preset persistence remains DSPi's native save path.
The extended link-setting command is probed; unsupported DSPi firmware is not
treated as supporting the complete beta3 control set.

This compact menu does not reproduce the Console graph, built-in starting-point
presets or temporary Solo audition control. Headroom is refreshed on entry,
local apply or selecting its row, not continuously: external changes can leave
that one displayed estimate stale until refreshed. No automatic gain correction.

## Changed files

- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`
- `firmware/DSPi_ESP32_Front_Panel_v1_1_2/SubSynth.h`
- `local-validation/Test-SubSynth.cpp`
- `local-validation/Test-DSPi-v1.2.1-Sub-Synth.py`
- This build record.

## Verification

- 129 Python regression/integration contracts passed.
- Native Sub Synth policy test passed: defaults, parameter mapping, valid ranges,
  invalid numbers, output filtering/mask preservation and command widths.
- Native artwork-cache and directory-page-cache regression tests passed.
- 18 web-portal assertions passed and portal JavaScript parsed.
- Independent static review found no blocking issue; noted Headroom freshness
  limitation above. Final font-coverage and capability checks added afterwards.
- `git diff --check` passed.
- Final Arduino compile succeeded: 1,924,477 program bytes (61% of app slot),
  78,156 static global bytes (23%); 249,524 bytes remaining before dynamic use.

ESP32 core 3.3.11; ESP32-S3 hwcdc/cdc, 240 MHz, QIO, 16 MB flash,
app3M_fat9M_16MB partitions, OPI PSRAM. Extra compiler flags:
`-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING`.

## Artifacts

Folder: `local-validation/builds/v1.2.1-sub-synth-20260916-r1`.
The folder date reflects when this stage started; final build finished Sept 17.

| File | Bytes | SHA-256 |
|---|---:|---|
| DSPi-ESP32-Front-Panel-v1.2.1-Sub-Synth-r1-OTA.bin | 1,924,624 | AED3D972B1F78FDD36342E95225C66B0ECF3F9AF08CDDB9648F88AA523D5BF72 |
| DSPi-ESP32-Front-Panel-v1.2.1-Sub-Synth-r1-Full.bin | 16,777,216 | 0E815EB12B9A7DC1E47414F22BDC9C4ED33C50FE9D6E6B284343997F0FE57C0A |

Use only the OTA application on Wi-Fi Transfer/Update. The full image is for
USB and can overwrite settings. Neither image has been flashed or published.
No public release files were replaced.

## Hardware acceptance still required

Use compatible DSPi beta3 firmware. Check each setting against Console, Link
Pairs both ways, hidden/disabled outputs, preset save/reload, D-pad shortcut,
Home icon and notifications (including no boot false notice), edit cancel/back,
and 44.1/48 kHz music while navigating. Host tests cannot establish actual UART
or hardware/display behaviour. Roll back using the retained folder-page-cache
r1 OTA if necessary; no ESP settings schema was changed.
