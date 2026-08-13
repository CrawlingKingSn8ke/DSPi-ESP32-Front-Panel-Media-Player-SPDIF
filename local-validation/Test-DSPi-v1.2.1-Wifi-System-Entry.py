import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (
    ROOT
    / "firmware"
    / "DSPi_ESP32_Front_Panel_v1_1_2"
    / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
).read_text()


class WifiSystemEntryContracts(unittest.TestCase):
    @staticmethod
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
                    return INO[brace : offset + 1]
        raise AssertionError(f"unterminated function: {signature}")

    def test_system_list_has_wifi_after_volume_limit(self):
        expected = (
            '"Status", "Screen Settings", "Volume Limit", '
            '"WiFi Transfer/Update"'
        )
        self.assertIn(expected, INO)
        self.assertIn("case PAGE_SYSTEM: return 4", INO)

    def test_system_wifi_row_opens_existing_confirmation(self):
        self.assertIn(
            "if (menuPage == PAGE_SYSTEM && menuIndex == 3) {\n"
            "    beginWifiTransferConfirmation();",
            INO,
        )

    def test_music_browser_has_no_wifi_action(self):
        self.assertNotIn("mediaBrowserItemIsWifiTransfer", INO)
        self.assertNotIn('return "Wi-Fi Transfer"', INO)
        self.assertIn("return count;", INO)

    def test_all_transfer_returns_go_to_system(self):
        confirmation = self.function_body("void dispatchUiAction(UiAction action)")
        confirmation = confirmation[
            confirmation.index("if (uiView == VIEW_WIFI_TRANSFER_CONFIRM)") :
            confirmation.index("// Transfer screens deliberately")
        ]
        self.assertNotIn("enterPage(PAGE_MEDIA)", confirmation)
        self.assertGreaterEqual(confirmation.count("enterPage(PAGE_SYSTEM)"), 3)
        lifecycle = INO[INO.index("void finishWifiTransferLifecycle()") :]
        lifecycle = lifecycle[: lifecycle.index("void serviceWifiTransferUiRedraw()")]
        self.assertIn("enterPage(PAGE_SYSTEM)", lifecycle)

    def test_transfer_lifecycle_entry_point_is_unchanged(self):
        self.assertIn("void requestWifiTransferEntry()", INO)
        service = self.function_body("void serviceWifiTransfer()")
        self.assertIn("case WifiTransferPhase::StopMedia", service)
        self.assertIn("mediaPlayerPoc.requestStop()", service)
        self.assertIn("case WifiTransferPhase::FlushPreferences", service)
        self.assertIn("serviceDeferredPreferences()", service)


if __name__ == "__main__":
    unittest.main()
