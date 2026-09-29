# Feature Menu and Remote Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Features, System and Remote consistent scrolling lists, correct Tube units, and fit all active-feature icons on Home without changing the music player.

**Architecture:** Add a small, pure layout policy for paged lists and icon widths. Adapt the existing sketch's navigation and renderers around those policies while keeping DSPi/BLE command and persistence owners intact. Build on `f0141eb` in a new isolated local branch and give the test images unique names.

**Tech Stack:** Arduino ESP32-S3 core 3.3.11, C++ sketch, NimBLE-Arduino 2.5.0, Python `unittest` contracts, PowerShell build verification.

**Spec:** `docs/superpowers/specs/2026-09-29-feature-menu-remote-layout-design.md`

## Global Constraints

- Do not modify `MediaPlayerPoC.*`, `MediaFs.*`, the Music browser/now-playing renderers, S/PDIF code, audio task priorities, or shared-SPI scheduling.
- Preserve existing BLE bond, map, theme, preset and D-pad settings; append new action/page IDs without renumbering saved action values.
- Keep the existing DSPi protocol support floor; no Tube/Limiter write if unsupported, and no boot-time or unchanged-poll feature overlay.
- Preserve semantic main/accent/dim colours and fixed warning/fault/clip roles.
- Build only; do not flash, OTA-upload, push or replace prior build/release files.
- Use `apply_patch` for edits, preserve unrelated work, and commit each testable task.

## Review Focus

- A list with exactly four entries must have no arrow; a five-entry list must show it only until the final window (Task 1 test).
- A preset toast or other footer message must temporarily hide the arrow, without changing list selection (Task 1 test).
- A saved BLE map or D-pad action from the old build must survive the new menus unchanged (Task 4 test).
- A sleeping paired remote must say Reconnecting with its saved name, not Connected or Not paired (Task 4 test).
- A failed Tube/limiter status read must not turn on an unconfirmed icon or generate a feature overlay; a same-generation last-confirmed state may be retained (Task 5 test).

## File map

- Create `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiMenuLayout.h`: pure four-row visible-window and below-list policy.
- Modify `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h`: Tube/Limiter icon masks, widths and total-span calculation.
- Modify `firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino`: navigation, existing renderers, BLE UI presentation, feature observation and shortcut dispatch. Do not edit its Music functions.
- Create `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`: focused source/layout contracts, added task by task.
- Create `local-validation/DSPi-v1.2.1-Feature-Menu-Layout-r1-Build-Record.md`: verification, file list, sizes and hashes.
- Preserve `docs/superpowers/specs/2026-09-29-feature-menu-remote-layout-design.md` and this plan as stage documentation, not product code.

---

### Task 1: Four-row window and unobtrusive down arrow

**Files:** Create `firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiMenuLayout.h`; modify the sketch's `drawSystemSettingsList()` and `drawPresetList()`; test `local-validation/Test-DSPi-v1.2.1-Feature-Menu-Layout.py`.

**Interfaces:** `struct UiPagedWindow { uint8_t first, end; bool below; }; constexpr UiPagedWindow uiPagedWindow(uint8_t count, uint8_t selected, uint8_t visibleRows, uint8_t currentFirst);` uses `[first,end)` and clamps selected into view. `void drawListDownArrow(bool below, bool toastVisible);` draws a small accent chevron at the lower-right only when `below && !toastVisible`.

- [ ] Write failing layout contracts for counts 0/4/5/10, first/middle/last selection, and toast suppression; assert Music renderers contain no new arrow/layout calls.
- [ ] Run `py -3 -m unittest discover -s local-validation -p 'Test-DSPi-v1.2.1-Feature-Menu-Layout.py' -v`; expect FAIL.
- [ ] Implement the policy and arrow, then use it for four-row settings pages and the Preset list. Keep the Preset save hint and toast clear of the arrow; do not touch Music.
- [ ] Re-run the focused command; expect PASS.
- [ ] Commit only this helper, its sketch changes and tests as `Add paged-list continuation indicator`.

### Task 2: Five-category navigation and uniform feature lists

**Files:** Modify the sketch's `MenuPage`, `rememberedMenuIndex`, `pageTitle`, `menuItemCount`, `menuItemName`, `mainIndexForPage`, `pageForMainIndex`, `selectMenuItem`, `goBack`, and `isSystemSettingsListPage`; extend the same focused test.

**Interfaces:** Append `PAGE_FEATURES` to `MenuPage` and enlarge `rememberedMenuIndex` accordingly. `PAGE_MAIN` order is exactly Input, Music, Preset, Features, System; `PAGE_FEATURES` has the seven settings pages in spec order. `PAGE_SYSTEM` gains Remote as its fifth entry; `mainIndexForPage(PAGE_BLUETOOTH)` resolves to System. Every feature parameter page goes through the four-row `FontMedium` list renderer; Back returns to Features, while its nested pages return to their feature parent. Existing edit/apply functions and DSPi writes are not altered.

- [ ] Write failing tests for exact menu order/count, System-last mapping, feature parent/Back routing, and `FontMedium` list rendering.
- [ ] Run the focused command; expect FAIL.
- [ ] Implement navigation and page routing, retaining remembered positions and all existing feature-setting parameter counts. Ensure Status, Screen Settings, Volume Limit and Wi-Fi still select their original actions.
- [ ] Re-run focused tests; expect PASS.
- [ ] Commit this navigation unit as `Group DSPi features under Features menu`.

### Task 3: Tube numbers and units

**Files:** Modify the sketch's `tubeValueText`, `menuValueUnit`, `menuValueUsesPercent`, `drawMenuValue`, and list-value rendering; extend the focused test.

**Interfaces:** `tubeValueText(TUBE_DRIVE, x)` and `tubeValueText(TUBE_MIX, x)` provide numeric text for editing; list rows draw `dB`/`%` as a measured compact suffix. `menuValueUnit()` returns `dB` for Tube Drive and `%` for Tube Mix. Do not change the values sent to DSPi.

- [ ] Write failing tests that Drive remains `FontLarge` at `-9.5` and `-10.0`, `%` replaces `pct`, and `dB`/`%` are small, same-sized suffixes in edit and list views.
- [ ] Run the focused command; expect FAIL.
- [ ] Implement unit-aware presentation using the existing numeric/unit draw path, measuring number plus suffix before centring; retain the old floating-point quantisation and write path.
- [ ] Re-run focused tests; expect PASS.
- [ ] Commit this presentation unit as `Align Tube numeric units with feature editor`.

### Task 4: Remote and D-pad list workflow

**Files:** Modify the sketch's `bleMenuActionForIndex`, `bleMenuItemName/Value`, `drawBleScreen`, `bleNavigate`, `bleBack`, `bleSelect`, and BLE learning UI text; extend the focused test.

**Interfaces:** Remote stays `PAGE_BLUETOOTH` beneath System. Keep `BLE_MAPPING_COUNT == 21`, `remoteMap[]`, profile removal and `saveHomeShortcuts()` ownership unchanged. `BLE_UI_MAPPING`, `BLE_UI_SHORTCUTS` and `BLE_UI_SHORTCUT_EDIT` render four-row lists through `uiPagedWindow`; unmapped rows display dim `None`. Preserve the existing Restore Defaults action. A select-release guard precedes `Press button`; a captured press shows `Release button`. Remove confirmation displays the saved device name with Cancel as default.

- [ ] Write failing tests for connected/reconnecting/unpaired labels, 21 rows, mapped accent versus `None` dim, long-name clipping, Back/cancel preserving a map/shortcut, Restore Defaults retention, and explicit named removal.
- [ ] Run the focused command; expect FAIL.
- [ ] Implement only BLE presentation/navigation transitions; retain scan/reconnect, NimBLE callbacks, saved profiles and key ownership. Reuse existing timeout/disconnect handling to leave learning without replacing a mapping.
- [ ] Re-run focused tests; expect PASS.
- [ ] Commit this Remote UI unit as `Present Remote mappings as readable lists`.

### Task 5: Home icons, Tube shortcut and confirmed-state feedback

**Files:** Modify `UiReadability.h` and the sketch's `drawTopStatus`, icon drawers, `UiAction`, shortcut arrays, `performAction`, and `pollExternalRuntimeState`; extend the focused test and add compile-time icon-span assertions in `UiReadability.h`.

**Interfaces:** Append `ACT_TUBE_TOGGLE` after `ACT_SUB_SYNTH_TOGGLE`; append `Toggle Tube` to the 14 existing shortcut choices. Add `UI_FEATURE_TUBE=1<<5`, `UI_FEATURE_LIMITER=1<<6`, with widths 15 and 14 and five-pixel gaps; all seven total 138 px. Place source/preset on row one and centre icons on row two without moving the Home divider or meters. Use DSPi `REQ_LIMITER` index `0x81` status byte 0 (`engaged`) for the Limiter icon, not per-output meter activity; the [upstream limiter protocol](https://github.com/WeebLabs/DSPi/blob/557bce7/Documentation/Features/output_limiter_spec.md) defines this four-byte status. Tube uses `TUBE_ENABLE` readback and existing full-screen notification path. Retain the current 1.2-second runtime watcher; do not add work to the audio task.

- [ ] Write failing tests for 138-px all-icon packing, existing saved action values, Tube success/error paths, source/preset separation, boot/preset-change notification suppression, and failed-read icon behaviour.
- [ ] Run focused tests; expect FAIL.
- [ ] Draw Tube and Limiter symbols with semantic colours, update confirmed states independently, and add Tube local/Console notifications. Probe unsupported firmware safely; only confirmed Limiter engaged=1 lights its symbol. Keep preset/source overlays higher priority.
- [ ] Re-run focused tests; expect PASS.
- [ ] Commit this Home/feedback unit as `Fit all feature icons and notify Tube changes`.

### Task 6: Whole-stage verification and uniquely named build

**Files:** Create `local-validation/DSPi-v1.2.1-Feature-Menu-Layout-r1-Build-Record.md`; create only uniquely named binaries under `local-validation/builds/feature-menu-layout-20260929-r1/`.

**Interfaces:** No product-code change. The previous Tube/Limiter OTA and all existing release files remain untouched.

- [ ] Run `py -3 -m unittest discover -s local-validation -p 'Test-*.py' -v` and `git diff --check`; expect all tests PASS and no whitespace errors. Compare `git diff --name-only 4a4be60...HEAD` against the file map and fail if music/audio files or Music renderer bodies changed.
- [ ] Compile with `arduino-cli compile --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=921600,DebugLevel=none,EraseFlash=none' --build-property 'compiler.cpp.extra_flags=-DSDFAT_FILE_TYPE=3 -DUSE_UTF8_LONG_NAMES=1 -DDISABLE_FS_H_WARNING' --export-binaries --build-path 'local-validation/builds/feature-menu-layout-20260929-r1/work' --output-dir 'local-validation/builds/feature-menu-layout-20260929-r1' 'firmware/DSPi_ESP32_Front_Panel_v1_1_2'`; expect successful ESP32-S3 compile and application below the 3,145,728-byte partition ceiling.
- [ ] Copy `DSPi_ESP32_Front_Panel_v1_1_2.ino.bin` and `.ino.merged.bin` from that new build folder to distinct `DSPi-ESP32-Front-Panel-v1.2.1-Feature-Menu-Layout-r1-OTA.bin` and `DSPi-ESP32-Front-Panel-v1.2.1-Feature-Menu-Layout-r1-Full.bin` names in the same unique folder; verify each length and SHA-256 with `Get-Item` and `Get-FileHash`. Record exact changed files, tests, sizes, hashes and hardware checks not yet performed; commit only the build record (not generated binaries). Do not flash or push.
