"""Contracts for Spectrum peak markers and temporary full-screen overlays."""
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


class SpectrumPeaksAndOverlays(unittest.TestCase):
    def test_peak_marker_has_hold_fall_and_new_hit(self):
        update = body("void updateSpectrumPeaks(uint8_t slot, const SpectrumRta::BandFrame &frame)")
        draw = body("void drawSpectrumChannel(uint8_t channel, int16_t baseline, uint16_t colour)")
        self.assertIn("height >= peak", update)
        self.assertIn("peak = height", update)
        self.assertIn("now + 240", update)
        self.assertIn("elapsed * 42U", update)
        self.assertIn("std::max<uint8_t>(height", update)
        self.assertIn("acceptSpectrumFrame(slot, frame)", body("void serviceStereoSpectrum()"))
        self.assertIn("updateSpectrumPeaks(slot, frame)", body("void acceptSpectrumFrame(uint8_t slot, const SpectrumRta::BandFrame &frame)"))
        self.assertIn("baseline - peak - (peak ? 2 : 0)", draw)
        self.assertIn("width, 1, colour", draw)
        self.assertIn("nextX - x - 1", draw)

    def test_feature_icons_and_shortcuts_use_home_roles(self):
        display = body("bool drawSpectrumVisualizer()")
        self.assertIn("drawTopStatus()", display)
        header = body("void drawTopStatus()")
        for icon in ("drawEarIcon", "drawHeadphonesIcon", "drawLevellerIcon",
                     "drawPsybassIcon", "drawSubSynthIcon", "drawTubeIcon",
                     "drawLimiterIcon"):
            self.assertIn(icon, header)
        remote = body("void processBleReport(const BleReportPacket &packet)")
        self.assertIn("visualizerPage == VISUALIZER_SPECTRUM", remote)
        self.assertIn("resolveHomeShortcut(action)", remote)

    def test_notifications_return_to_spectrum_without_reconfiguring_rta(self):
        feature = body("void showFeatureStateNotification(const char *featureName, bool enabled)")
        self.assertIn("VISUALIZER_SPECTRUM", feature)
        self.assertIn("featureConfirmReturnView = uiView", body(
            "void showFeatureConfirmation(const char *featureName, bool enabled)"))
        self.assertIn("redrawCurrentView()", body("void dismissFeatureConfirmation()"))
        change = body("void dismissChangeOverlay()")
        self.assertIn("uiView = changeOverlayReturnView", change)
        self.assertIn("redrawCurrentView()", change)
        exit_service = body("void serviceStereoSpectrumExit()")
        self.assertIn("spectrumNotification", exit_service)
        self.assertIn("changeOverlayReturnView == VIEW_VISUALIZER", exit_service)
        self.assertIn("featureConfirmReturnView == VIEW_VISUALIZER", exit_service)
        service = body("void serviceStereoSpectrum()")
        self.assertIn("memcmp(desiredConfig, stereoSpectrum.configWire", service)
        self.assertIn("REQ_RTA_GET_CONFIG", service)
        self.assertIn("REQ_RTA_SET_CONFIG", service)
        self.assertIn("memset(stereoSpectrum.peakHeight", service)

    def test_volume_repeat_updates_only_number_region(self):
        volume = body("void showSpectrumVolumeNotification()")
        self.assertIn("refreshNumberOnly", volume)
        self.assertIn("drawMediaVolumeOverlay(true)", volume)
        self.assertIn('featureConfirmName = ""', volume)
        self.assertIn("featureConfirmUntil = millis() + 900", volume)
        card = body("void drawFeatureConfirmation()")
        self.assertLess(card.index("if (featureConfirmVolume)"),
                        card.index("drawMenuTextNative"))
        self.assertIn("drawMediaVolumeOverlay(false)", card)
        self.assertIn("showSpectrumVolumeNotification()", body(
            "void changeVolume(float delta)"))
        dispatch = body("void dispatchUiAction(UiAction action)")
        self.assertIn('showFeatureStateNotification("Mute", dspi.muted)', dispatch)
        self.assertIn("VISUALIZER_SPECTRUM", dispatch)


if __name__ == "__main__":
    unittest.main()
