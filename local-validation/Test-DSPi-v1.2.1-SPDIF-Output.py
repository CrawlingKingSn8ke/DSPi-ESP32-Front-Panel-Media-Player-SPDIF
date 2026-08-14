import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
INO = (FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
PLAYER = (FIRMWARE / "MediaPlayerPoC.cpp").read_text()
ENCODER = (FIRMWARE / "SpdifBlockEncoder.cpp").read_text()
REFERENCE = (
    ROOT.parent
    / "squeezelite-spdif-reference"
    / "components"
    / "squeezelite"
    / "output_i2s.c"
).read_text()


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : offset + 1]
    raise AssertionError(f"unterminated function {signature}")


def lookup_values(source: str, declaration: str):
    start = source.index(declaration)
    brace = source.index("{", start)
    end = source.index("};", brace)
    return [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", source[brace:end])]


class SpdifOutputContracts(unittest.TestCase):
    def test_encoder_table_exactly_matches_proven_reference(self):
        ours = lookup_values(ENCODER, "kBmcLookup[256]")
        reference = lookup_values(REFERENCE, "spdif_bmclookup[256]")
        self.assertEqual(256, len(ours))
        self.assertEqual(reference, ours)

    def test_encoder_is_24_bit_and_has_consumer_block_preambles(self):
        self.assertIn(">> 24", ENCODER)
        self.assertIn(">> 16", ENCODER)
        self.assertIn(">> 8", ENCODER)
        self.assertIn("kPreambleB", ENCODER)
        self.assertIn("kPreambleM", ENCODER)
        self.assertIn("kPreambleW", ENCODER)
        self.assertIn("frameNumber_ >= 192", ENCODER)
        self.assertIn("stereoPcm ? stereoPcm[frame * 2] : 0", ENCODER)

    def test_media_accepts_only_requested_rates(self):
        body = function_body(PLAYER, "bool nativeRateSupported")
        self.assertIn("sampleRate == 44100", body)
        self.assertIn("sampleRate == 48000", body)
        self.assertNotIn("96000", body)

    def test_transmitter_is_master_with_internal_clocks_and_data_only_pin(self):
        body = function_body(PLAYER, "bool MediaPlayerPoC::startSpdif")
        self.assertIn("I2S_ROLE_MASTER", body)
        self.assertIn("currentFile.sampleRate * 2u", body)
        self.assertIn("I2S_STD_MSB_SLOT_DEFAULT_CONFIG", body)
        self.assertIn("standardConfig.gpio_cfg.bclk = I2S_GPIO_UNUSED", body)
        self.assertIn("standardConfig.gpio_cfg.ws = I2S_GPIO_UNUSED", body)
        self.assertIn("standardConfig.gpio_cfg.dout = (gpio_num_t)dataOutPin", body)
        self.assertIn("I2S_CLK_SRC_APLL", body)

    def test_output_uses_block_dma_and_valid_encoded_silence(self):
        body = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        self.assertIn("kOutputChunkFrames * SpdifBlockEncoder::kWordsPerStereoFrame", body)
        self.assertIn("encoder.encode(samples, frames", body)
        self.assertIn("i2s_channel_write(tx, next, remaining", body)
        self.assertGreaterEqual(body.count("sendFrames(nullptr"), 4)

    def test_route_validates_spdif1_gpio5_and_selects_optical1(self):
        body = function_body(INO, "bool activateDspiMediaRoute")
        self.assertIn("REQ_GET_SPDIF_INPUT_CONFIG", body)
        self.assertIn("MEDIA_PICO_SPDIF_RX_PIN", body)
        self.assertIn("MEDIA_DSPI_SPDIF_SOURCE", body)
        self.assertIn("MEDIA_DSPI_SPDIF_INPUT_INDEX", body)
        self.assertIn("S/PDIF input 1", body)
        self.assertNotIn("setDspiInputRate", body)
        self.assertNotIn("REQ_SET_I2S_CLOCK_MODE", body)

    def test_restore_only_restores_prior_source(self):
        body = function_body(INO, "bool restoreDspiMediaRoute")
        self.assertIn("setInputSource(mediaRoute.source", body)
        self.assertNotIn("setDspiInputRate", body)
        self.assertNotIn("REQ_SET_I2S_CLOCK_MODE", body)

    def test_test_wiring_constants_are_single_wire(self):
        self.assertIn("#define MEDIA_SPDIF_DATA_OUT_PIN 13", INO)
        self.assertIn("#define MEDIA_PICO_SPDIF_RX_PIN   5", INO)
        self.assertIn(
            "MEDIA_DSPI_SPDIF_SOURCE = SRC_OPTICAL", INO
        )
        self.assertIn("MEDIA_DSPI_SPDIF_INPUT_INDEX = 0", INO)
        self.assertNotIn("MEDIA_I2S_BCLK_PIN", INO)
        self.assertNotIn("MEDIA_I2S_LRCLK_PIN", INO)

    def test_spdif_dma_never_auto_clears_to_invalid_raw_zero_carrier(self):
        start = function_body(PLAYER, "bool MediaPlayerPoC::startSpdif")
        self.assertIn("channelConfig.auto_clear_after_cb = false", start)
        self.assertIn("encodedSilence", start)
        self.assertIn("silenceEncoder.encode(nullptr, 192", start)
        self.assertIn("i2s_channel_write(tx, encodedSilence", start)

    def test_dma_ring_is_deep_and_exactly_spdif_block_aligned(self):
        self.assertIn("kSpdifDmaFramesPerDescriptor = 384", PLAYER)
        self.assertIn("kSpdifDmaDescriptorCount = 16", PLAYER)
        self.assertIn("kSpdifInterruptPriority = 2", PLAYER)
        self.assertIn("kOutputTaskPriority = 6", PLAYER)
        self.assertIn(
            "kSpdifDmaFramesPerDescriptor == 192u * 2u", PLAYER
        )
        start = function_body(PLAYER, "bool MediaPlayerPoC::startSpdif")
        self.assertIn(
            "channelConfig.dma_desc_num = kSpdifDmaDescriptorCount", start
        )
        self.assertIn(
            "channelConfig.dma_frame_num = kSpdifDmaFramesPerDescriptor", start
        )
        self.assertIn(
            "block < kSpdifDmaDescriptorCount", start
        )

    def test_stopped_transmitter_is_explicitly_held_low(self):
        stop = function_body(PLAYER, "void MediaPlayerPoC::stopSpdif")
        self.assertIn("pinMode(spdifDataOutPin, OUTPUT)", stop)
        self.assertIn("digitalWrite(spdifDataOutPin, LOW)", stop)
        self.assertIn("spdifDataOutPin = -1", stop)

    def test_lightweight_runtime_watcher_confirms_spdif_lock_and_rate(self):
        body = function_body(INO, "bool pollExternalRuntimeState()")
        self.assertIn("REQ_GET_SPDIF_RX_STATUS", body)
        self.assertIn("REQ_GET_SPDIF_RX_CH_STATUS", body)
        self.assertIn("dspi.spdifState = observedSpdifState", body)
        self.assertIn("dspi.sampleRate = observedSampleRate", body)
        self.assertIn("spdifStatusChanged", body)

    def test_returning_home_requests_immediate_spdif_status(self):
        body = function_body(INO, "\nvoid transitionToHome()\n")
        self.assertIn("mediaPlayerPoc.active()", body)
        self.assertIn("isSpdifSource(dspi.source)", body)
        self.assertIn("externalRuntimeRefreshRequested = true", body)

    def test_long_spdif_overlay_keeps_large_scaled_glyphs(self):
        body = function_body(INO, "void drawChangeOverlay()")
        self.assertIn("fontTextWidthScaledKerned(FontLarge, source", body)
        self.assertIn("drawFontTextScaledKerned(FontLarge", body)
        self.assertNotIn(
            "drawFontCentredGlowColour(FontMedium, 119, source", body
        )

    def test_volume_limit_stops_media_before_dspi_flash(self):
        body = function_body(INO, "void applyEdit()")
        stop = body.index('stopMediaPlayback("volume limit flash guard")')
        persist = body.index("persistIndependentMasterVolumeVerified()")
        self.assertLess(stop, persist)
        self.assertIn("!mediaPlayerPoc.active()", body)
        self.assertIn("!mediaTrackTransitionActive()", body)
        self.assertIn("!mediaRouteRestorePending", body)
        self.assertNotIn("beginExternalHold", body)
        self.assertNotIn("endExternalHold", body)

    def test_all_real_local_input_changes_stop_music(self):
        apply_body = function_body(INO, "void applyEdit()")
        shortcut_body = function_body(INO, "void changeInput(int direction)")
        self.assertIn(
            "mediaSessionPresent && selectedSource != dspi.source",
            apply_body,
        )
        self.assertIn(
            "mediaSessionPresent && target != baseSource", shortcut_body
        )
        for body in (apply_body, shortcut_body):
            self.assertIn("mediaPlaybackSuspended", body)
            self.assertIn("mediaCurrentPath[0]", body)
            self.assertIn("mediaTrackTransitionActive()", body)
        self.assertNotIn("target != SRC_I2S", shortcut_body)

    def test_console_input_change_still_stops_music(self):
        loop_body = function_body(INO, "void loop()")
        self.assertIn('stopMediaPlayback("external input change")', loop_body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
