import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
PROTOCOL = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
            "SpectrumRta.h").read_text()


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


class StereoSpectrumContracts(unittest.TestCase):
    def test_protocol_is_v3_selected_pair_only(self):
        for token in ("kVersion = 3", "kBands = 37", "kBandFrameBytes = 82",
                      "kOutputTap = 1", "kInputTap = 0",
                      "channelMask(selection)",
                      "wire[1] = selection.tap", "wire[1] != expectedChannel",
                      "wire[3] > kBands", "wire + 8"):
            self.assertIn(token, PROTOCOL)
        self.assertIn("levelZero) - 120", PROTOCOL)
        self.assertIn("fitSelection", PROTOCOL)
        self.assertIn("uint8_t payload[96]", SKETCH)
        self.assertNotIn("REQ_RTA_GET_BANDS_ALL", SKETCH)

    def test_digital_page_is_gone_but_legacy_preference_is_migrated(self):
        for removed in ("Digital VU", "SCREEN_TIMEOUT_DIGITAL_VU",
                        "VISUALIZER_STEREO", "drawStereoVuColumn",
                        "drawStereoVisualizer", "VISUALIZER_SEGMENT_COUNT"):
            self.assertNotIn(removed, SKETCH)
        self.assertRegex(SKETCH, r"SCREEN_TIMEOUT_SPECTRUM\s*=\s*2")
        self.assertIn('case SCREEN_TIMEOUT_SPECTRUM: return "Spectrum"', SKETCH)
        self.assertIn("VISUALIZER_ANALOG", body("void toggleVisualizer()"))
        self.assertIn("VISUALIZER_SPECTRUM", body("void toggleVisualizer()"))

    def test_polling_is_visible_only_and_music_safe(self):
        service = body("void serviceStereoSpectrum()")
        meters = body("void serviceVisibleMeters(bool mediaActive)")
        self.assertIn("if (!stereoSpectrumVisible() || !dspi.connected) return", service)
        self.assertIn("REQ_RTA_GET_CAPS", service)
        self.assertIn("REQ_RTA_SET_CONFIG", service)
        self.assertIn("REQ_RTA_GET_CONFIG", service)
        self.assertIn("memcmp(current, stereoSpectrum.configWire", service)
        self.assertIn("REQ_RTA_GET_BANDS", service)
        self.assertIn("selectedSpectrumChannelsLive()", service)
        self.assertIn("spectrumSelection.upper", service)
        self.assertIn("spectrumSelection.lower", service)
        self.assertIn("mediaPlaybackBufferLow()", meters)
        self.assertLess(meters.index("mediaPlaybackBufferLow()"),
                        meters.index("serviceStereoSpectrum();"))
        self.assertIn("visualizerPage == VISUALIZER_SPECTRUM",
                      body("bool flushVisualizerFrame()"))

    def test_exit_stops_only_our_configuration(self):
        exit_service = body("void serviceStereoSpectrumExit()")
        self.assertIn("REQ_RTA_GET_CONFIG", exit_service)
        self.assertIn("memcmp(current, stereoSpectrum.configWire", exit_service)
        self.assertIn("REQ_RTA_CONTROL", exit_service)
        self.assertIn("serviceStereoSpectrumExit();", body("void loop()"))

    def test_menu_and_display_readability_contract(self):
        self.assertIn("PAGE_SPECTRUM", SKETCH)
        self.assertIn('"Upper Channel", "Lower Channel"', SKETCH)
        self.assertIn("REQ_GET_OUTPUT_ENABLE", body("bool refreshSpectrumAvailability()"))
        self.assertIn("readStatusU32ForMeter(23, activeInputs)",
                      body("bool refreshSpectrumAvailability()"))
        self.assertIn("spectrumAvailabilityKnown = false",
                      body("bool refreshSpectrumAvailability()"))
        self.assertIn("if (!spectrumAvailabilityKnown) return 0",
                      body("uint16_t spectrumAvailableMask(uint8_t tap)"))
        display = body("bool drawSpectrumVisualizer()")
        self.assertIn("sourceText()", display)
        self.assertIn("presetText()", display)
        self.assertIn('"Spectrum"', display)
        channel = body("void drawSpectrumChannel(uint8_t channel, int16_t baseline, uint16_t colour)")
        self.assertIn('"-20"', channel)
        self.assertIn('"-40"', channel)
        self.assertIn('"-60"', channel)
        self.assertIn("FontMedium", channel)
        self.assertIn("uiAccentSoft()", display)

    def test_manual_visualizers_do_not_dim(self):
        power = body("void serviceScreenPower()")
        self.assertIn("uiView == VIEW_VISUALIZER && !screenTimeoutViewActive", power)
        self.assertIn("lastUserActivityAt = millis()", power)
        self.assertIn("startBacklightFade(uiBrightnessPwm())", power)

    def test_selection_persists_separately_from_legacy_panel_record(self):
        self.assertIn('putBytes("sp_cfg", &spectrumSelection', SKETCH)
        self.assertIn('getBytesLength("sp_cfg")', SKETCH)
        self.assertIn('"spectrum_p%u"', SKETCH)


if __name__ == "__main__":
    unittest.main()
