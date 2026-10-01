import os
import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
SKETCH = FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino"


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    while ";" in source[start:opening]:
        start = source.index(signature, start + len(signature))
        opening = source.index("{", start)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


def compile_assertions(source):
    compiler = (pathlib.Path(os.environ["LOCALAPPDATA"]) / "Arduino15" /
                "packages" / "esp32" / "tools" / "esp-x32" / "2601" /
                "bin" / "xtensa-esp32s3-elf-g++.exe")
    result = subprocess.run(
        [str(compiler), "-std=c++17", "-fsyntax-only", "-x", "c++",
         "-I", str(FIRMWARE), "-"],
        input=source, text=True, capture_output=True, check=False)
    return result


class FeatureMenuLayoutTests(unittest.TestCase):
    def test_paged_window_and_toast_policy(self):
        source = """
#include "UiMenuLayout.h"
static_assert(uiPagedWindow(0, 0, 4, 0).end == 0);
static_assert(!uiPagedWindow(0, 0, 4, 0).below);
static_assert(uiPagedWindow(4, 3, 4, 0).first == 0);
static_assert(!uiPagedWindow(4, 3, 4, 0).below);
static_assert(uiPagedWindow(5, 0, 4, 0).end == 4);
static_assert(uiPagedWindow(5, 0, 4, 0).below);
static_assert(uiPagedWindow(5, 4, 4, 0).first == 1);
static_assert(!uiPagedWindow(5, 4, 4, 0).below);
static_assert(uiPagedWindow(10, 5, 4, 0).first == 2);
static_assert(uiPagedWindow(10, 5, 4, 0).end == 6);
static_assert(uiPagedWindow(10, 5, 4, 0).below);
static_assert(uiPagedWindow(10, 9, 4, 2).first == 6);
static_assert(!uiPagedWindow(10, 9, 4, 2).below);
static_assert(uiShowDownArrow(true, false));
static_assert(!uiShowDownArrow(true, true));
static_assert(!uiShowDownArrow(false, false));
static_assert(uiShowUpArrow(false, 1, false));
static_assert(!uiShowUpArrow(true, 1, false));
static_assert(!uiShowUpArrow(false, 0, false));
static_assert(!uiShowUpArrow(false, 1, true));
"""
        result = compile_assertions(source)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_preset_and_settings_use_continuation_indicator(self):
        sketch = SKETCH.read_text()
        for signature in ("void drawPresetList()", "void drawSystemSettingsList()"):
            body = function_body(sketch, signature)
            self.assertIn("uiPagedWindow(", body, signature)
            self.assertIn("drawListDownArrow(", body, signature)

    def test_music_renderers_remain_unchanged_from_base(self):
        previous = subprocess.run(
            ["git", "show", "8c6664a:" +
             "firmware/DSPi_ESP32_Front_Panel_v1_1_2/"
             "DSPi_ESP32_Front_Panel_v1_1_2.ino"],
            cwd=ROOT, text=True, capture_output=True, check=True).stdout
        current = SKETCH.read_text()
        for signature in ("void renderMediaBrowser(bool fullFrame)",
                          "void drawMediaNowPlaying()",
                          "void serviceMediaBrowserRedraw()"):
            self.assertEqual(function_body(current, signature),
                             function_body(previous, signature), signature)

    def test_top_level_and_feature_order(self):
        sketch = SKETCH.read_text()
        counts = function_body(sketch, "uint8_t menuItemCount(MenuPage page)")
        self.assertIn("case PAGE_MAIN: return 5;", counts)
        self.assertIn("case PAGE_FEATURES: return 7;", counts)
        self.assertIn("case PAGE_SYSTEM: return 5;", counts)
        names = function_body(sketch, "String menuItemName(MenuPage page, uint8_t index)")
        self.assertIn('"Input", "Music", "Preset", "Setup", "System"', names)
        self.assertIn('"Loudness", "Crossfeed", "Leveller", "Psy Bass",', names)
        self.assertIn('"Sub Synth", "Tube Modeller", "Output Limiter"', names)
        self.assertIn('"Remote", "Volume Limit", "Screen Settings",', names)
        self.assertIn('"WiFi Transfer/Update", "Status"', names)
        pages = function_body(sketch, "MenuPage pageForMainIndex(uint8_t index)")
        self.assertIn("PAGE_INPUT, PAGE_MEDIA, PAGE_PRESET, PAGE_FEATURES, PAGE_SYSTEM", pages)
        main_index = function_body(sketch, "int8_t mainIndexForPage(MenuPage page)")
        self.assertIn("case PAGE_BLUETOOTH: return 4;", main_index)

    def test_feature_settings_use_medium_list_and_back_to_features(self):
        sketch = SKETCH.read_text()
        renderer = function_body(sketch, "bool isSystemSettingsListPage(MenuPage page)")
        for page in ("PAGE_FEATURES", "PAGE_LOUDNESS", "PAGE_CROSSFEED",
                     "PAGE_LEVELLER", "PAGE_PSYBASS", "PAGE_BLUETOOTH"):
            self.assertIn(page, renderer)
        listing = function_body(sketch, "void drawSystemSettingsList()")
        self.assertIn("drawFontText(FontMedium", listing)
        self.assertIn("uiListColumns", listing)
        selection = function_body(sketch, "void selectMenuItem()")
        self.assertIn("menuPage == PAGE_FEATURES", selection)
        self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 0", selection)
        back = function_body(sketch, "void goBack()")
        self.assertIn("enterPage(PAGE_FEATURES)", back)
        self.assertIn("enterPage(PAGE_SYSTEM)", back)
        self.assertNotIn("enterPage(PAGE_FEATURES)",
                         function_body(sketch, "void renderMediaBrowser(bool fullFrame)"))

    def test_tube_drive_and_mix_keep_large_numbers_with_matching_small_units(self):
        sketch = SKETCH.read_text()
        value = function_body(sketch, "String tubeValueText(TubeParam p, float value)")
        self.assertIn('if (p == TUBE_DRIVE) return String(value, 1);', value)
        self.assertIn('if (p == TUBE_MIX) return String((int)value);', value)
        self.assertNotIn('pct', value)
        unit = function_body(sketch, "const char *menuValueUnit()")
        self.assertIn('menuPage == PAGE_TUBE && menuIndex == 2', unit)
        percent = function_body(sketch, "bool menuValueUsesPercent()")
        self.assertIn('menuPage == PAGE_TUBE && menuIndex == 3', percent)
        editor = function_body(sketch, "void drawMenuValue(int16_t y, const String &value)")
        self.assertIn('drawMenuNumericWithCompactUnit(y, value, menuValueUnit()', editor)
        numeric = function_body(sketch, "void drawMenuNumericWithCompactUnit(")
        self.assertIn('fontTextWidth(FontLarge, number)', numeric)
        self.assertIn('canvas->setTextSize(2)', numeric)
        listing = function_body(sketch, "void drawSystemSettingsList()")
        self.assertIn('const bool tubeUnit', listing)
        self.assertIn('canvas->setTextSize(2)', listing)

    def test_remote_landing_reports_real_connection_state(self):
        sketch = SKETCH.read_text()
        names = function_body(sketch, "String bleMenuItemName(uint8_t index)")
        self.assertIn('bleConnected ? "Connected" : "Reconnecting"', names)
        self.assertIn('"Not paired"', names)
        self.assertIn('"D-pad Shortcuts"', names)
        self.assertIn('BLE_MENU_RESTORE_DEFAULTS', names)
        values = function_body(sketch, "String bleMenuItemValue(uint8_t index)")
        self.assertIn('bleSavedName', values)
        listing = function_body(sketch, "void drawSystemSettingsList()")
        self.assertIn('menuPage == PAGE_BLUETOOTH', listing)
        self.assertIn('uiDimText()', listing)

    def test_remote_mapping_and_shortcuts_are_four_row_lists(self):
        sketch = SKETCH.read_text()
        self.assertIn('#define BLE_MAPPING_COUNT 21', sketch)
        renderer = function_body(sketch, "void drawBleScreen()")
        self.assertIn('bleUiMode == BLE_UI_MAPPING', renderer)
        self.assertIn('bleUiMode == BLE_UI_SHORTCUTS', renderer)
        self.assertIn('bleUiMode == BLE_UI_SHORTCUT_EDIT', renderer)
        self.assertGreaterEqual(renderer.count('uiPagedWindow('), 3)
        self.assertGreaterEqual(renderer.count('drawListDownArrow('), 3)
        self.assertIn('remoteMapEntrySet(remoteMap[entry])', renderer)
        row = function_body(sketch, "void drawBleListRow(")
        self.assertIn('uiListColumns(', row)
        self.assertIn('uiDimText()', row)
        self.assertIn('ellipsizeFontText(FontMedium', row)
        self.assertIn('return "None"',
                      function_body(sketch, "String remoteLabelForAction("))

    def test_remote_learning_and_removal_are_cancel_safe(self):
        sketch = SKETCH.read_text()
        renderer = function_body(sketch, "void drawBleScreen()")
        self.assertIn('"Press button"', renderer)
        self.assertIn('"Release button"', renderer)
        self.assertIn('bleSavedName', renderer)
        self.assertIn('bleRemoveYes ? "Remove" : "Cancel"', renderer)
        selection = function_body(sketch, "void bleSelect()")
        self.assertIn('bleRemoveYes', selection)
        self.assertIn('bleUiMode == BLE_UI_SHORTCUT_EDIT', selection)
        self.assertIn('const bool awaitSelectRelease = bleButtonHeld', selection)
        back = function_body(sketch, "void bleBack()")
        self.assertIn('bleUiMode == BLE_UI_LEARNING', back)
        self.assertIn('bleUiMode == BLE_UI_SHORTCUT_EDIT', back)
        self.assertIn('bleLearningPacketReady = false', back)
        self.assertNotIn('saveHomeShortcuts()', back)
        self.assertNotIn('saveRemoteMappings()', back)
        report = function_body(sketch, "void processBleReport(const BleReportPacket &packet)")
        self.assertIn('bleLearningPacketReady', report)
        self.assertIn('learnRemoteReport(bleLearningPacket)', report)
        polling = function_body(sketch, "void pollBleRemote()")
        self.assertIn('!bleConnected && bleUiMode == BLE_UI_LEARNING', polling)

    def test_all_feature_icons_fit_and_keep_source_preset_separate(self):
        source = """
#include "UiReadability.h"
static_assert(UI_FEATURE_ALL == 0x7f);
static_assert(uiFeatureIconSpan(UI_FEATURE_ALL) == 138);
static_assert(uiFeatureIconOffset(UI_FEATURE_ALL, UI_FEATURE_LIMITER) == 124);
static_assert(uiFeatureIconSpan(UI_FEATURE_TUBE | UI_FEATURE_LIMITER) == 34);
"""
        result = compile_assertions(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        sketch = SKETCH.read_text()
        top = function_body(sketch, "void drawTopStatus()")
        self.assertIn('UI_FEATURE_TUBE', top)
        self.assertIn('UI_FEATURE_LIMITER', top)
        self.assertIn('const int16_t iconX = std::max<int16_t>(sourceRight + 8', top)
        self.assertIn('drawTubeIcon(', top)
        self.assertIn('drawLimiterIcon(', top)
        self.assertIn('presetLeft', top)
        self.assertIn('drawTaperLine((UI_W / 2) - 2, 72',
                      function_body(sketch, "void drawHome()"))

    def test_tube_shortcut_is_append_only_and_confirmed(self):
        sketch = SKETCH.read_text()
        previous = subprocess.run(
            ["git", "show", "8c6664a:" +
             "firmware/DSPi_ESP32_Front_Panel_v1_1_2/"
             "DSPi_ESP32_Front_Panel_v1_1_2.ino"],
            cwd=ROOT, text=True, capture_output=True, check=True).stdout
        actions = sketch[sketch.index("enum UiAction : uint8_t {"):
                         sketch.index("enum BleMappingKind", sketch.index("enum UiAction : uint8_t {"))]
        old_actions = previous[previous.index("enum UiAction : uint8_t {"):
                               previous.index("enum BleMappingKind", previous.index("enum UiAction : uint8_t {"))]
        old_ids = [entry.strip() for entry in
                   old_actions.split("{")[1].split("}")[0].split(",")]
        new_ids = [entry.strip() for entry in
                   actions.split("{")[1].split("}")[0].split(",")]
        self.assertEqual(new_ids[:-1], old_ids)
        self.assertIn('ACT_SUB_SYNTH_TOGGLE,\n  ACT_TUBE_TOGGLE', actions)
        self.assertIn('#define HOME_SHORTCUT_OPTION_COUNT 15', sketch)
        self.assertIn('"Toggle Sub Synth",\n  "Toggle Tube"', sketch)
        dispatch = function_body(sketch, "void dispatchUiAction(UiAction action)")
        self.assertIn('case ACT_TUBE_TOGGLE:', dispatch)
        self.assertIn('readTubeParam(TUBE_ENABLE, current)', dispatch)
        self.assertIn('writeTubeParam(TUBE_ENABLE, target ? 1 : 0)', dispatch)
        self.assertIn('showFeatureStateNotification("Tube", target)', dispatch)

    def test_runtime_watcher_uses_exact_tube_and_limiter_readback(self):
        sketch = SKETCH.read_text()
        poll = function_body(sketch, "bool pollExternalRuntimeState()")
        self.assertIn('readTubeParam(TUBE_ENABLE, observedTube)', poll)
        self.assertIn('LIMITER_REQUEST, 0x81, 4', poll)
        self.assertIn('limiterStatus[0] <= 1', poll)
        self.assertIn('dspi.limiter.engagedKnown', poll)
        self.assertIn('if (presetChanged)', poll)
        self.assertIn('dspi.tube.known = false;', poll)
        self.assertIn('dspi.limiter.engagedKnown = false;', poll)
        self.assertIn('const bool notificationsAllowed = externalRuntimeStateReady;', poll)
        self.assertIn('showFeatureStateNotification("Tube"', poll)
        self.assertNotIn('showFeatureStateNotification("Limiter"', poll)


if __name__ == "__main__":
    unittest.main()
