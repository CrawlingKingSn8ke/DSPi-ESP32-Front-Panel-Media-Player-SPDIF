import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
READABILITY = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
               "UiReadability.h").read_text()


def body(signature):
    start = SKETCH.index(signature)
    opening = SKETCH.index("{", start)
    while ";" in SKETCH[start:opening]:
        start = SKETCH.index(signature, start + len(signature))
        opening = SKETCH.index("{", start)
    depth = 0
    for position in range(opening, len(SKETCH)):
        if SKETCH[position] == "{":
            depth += 1
        elif SKETCH[position] == "}":
            depth -= 1
            if depth == 0:
                return SKETCH[opening:position + 1]
    raise AssertionError(signature)


class UiPolishR3Contracts(unittest.TestCase):
    def test_dsp_setup_uses_exact_main_menu_style_and_fits(self):
        menu = body("void drawMenu()")
        self.assertIn("drawMenuTextNative(80, selected, uiMainText())", menu)
        self.assertNotIn("drawFontTextScaledKerned(FontMenu", menu)
        names = body("String menuItemName(MenuPage page, uint8_t index)")
        self.assertIn('"Input", "Music", "Preset", "DSP Setup", "System"', names)
        glyph_table = SKETCH.split("static const GlyphDef FontMenu_glyphs[] = {", 1)[1].split("};", 1)[0]
        advances = dict((letter, int(advance)) for letter, advance in
                        re.findall(r"\{'(.)',\s*\d+,\s*\d+,\s*(\d+),", glyph_table))
        title = "DSP Setup"
        self.assertTrue(all(ch == " " or ch in advances for ch in title))
        width = sum(15 if ch == " " else advances[ch] for ch in title)
        self.assertEqual(width, 299)
        self.assertLessEqual(width, 320 - 16)

    def test_home_icons_enlarge_with_seven_pixel_gap_and_bounded_labels(self):
        home = body("void drawTopStatus()")
        self.assertIn("ICON_SCALE_PERCENT = 125", home)
        self.assertIn("ICON_GAP = 7", home)
        self.assertIn("uiFeatureIconScaledSpan(", home)
        self.assertEqual(home.count("uiFeatureIconScaledOffset("), 7)
        self.assertIn("sourceBudget", home)
        self.assertIn("presetLeft - iconSpan - 8", home)
        self.assertIn("iconY = 20", home)
        self.assertIn("uiFeatureIconScaledSpan(UI_FEATURE_ALL, 125, 7) == 178",
                      READABILITY)

    def test_feature_menu_keeps_original_icon_size_and_meter_colour(self):
        painter = body("void drawFeatureIconForIndex(")
        self.assertIn("uiVolumeMeterColour()", painter)
        self.assertNotIn("ICON_SCALE_PERCENT", painter)
        for name in ("Ear", "Headphones", "Leveller", "Psybass",
                     "SubSynth", "Tube", "Limiter"):
            function = body(f"void draw{name}Icon(")
            self.assertIn("scalePercent = 100", SKETCH[SKETCH.index(f"void draw{name}Icon("):
                                                        SKETCH.index("{", SKETCH.index(f"void draw{name}Icon("))])
            self.assertIn("FeatureIconPainter", function)


if __name__ == "__main__":
    unittest.main()
