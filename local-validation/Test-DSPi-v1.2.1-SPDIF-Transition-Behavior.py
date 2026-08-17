import enum
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
PLAYER = (FIRMWARE / "MediaPlayerPoC.cpp").read_text()
HEADER = (FIRMWARE / "MediaPlayerPoC.h").read_text()
INO = (FIRMWARE / "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()


class Retention(enum.Enum):
    OFF = 0
    REQUESTED = 1
    READY = 2
    FAULTED = 3


class ScriptedWriter:
    def __init__(self, script, after_call=None):
        self.script = list(script)
        self.after_call = after_call
        self.calls = 0
        self.wire = bytearray()

    def write(self, payload):
        result, written = self.script.pop(0) if self.script else ("ok", len(payload))
        written = min(written, len(payload))
        self.wire.extend(payload[:written])
        self.calls += 1
        if self.after_call:
            self.after_call(self.calls)
        return result, written


class TransitionModel:
    BYTES_PER_FRAME = 16
    BLOCK_FRAMES = 192
    RING_FRAMES = 16 * (384 // 2)

    def __init__(self):
        self.stop = False
        self.retention = Retention.OFF
        self.encoder_frame = 0
        self.next_encoded_byte = 0
        self.partial_writes = 0
        self.errors = 0

    def encode(self, frames):
        length = frames * self.BYTES_PER_FRAME
        payload = bytes(
            (self.next_encoded_byte + offset) & 0xFF for offset in range(length)
        )
        self.next_encoded_byte += length
        self.encoder_frame = (self.encoder_frame + frames) % self.BLOCK_FRAMES
        return payload

    def request_stop(self, retain):
        already_stopping = self.stop
        if not retain:
            self.retention = Retention.OFF
        elif not already_stopping and self.retention is Retention.OFF:
            self.retention = Retention.REQUESTED
        self.stop = True

    def send_frames(self, frames, writer):
        payload = self.encode(frames)
        offset = 0
        while offset < len(payload):
            if self.stop and self.retention is not Retention.REQUESTED:
                return False
            requested = len(payload) - offset
            result, written = writer.write(payload[offset:])
            if 0 < written < requested:
                self.partial_writes += 1
            offset += written
            if result == "timeout":
                continue
            if result != "ok" or (written == 0 and offset < len(payload)):
                self.errors += 1
                self.retention = Retention.FAULTED
                self.stop = True
                return False
        return True

    def retain_ring(self, writer):
        if not self.stop or self.retention is not Retention.REQUESTED:
            return False
        remainder = (-self.encoder_frame) % self.BLOCK_FRAMES
        if remainder and not self.send_frames(remainder, writer):
            return False
        left = self.RING_FRAMES
        while left:
            frames = min(left, 256)
            if not self.send_frames(frames, writer):
                return False
            left -= frames
        if self.retention is not Retention.REQUESTED:
            return False
        self.retention = Retention.READY
        return True


class SpdifTransitionBehaviour(unittest.TestCase):
    def setUp(self):
        # Tie the executable policy model to the production control structure.
        for contract in (
            "while (remaining)",
            "written < requested",
            "next += written",
            "remaining -= written",
            "SpdifCarrierRetention::Requested",
            "publishRetainedSpdifCarrier()",
            "SpdifCarrierRetention::Ready",
            "kSpdifDmaDescriptorCount * (kSpdifDmaFramesPerDescriptor / 2u)",
        ):
            self.assertIn(contract, PLAYER + HEADER)

    def test_partial_timeout_then_transition_stop_finishes_exact_bytes(self):
        model = TransitionModel()
        uninterrupted = model.encode(256)

        model = TransitionModel()
        def request_retention(call):
            if call == 1:
                model.retention = Retention.REQUESTED
                model.stop = True

        writer = ScriptedWriter(
            [("timeout", 100 * model.BYTES_PER_FRAME), ("ok", 10_000)],
            request_retention,
        )
        self.assertTrue(model.send_frames(256, writer))
        self.assertEqual(uninterrupted, bytes(writer.wire))
        self.assertEqual(1, model.partial_writes)
        self.assertEqual(64, model.encoder_frame)

    def test_partial_timeout_then_retry_without_stop_is_byte_exact(self):
        reference = TransitionModel()
        uninterrupted = reference.encode(256)

        model = TransitionModel()
        writer = ScriptedWriter(
            [("timeout", 73 * model.BYTES_PER_FRAME), ("ok", 10_000)]
        )
        self.assertTrue(model.send_frames(256, writer))
        self.assertEqual(uninterrupted, bytes(writer.wire))
        self.assertEqual(2, writer.calls)
        self.assertEqual(1, model.partial_writes)
        self.assertEqual(0, model.errors)

    def test_success_with_partial_prefix_continues_without_duplication(self):
        reference = TransitionModel()
        uninterrupted = reference.encode(256)

        model = TransitionModel()
        writer = ScriptedWriter(
            [("ok", 91 * model.BYTES_PER_FRAME), ("ok", 10_000)]
        )
        self.assertTrue(model.send_frames(256, writer))
        self.assertEqual(uninterrupted, bytes(writer.wire))
        self.assertEqual(2, writer.calls)
        self.assertEqual(1, model.partial_writes)
        self.assertEqual(0, model.errors)

    def test_stop_before_first_byte_still_finishes_only_for_retention(self):
        retained = TransitionModel()
        retained.stop = True
        retained.retention = Retention.REQUESTED
        retained_writer = ScriptedWriter([])
        self.assertTrue(retained.send_frames(256, retained_writer))
        self.assertEqual(256 * retained.BYTES_PER_FRAME,
                         len(retained_writer.wire))

        ordinary = TransitionModel()
        ordinary.stop = True
        ordinary.retention = Retention.OFF
        ordinary_writer = ScriptedWriter([])
        self.assertFalse(ordinary.send_frames(256, ordinary_writer))
        self.assertEqual(b"", ordinary_writer.wire)

    def test_alignment_and_complete_ring_end_at_frame_zero(self):
        model = TransitionModel()
        model.encoder_frame = 64
        model.stop = True
        model.retention = Retention.REQUESTED
        writer = ScriptedWriter([])
        self.assertTrue(model.retain_ring(writer))
        self.assertEqual(Retention.READY, model.retention)
        self.assertEqual(0, model.encoder_frame)
        expected_frames = 128 + model.RING_FRAMES
        self.assertEqual(expected_frames * model.BYTES_PER_FRAME,
                         len(writer.wire))
        self.assertEqual(3_072, model.RING_FRAMES)

    def test_ordinary_stop_cancels_repeated_retention_timeouts(self):
        model = TransitionModel()
        model.stop = True
        model.retention = Retention.REQUESTED

        def cancel_retention(call):
            if call == 2:
                model.retention = Retention.OFF

        writer = ScriptedWriter(
            [("timeout", 0), ("timeout", 0), ("timeout", 0)],
            cancel_retention,
        )
        self.assertFalse(model.send_frames(192, writer))
        self.assertEqual(2, writer.calls)
        self.assertEqual(Retention.OFF, model.retention)

    def test_ui_transition_timeout_escalates_to_ordinary_stop(self):
        self.assertIn("#define MEDIA_TRANSITION_TIMEOUT_MS 12000", INO)
        start = INO.index("\nvoid serviceMediaTrackTransition()\n{")
        service = INO[start:INO.index("\n}\n", start) + 3]
        timeout = service.index("MEDIA_TRANSITION_TIMEOUT_MS")
        ordinary_stop = service.index("mediaPlayerPoc.requestStop();", timeout)
        failure_cleanup = service.index(
            "MediaTrackTransitionPhase::FailureCleanup", timeout
        )
        self.assertLess(timeout, ordinary_stop)
        self.assertLess(ordinary_stop, failure_cleanup)

    def test_hard_write_error_cannot_publish_ready(self):
        model = TransitionModel()
        model.stop = True
        model.retention = Retention.REQUESTED
        writer = ScriptedWriter([("error", 64)])
        self.assertFalse(model.retain_ring(writer))
        self.assertEqual(Retention.FAULTED, model.retention)
        self.assertEqual(1, model.errors)

    def test_fault_and_ordinary_stop_cannot_be_resurrected(self):
        ordinary = TransitionModel()
        ordinary.request_stop(retain=False)
        ordinary.request_stop(retain=True)
        self.assertEqual(Retention.OFF, ordinary.retention)

        faulted = TransitionModel()
        faulted.retention = Retention.FAULTED
        faulted.stop = True
        faulted.request_stop(retain=True)
        self.assertEqual(Retention.FAULTED, faulted.retention)

        requested = TransitionModel()
        requested.request_stop(retain=True)
        requested.request_stop(retain=True)
        self.assertEqual(Retention.REQUESTED, requested.retention)

    def test_zero_progress_success_is_terminal_not_an_infinite_loop(self):
        model = TransitionModel()
        writer = ScriptedWriter([("ok", 0)])
        self.assertFalse(model.send_frames(1, writer))
        self.assertEqual(1, writer.calls)
        self.assertEqual(1, model.errors)

    def test_rate_mismatch_reaches_teardown_before_new_channel(self):
        start = PLAYER[PLAYER.index("bool MediaPlayerPoC::startSpdif"):]
        start = start[:start.index("void MediaPlayerPoC::stopSpdif")]
        reuse_rate = start.index("spdifSampleRate == currentFile.sampleRate")
        teardown = start.index("stopSpdif()")
        create = start.index("i2s_new_channel")
        self.assertLess(reuse_rate, teardown)
        self.assertLess(teardown, create)

    def test_rejected_route_and_ui_stop_have_teardown_paths(self):
        self.assertIn("DSPi S/PDIF route verification failed", PLAYER)
        route_error = PLAYER.index("DSPi S/PDIF route verification failed")
        self.assertIn("stopSpdif()", PLAYER[route_error - 500:route_error])
        for signature in (
            "\nvoid stopMediaPlayback(const char *reason)\n{",
            "\nvoid parkMediaPlayback(const char *reason)\n{",
        ):
            start = INO.index(signature)
            body = INO[start:INO.index("\n}\n", start) + 3]
            self.assertIn("mediaPlayerPoc.stop()", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
