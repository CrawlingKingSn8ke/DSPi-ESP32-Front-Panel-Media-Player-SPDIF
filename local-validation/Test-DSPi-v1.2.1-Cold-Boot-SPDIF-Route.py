import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
INO = (FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
PLAYER = (FIRMWARE / "MediaPlayerPoC.cpp").read_text()


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


class ColdBootSpdifRouteContracts(unittest.TestCase):
    def test_unprimed_same_source_is_rearmed_through_usb_once(self):
        body = function_body(INO, "bool activateDspiMediaRoute(")
        self.assertIn("liveSource == MEDIA_DSPI_SPDIF_SOURCE", body)
        self.assertIn("!mediaDspiSourcePrimed", body)
        usb = body.index("setInputSource(SRC_USB, false)")
        spdif = body.index("setInputSource(MEDIA_DSPI_SPDIF_SOURCE, false)")
        self.assertLess(usb, spdif)

    def test_real_source_changes_prime_the_route_and_link_loss_clears_it(self):
        setter = function_body(INO, "bool setInputSource(")
        self.assertIn("if (oldSource != dspi.source)", setter)
        self.assertIn("mediaDspiSourcePrimed = true", setter)
        self.assertIn(
            "dspi.connected = false;\n      mediaDspiSourcePrimed = false;", INO
        )

    def test_music_waits_for_exact_spdif_audio_lock_and_native_rate(self):
        body = function_body(INO, "bool activateDspiMediaRoute(")
        self.assertIn("REQ_GET_SPDIF_RX_STATUS", body)
        self.assertIn("statusPayload[0] == 2", body)
        self.assertIn(
            "statusPayload[1] == (uint8_t)MEDIA_DSPI_SPDIF_SOURCE", body
        )
        self.assertIn("readLe32(statusPayload + 4) == sampleRate", body)
        self.assertLess(body.index("if (!locked)"), body.index("mediaRoute.active = true"))

    def test_encoded_silence_is_live_before_route_and_music_tasks(self):
        body = function_body(PLAYER, "MediaStartStatus MediaPlayerPoC::servicePlayStart(")
        self.assertLess(body.index("startSpdif(startDataOutPin)"),
                        body.index("startRouteCallback(currentFile.sampleRate"))
        self.assertLess(body.index("startRouteCallback(currentFile.sampleRate"),
                        body.index('"media-decode"'))

    def test_route_fix_does_not_change_volume_or_pcm_gain(self):
        body = function_body(INO, "bool activateDspiMediaRoute(")
        for forbidden in (
            "REQ_SET_USER_VOLUME",
            "REQ_SET_MASTER_VOLUME",
            "setUserVolume",
            "setMasterVolume",
        ):
            self.assertNotIn(forbidden, body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
