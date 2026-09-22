"""Source integration contracts; hardware UART/display tests remain required."""
import re
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "firmware" /
          "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()


class SubSynthContracts(unittest.TestCase):
    def test_new_menu_and_action_ids_are_appended(self):
        self.assertRegex(SOURCE, r"PAGE_THEME\s*,\s*PAGE_SUB_SYNTH")
        self.assertRegex(SOURCE, r"ACT_MEDIA_SETTINGS\s*,\s*ACT_SUB_SYNTH_TOGGLE")

    def test_feature_uses_common_notifications(self):
        self.assertEqual(SOURCE.count('showFeatureStateNotification("Sub Synth",'), 2)

    def test_beta3_link_support_is_probed_without_setting_defaults(self):
        probe = SOURCE.split("bool probeSubSynth(", 1)[1].split("bool syncSubSynthDetails", 1)[0]
        self.assertIn("dspiGetResult(0x2f", probe)
        self.assertNotIn("dspiSet", probe)
        self.assertIn("state.value[SUB_LINK] = data[0]", probe)

    def test_output_knowledge_invalidated_before_detail_read(self):
        body = SOURCE.split("bool syncSubSynthDetails(", 1)[1].split("bool writeSubSynthParam", 1)[0]
        self.assertLess(body.index("outputsKnown = false"), body.index("readSubSynthParam"))
        self.assertIn("getExactByte(0x73", body)

    def test_complete_font_is_used_for_new_title_and_editor(self):
        title = SOURCE.split("void drawMenuTextNative(", 1)[1].split("void drawMenuDots", 1)[0]
        menu_table = SOURCE.split("static const GlyphDef FontMenu_glyphs[] = {", 1)[1].split("};", 1)[0]
        self.assertIn("{'b',", menu_table)
        self.assertIn("drawFontCentredVisual(FontMenu", title)
        self.assertNotIn('text == "Sub Synth"', title)
        self.assertNotIn("menuPage == PAGE_THEME || isSubSynthPage(menuPage)", SOURCE)

    def test_all_sub_synth_value_characters_exist_in_complete_fonts(self):
        for font in ("FontSmall", "FontMedium", "FontLarge"):
            table = SOURCE.split(f"static const GlyphDef {font}_glyphs[] = {{", 1)[1].split("};", 1)[0]
            supported = set(re.findall(r"\{'(.)',", table)) | {" "}
            self.assertFalse(set("Sub Synth Percussive Sustained All material On Off -40.0 dB 100 pct 400 ms") - supported)


if __name__ == "__main__":
    unittest.main()
