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
        self.assertIn('"Input", "Music", "Preset", "Features", "System"', names)
        self.assertIn('"Loudness", "Crossfeed", "Leveller", "Psy Bass",', names)
        self.assertIn('"Sub Synth", "Tube Modeller", "Output Limiter"', names)
        self.assertIn('"WiFi Transfer/Update", "Remote"', names)
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
        self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 4", selection)
        back = function_body(sketch, "void goBack()")
        self.assertIn("enterPage(PAGE_FEATURES)", back)
        self.assertIn("enterPage(PAGE_SYSTEM)", back)
        self.assertNotIn("enterPage(PAGE_FEATURES)",
                         function_body(sketch, "void renderMediaBrowser(bool fullFrame)"))


if __name__ == "__main__":
    unittest.main()
