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


if __name__ == "__main__":
    unittest.main()
