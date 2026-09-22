"""Pixel and UI contracts for the photographed menu defects."""
import re
import unittest
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "firmware" /
          "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()


def menu_glyph(code, width, height):
    match = re.search(
        rf"static const uint8_t FontMenu_{code}\[\] PROGMEM = \{{(.*?)\}};",
        SOURCE, re.S)
    assert match, f"missing menu glyph {code}"
    encoded = [int(value, 16) for value in
               re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1))]
    pixels = [alpha for count, alpha in zip(encoded[::2], encoded[1::2])
              for _ in range(count)]
    assert len(pixels) == width * height
    return [pixels[y * width:(y + 1) * width] for y in range(height)], len(encoded)


class GlyphKeyMapContracts(unittest.TestCase):
    def test_h_has_only_its_left_ascender_above_the_arch(self):
        rows, encoded_length = menu_glyph("68", 35, 45)
        for y in range(13):
            self.assertTrue(any(rows[y][3:10]))
            self.assertFalse(any(rows[y][15:]), f"stray upper stem at row {y}")
        self.assertTrue(any(rows[13][15:24]))
        match = re.search(r"\{'h', 35, 45, 35, 0, 16, (\d+), FontMenu_68\}", SOURCE)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), encoded_length)

    def test_r_has_a_clear_counter_and_no_detached_left_pixels(self):
        rows, encoded_length = menu_glyph("52", 40, 45)
        self.assertTrue(any(rows[0][4:29]))
        self.assertFalse(any(row[1] for row in rows))
        for y in range(6, 22):
            self.assertFalse(any(rows[y][12:22]),
                             f"bright defect inside R counter at row {y}")
        match = re.search(r"\{'R', 40, 45, 40, 0, 16, (\d+), FontMenu_52\}", SOURCE)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), encoded_length)

    def test_remote_key_map_label_uses_readable_coloured_text(self):
        start = SOURCE.index("} else if (bleUiMode == BLE_UI_MAPPING) {")
        end = SOURCE.index("} else if (bleUiMode == BLE_UI_LEARNING) {", start)
        body = SOURCE[start:end]
        self.assertIn("remoteLabelForAction(bleMappingIndex)", body)
        self.assertIn("drawFontCentredGlowColour(FontMedium, 166, label", body)
        self.assertNotIn("drawFontCentredGlowColour(FontSmall, 166, label", body)


if __name__ == "__main__":
    unittest.main()
