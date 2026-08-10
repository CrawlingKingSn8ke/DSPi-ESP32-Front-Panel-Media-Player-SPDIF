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

    def test_route_validates_spdif1_gpio5_and_selects_optical(self):
        body = function_body(INO, "bool activateDspiMediaRoute")
        self.assertIn("REQ_GET_SPDIF_INPUT_CONFIG", body)
        self.assertIn("MEDIA_PICO_SPDIF_RX_PIN", body)
        self.assertIn("SRC_OPTICAL", body)
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
        self.assertNotIn("MEDIA_I2S_BCLK_PIN", INO)
        self.assertNotIn("MEDIA_I2S_LRCLK_PIN", INO)


if __name__ == "__main__":
    unittest.main(verbosity=2)
