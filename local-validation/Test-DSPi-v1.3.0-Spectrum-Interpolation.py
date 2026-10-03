"""Contracts for external-source RTA interpolation and audio-safe rendering."""
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
PROTOCOL = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
            "SpectrumRta.h").read_text()


def body(source, signature):
    match = re.search(re.escape(signature) + r"\s*\{", source)
    if not match:
        raise AssertionError(signature)
    opening = source.index("{", match.start())
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if depth == 0:
            return source[opening:pos + 1]
    raise AssertionError(signature)


class SpectrumInterpolationContracts(unittest.TestCase):
    def test_integer_interpolation_is_bounded_and_handles_both_directions(self):
        lerp = body(PROTOCOL, "inline uint8_t interpolateHeight(uint8_t from, uint8_t to,\n                                 uint32_t elapsedMs, uint32_t durationMs)")
        self.assertIn("!durationMs || elapsedMs >= durationMs", lerp)
        self.assertIn("to >= from ? to - from : from - to", lerp)
        self.assertIn("distance * elapsedMs", lerp)
        self.assertIn("from + step", lerp)
        self.assertIn("from - step", lerp)

    def test_external_data_cadence_and_music_safety_are_separate(self):
        service = body(SKETCH, "void serviceStereoSpectrum()")
        self.assertIn("#define SPECTRUM_EXTERNAL_CHANNEL_POLL_MS 30", SKETCH)
        self.assertIn("#define SPECTRUM_MEDIA_CHANNEL_POLL_MS 80", SKETCH)
        self.assertIn("#define SPECTRUM_EXTERNAL_RENDER_MS 40", SKETCH)
        self.assertIn("SPECTRUM_MEDIA_CHANNEL_POLL_MS", service)
        self.assertIn("SPECTRUM_EXTERNAL_CHANNEL_POLL_MS", service)
        self.assertIn("if (pollDue && stereoSpectrum.nextChannel == 0) drawVisualizer()", service)
        self.assertIn("drawSpectrumInterpolatedFrame()", service)
        self.assertIn("SPECTRUM_FULL_REFRESH_MS", service)
        self.assertIn("mediaPlaybackBufferLow()", body(SKETCH, "void serviceVisibleMeters(bool mediaActive)"))

    def test_duplicate_frames_do_not_restart_interpolation(self):
        accept = body(SKETCH, "void acceptSpectrumFrame(uint8_t slot, const SpectrumRta::BandFrame &frame)")
        self.assertIn("stereoSpectrum.frame[slot].sequence == frame.sequence", accept)
        self.assertIn("memcmp(stereoSpectrum.frame[slot].average, frame.average", accept)
        self.assertIn("spectrumRenderedHeight(slot, band, now)", accept)
        self.assertIn("mediaPlayerPoc.active()", accept)
        self.assertIn("SPECTRUM_INTERPOLATION_MS", accept)

    def test_falling_external_bar_uses_new_sample_without_extra_interpolation(self):
        accept = body(SKETCH, "void acceptSpectrumFrame(uint8_t slot, const SpectrumRta::BandFrame &frame)")
        self.assertIn("target < rendered ? target : rendered", accept)
        self.assertIn("mediaPlayerPoc.active()", accept)
        self.assertIn("SPECTRUM_INTERPOLATION_MS", accept)
        self.assertIn("wire[6] = 120", PROTOCOL)

    def test_empty_spectrum_has_no_waiting_message(self):
        for signature in ("bool drawSpectrumInterpolatedFrame()",
                          "bool drawSpectrumVisualizer()"):
            self.assertNotIn("Waiting for audio", body(SKETCH, signature))

    def test_intermediate_transfer_covers_only_dynamic_plot_rows(self):
        frame = body(SKETCH, "bool drawSpectrumInterpolatedFrame()")
        transfer = body(SKETCH, "bool flushCanvasFullWidthRegionTryLocked(int16_t y, int16_t h,\n                                         uint32_t timeoutMs)")
        self.assertIn("regionY = 54", frame)
        self.assertIn("regionH = 163", frame)
        self.assertIn("flushCanvasFullWidthRegionTryLocked", frame)
        self.assertIn("mediaSharedSpiTryLock(timeoutMs)", transfer)
        self.assertIn("panel->draw16bitRGBBitmap(0, y, framebuffer +", transfer)
        self.assertLess(320 * 163 * 2, 320 * 240 * 2)
        self.assertNotIn("drawTopStatus()", frame)


if __name__ == "__main__":
    unittest.main()
