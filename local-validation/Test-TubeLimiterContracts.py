import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
SOURCE = FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
HELPERS = FIRMWARE / "TubeLimiter.h"


class TubeLimiterContracts(unittest.TestCase):
    def test_protocol_and_basic_only(self):
        text = HELPERS.read_text()
        for item in ("TUBE_ENABLE = 0", "TUBE_MASK = 1", "TUBE_TYPE = 2",
                     "TUBE_DRIVE = 3", "TUBE_MIX = 12", "0x3e", "0x3f",
                     "LIMITER_ENABLE = 0", "LIMITER_THRESHOLD = 1",
                     "LIMITER_RELEASE = 2", "LIMITER_LINK = 3", "0x81"):
            self.assertTrue(item in text, item)
        self.assertIn("tubeBasicWritable", text)
        self.assertIn("limiterValid", text)

    def test_menu_uses_existing_list_renderer_and_guards_old_firmware(self):
        text = SOURCE.read_text()
        for item in ("PAGE_TUBE", "PAGE_TUBE_OUTPUTS", "PAGE_LIMITER",
                     "PAGE_LIMITER_OUTPUT", "probeTube", "probeLimiter",
                     "drawSystemSettingsList", "isTubeLimiterPage",
                     "Output Limiter", "Tube Modeller"):
            self.assertTrue(item in text, item)
        self.assertRegex(text, r"isSystemSettingsListPage\([\s\S]*?isTubeLimiterPage")
        list_renderer = text.split("void drawSystemSettingsList()", 1)[1].split("void drawMenu()", 1)[0]
        self.assertIn("FontMedium", list_renderer)
        self.assertIn("uiListColumns", list_renderer)
        self.assertIn("isTubeLimiterPage(menuPage)", list_renderer)

    def test_writes_are_indexed_and_verified(self):
        text = SOURCE.read_text()
        for item in ("tubeBasicWritable(p)", "readTubeParam(p, readback)",
                     "readLimiterParam(output, p, readback)",
                     "limiterValue(out, p)"):
            self.assertTrue(item in text, item)

    def test_output_identity_is_pinned_and_linked_readback_is_refreshed(self):
        text = SOURCE.read_text()
        self.assertIn("limiterDetailOutput = out;", text)
        self.assertIn("selectedLimiterOutput()", text)
        self.assertIn("syncLimiterDetails(false, output)", text)
        self.assertIn("getExactByte(0x73, enabled, output, false)", text)

    def test_basic_tube_does_not_expose_advanced_indices(self):
        text = HELPERS.read_text()
        allowed = text.split("inline bool tubeBasicWritable", 1)[1].split("}", 1)[0]
        for item in ("TUBE_ENABLE", "TUBE_MASK", "TUBE_TYPE", "TUBE_DRIVE", "TUBE_MIX"):
            self.assertIn(item, allowed)
        self.assertNotIn("TUBE_BIAS", text)
        self.assertNotIn("TUBE_SAG", text)


if __name__ == "__main__":
    unittest.main()
