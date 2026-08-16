import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
INO = (FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
PLAYER = (FIRMWARE / "MediaPlayerPoC.cpp").read_text()
HEADER = (FIRMWARE / "MediaPlayerPoC.h").read_text()


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


class ContinuousSpdifCarrierContracts(unittest.TestCase):
    def test_track_transition_has_dedicated_retaining_stop(self):
        self.assertIn("void requestTrackTransitionStop();", HEADER)
        request = function_body(INO, "bool requestMediaTrackTransition")
        self.assertIn("mediaPlayerPoc.requestTrackTransitionStop()", request)

    def test_ordinary_stop_explicitly_disables_retention(self):
        stop = function_body(PLAYER, "void MediaPlayerPoC::requestStop()")
        transition = function_body(
            PLAYER, "void MediaPlayerPoC::requestTrackTransitionStop()"
        )
        self.assertIn("requestStopInternal(false)", stop)
        self.assertIn("requestStopInternal(true)", transition)

    def test_same_pin_and_rate_reuse_existing_channel(self):
        start = function_body(PLAYER, "bool MediaPlayerPoC::startSpdif")
        reuse = start.index("spdifSampleRate == currentFile.sampleRate")
        teardown = start.index("stopSpdif()")
        self.assertLess(reuse, teardown)
        self.assertIn("spdifDataOutPin == dataOutPin", start)
        self.assertIn("reused continuous carrier", start)

    def test_rate_change_still_recreates_transmitter(self):
        start = function_body(PLAYER, "bool MediaPlayerPoC::startSpdif")
        self.assertIn("spdifSampleRate == currentFile.sampleRate", start)
        self.assertIn("stopSpdif()", start)
        stop = function_body(PLAYER, "void MediaPlayerPoC::stopSpdif")
        self.assertIn("spdifSampleRate = 0", stop)

    def test_finished_track_keeps_writing_valid_silence(self):
        output = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        finished = output.index("state = MediaPlaybackState::Finished")
        keepalive = output.index(
            "while (!stopRequested && state != MediaPlaybackState::Error)",
            finished,
        )
        silence = output.index(
            "sendFrames(nullptr, kOutputChunkFrames)", keepalive
        )
        self.assertLess(finished, keepalive)
        self.assertLess(keepalive, silence)

    def test_retained_dma_ring_contains_only_block_aligned_silence(self):
        output = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        self.assertIn("encoder.frameNumber()", output)
        self.assertIn("(192u - (size_t)encoder.frameNumber()) % 192u", output)
        self.assertIn(
            "kSpdifDmaDescriptorCount * (kSpdifDmaFramesPerDescriptor / 2u)",
            output,
        )
        self.assertIn("sendFrames(nullptr, frames, true)", output)

    def test_cleanup_preserves_only_requested_carrier(self):
        cleanup = function_body(
            PLAYER, "bool MediaPlayerPoC::serviceStopCleanup()"
        )
        self.assertIn(
            "if (!retainSpdifCarrierRequested) stopSpdif()", cleanup
        )

    def test_failed_transition_returns_to_normal_teardown(self):
        service = function_body(INO, "\nvoid serviceMediaTrackTransition()\n")
        self.assertIn("mediaPlayerPoc.requestStop()", service)
        self.assertIn("MediaTrackTransitionPhase::FailureCleanup", service)


if __name__ == "__main__":
    unittest.main()
