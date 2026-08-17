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
        self.assertIn("SpdifCarrierRetention::Ready", start)
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
            "while (!stopIsRequested() && state != MediaPlaybackState::Error)",
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
        self.assertIn("sendFrames(nullptr, frames)", output)
        self.assertIn("publishRetainedSpdifCarrier()", output)

    def test_cleanup_preserves_only_proven_ready_carrier(self):
        cleanup = function_body(
            PLAYER, "bool MediaPlayerPoC::serviceStopCleanup()"
        )
        self.assertIn(
            "spdifCarrierRetention() != SpdifCarrierRetention::Ready", cleanup
        )

    def test_genuine_stop_disables_tx_before_slow_decoder_cleanup(self):
        cleanup = function_body(
            PLAYER, "bool MediaPlayerPoC::serviceStopCleanup()"
        )
        output_wait = cleanup.index("if (outputTaskRunning()) return false")
        teardown = cleanup.index("stopSpdif()")
        decoder_wait = cleanup.index("if (decoderTaskRunning()) return false")
        self.assertLess(output_wait, teardown)
        self.assertLess(teardown, decoder_wait)

    def test_retention_has_atomic_requested_and_ready_states(self):
        self.assertIn("enum class SpdifCarrierRetention", HEADER)
        self.assertIn("Requested", HEADER)
        self.assertIn("Ready", HEADER)
        self.assertIn("Faulted", HEADER)
        publish = function_body(
            PLAYER, "bool MediaPlayerPoC::publishRetainedSpdifCarrier()"
        )
        self.assertIn("__atomic_compare_exchange_n", publish)
        self.assertIn("SpdifCarrierRetention::Requested", publish)
        self.assertIn("SpdifCarrierRetention::Ready", publish)

    def test_inflight_retained_write_is_finished_but_stop_can_cancel(self):
        output = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        send = function_body(output, "auto sendFrames")
        self.assertIn("while (remaining)", send)
        self.assertIn("stopIsRequested()", send)
        self.assertIn("SpdifCarrierRetention::Requested", send)
        self.assertNotIn("allowWhileStopping", send)
        self.assertIn("next += written", send)
        self.assertIn("remaining -= written", send)

    def test_output_errors_invalidate_retention(self):
        error = function_body(PLAYER, "void MediaPlayerPoC::setError")
        self.assertIn("SpdifCarrierRetention::Faulted", error)
        output = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        self.assertIn("S/PDIF DMA write failed", output)
        self.assertIn("setStopRequested(true)", output)

    def test_fault_or_ordinary_stop_cannot_be_resurrected_as_retention(self):
        request = function_body(
            PLAYER, "void MediaPlayerPoC::requestStopInternal"
        )
        self.assertIn("const bool alreadyStopping = stopIsRequested()", request)
        self.assertIn("!alreadyStopping", request)
        self.assertIn("state != MediaPlaybackState::Error", request)
        self.assertIn("SpdifCarrierRetention::Off", request)
        self.assertIn("SpdifCarrierRetention::Requested", request)

    def test_early_setup_failure_tears_down_retained_carrier(self):
        begin = function_body(PLAYER, "bool MediaPlayerPoC::beginPlay")
        queue_failure = begin.index("media control queues unavailable")
        teardown = begin.index("stopSpdif()", queue_failure)
        rejection = begin.index("MEDIA PLAY: rejected", queue_failure)
        self.assertLess(queue_failure, teardown)
        self.assertLess(teardown, rejection)

    def test_task_exit_is_release_published_and_cleanup_acquire_loads(self):
        output = function_body(PLAYER, "void MediaPlayerPoC::outputTask()")
        decoder = function_body(PLAYER, "void MediaPlayerPoC::decoderTask()")
        self.assertIn("__atomic_store_n(&outputTaskHandle", output)
        self.assertIn("__ATOMIC_RELEASE", output)
        self.assertIn("__atomic_store_n(&decoderTaskHandle", decoder)
        running = function_body(
            PLAYER, "bool MediaPlayerPoC::outputTaskRunning() const"
        )
        self.assertIn("__ATOMIC_ACQUIRE", running)

    def test_genuine_ui_stop_is_idempotent_even_when_state_is_stopped(self):
        stop = function_body(INO, "\nvoid stopMediaPlayback(const char *reason)\n")
        park = function_body(INO, "\nvoid parkMediaPlayback(const char *reason)\n")
        for body in (stop, park):
            self.assertIn("mediaPlayerPoc.stop()", body)
            conditional = body.index(
                "mediaPlayerPoc.playbackState() != MediaPlaybackState::Stopped"
            )
            call = body.rindex("mediaPlayerPoc.stop()")
            self.assertGreater(call, conditional)

    def test_failed_transition_returns_to_normal_teardown(self):
        service = function_body(INO, "\nvoid serviceMediaTrackTransition()\n")
        self.assertIn("mediaPlayerPoc.requestStop()", service)
        self.assertIn("MediaTrackTransitionPhase::FailureCleanup", service)


if __name__ == "__main__":
    unittest.main()
