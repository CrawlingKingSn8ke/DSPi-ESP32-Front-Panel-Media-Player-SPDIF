import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()


def body(signature):
    match = re.search(re.escape(signature) + r"\s*\{", SKETCH)
    if not match:
        raise AssertionError(signature)
    opening = SKETCH.index("{", match.start())
    depth = 0
    for pos in range(opening, len(SKETCH)):
        depth += (SKETCH[pos] == "{") - (SKETCH[pos] == "}")
        if depth == 0:
            return SKETCH[opening:pos + 1]
    raise AssertionError(signature)


class SpectrumBarColourContracts(unittest.TestCase):
    def test_separate_per_channel_persisted_record(self):
        self.assertIn("uint8_t inputPalette[16]", SKETCH)
        self.assertIn("uint8_t outputPalette[16]", SKETCH)
        self.assertIn("validSpectrumBarColours(candidate)", SKETCH)
        self.assertIn('getBytesLength("spbar_cfg")', SKETCH)
        self.assertIn('putBytes("spbar_cfg", &spectrumBarColours', SKETCH)
        self.assertIn('"spbar_p%u"', SKETCH)
        self.assertIn("spectrumBarColours.outputPalette[channel]", SKETCH)
        self.assertIn("spectrumBarColours.inputPalette[channel]", SKETCH)

    def test_menu_lists_live_channels_with_swatches(self):
        self.assertIn('"Bar Colours"', SKETCH)
        self.assertIn("case PAGE_SPECTRUM_COLOURS", SKETCH)
        self.assertIn("spectrumColourRowCount()", body("uint8_t menuItemCount(MenuPage page)"))
        self.assertIn("spectrumColourChannelForRow(row, tap, channel)",
                      body("void drawSystemSettingsList()"))
        self.assertIn("spectrumBarColour(tap, channel)",
                      body("void drawSystemSettingsList()"))
        self.assertIn("drawPaletteSwatchEditor(y)",
                      body("void drawMenuValue(int16_t y, const String &value)"))
        self.assertIn("PAGE_SPECTRUM_COLOURS", body("void adjustEdit(int direction)"))
        self.assertIn("PAGE_SPECTRUM_COLOURS", body("void applyEdit()"))

    def test_spectrum_does_not_follow_other_colour_roles(self):
        display = body("bool drawSpectrumVisualizer()")
        self.assertIn("spectrumBarColour(spectrumSelection.tap", display)
        self.assertNotIn("uiVolumeMeterColour()", display)
        self.assertNotIn("volumeMeterPaletteIndex", display)
        self.assertNotIn("uiAccent()", display)

    def test_real_bands_and_reference_frequency_labels(self):
        channel = body("void drawSpectrumChannel(uint8_t channel, int16_t baseline, uint16_t colour)")
        display = body("bool drawSpectrumVisualizer()")
        self.assertIn("firstBand = 3", channel)
        self.assertIn("band < bandCount", channel)
        self.assertIn("stereoSpectrum.frame[channel].count", channel)
        self.assertIn("canvas->fillRect(x, baseline - h", channel)
        self.assertNotIn("fillRoundRect(x, baseline - h", channel)
        for label in ('"20"', '"50"', '"100"', '"200"', '"500"',
                      '"1k"', '"2k"', '"5k"', '"10k"'):
            self.assertIn(f"canvas->print({label})", display)
        self.assertNotIn('canvas->print("20k")', display)
        # At 44.1/48 kHz DSPi sends 34 bands, so 20 Hz (index 3) to the
        # final real band gives 31 displayed bars.
        self.assertEqual(34 - 3, 31)


if __name__ == "__main__":
    unittest.main()
