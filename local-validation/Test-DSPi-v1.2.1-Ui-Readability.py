import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
       "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()


def function_body(signature):
    start = INO.rindex(signature)
    brace = INO.index("{", start)
    depth = 0
    for offset in range(brace, len(INO)):
        if INO[offset] == "{":
            depth += 1
        elif INO[offset] == "}":
            depth -= 1
            if depth == 0:
                return INO[brace:offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


def menu_glyph(code):
    match = re.search(
        rf"static const uint8_t FontMenu_{code}\[\] PROGMEM = \{{(.*?)\n\}};",
        INO, re.S)
    if not match:
        return None
    encoded = [int(item, 16) for item in re.findall(
        r"0x([0-9A-Fa-f]{2})", match.group(1))]
    pixels = []
    for count, alpha in zip(encoded[0::2], encoded[1::2]):
        pixels.extend([alpha] * count)
    return pixels


class UiReadabilityContracts(unittest.TestCase):
    def test_sub_synth_uses_complete_native_menu_font(self):
        glyph_b = menu_glyph("62")
        glyph_d = menu_glyph("64")
        self.assertIsNotNone(glyph_b)
        self.assertEqual(len(glyph_b), 36 * 45)
        mirrored_d = []
        for row in range(45):
            mirrored_d.extend(reversed(glyph_d[row * 36:(row + 1) * 36]))
        self.assertEqual(glyph_b, mirrored_d)
        menu_text = function_body("void drawMenuTextNative")
        self.assertNotIn('text == "Sub Synth"', menu_text)
        self.assertIn("drawFontCentredVisual(FontMenu", menu_text)

    def test_list_values_and_notifications_use_option_font(self):
        body = function_body("void drawSystemSettingsList")
        self.assertIn("uiListColumns", body)
        self.assertIn("drawFontRight(FontMedium", body)
        self.assertIn("drawFontCentredGlowColour(FontMedium", body)
        self.assertNotIn("drawFontRight(FontSmall", body)

    def test_analog_vu_parent_and_editor_use_swatches(self):
        body = function_body("void drawSystemSettingsList")
        self.assertIn("void drawAnalogVuSwatchEditor", INO)
        editor = function_body("void drawAnalogVuSwatchEditor")
        self.assertIn("row < 4", body)
        self.assertIn("uiAnalogVuColourForChoice", body)
        self.assertIn("uiAnalogSwatchX", editor)
        self.assertIn("vuColourChoiceText", editor)

    def test_status_omits_connected_ready_line_and_uses_medium_details(self):
        body = function_body("void drawStatusScreen")
        self.assertNotIn('"DSPi ready"', body)
        self.assertGreaterEqual(body.count("FontMedium"), 5)

    def test_wifi_active_screen_contains_only_large_essential_lines(self):
        body = function_body("void drawWifiTransferScreen")
        for removed in ("BLE remote inactive", "Do not remove power",
                        "Finish safely in browser"):
            self.assertNotIn(removed, body)
        self.assertIn('FontMedium, 42, "Network", uiAccent()', body)
        self.assertIn('FontMedium, 90, "Password", uiAccent()', body)
        self.assertNotIn("FontSmall", body)


if __name__ == "__main__":
    unittest.main()
