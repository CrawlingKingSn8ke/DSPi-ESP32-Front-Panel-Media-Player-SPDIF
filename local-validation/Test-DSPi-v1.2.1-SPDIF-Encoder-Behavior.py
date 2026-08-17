import random
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2"
SOURCE = (FIRMWARE / "SpdifBlockEncoder.cpp").read_text()


def array_values(declaration: str):
    start = SOURCE.index(declaration)
    brace = SOURCE.index("{", start)
    end = SOURCE.index("};", brace)
    return [int(value, 16) for value in re.findall(
        r"0x[0-9a-fA-F]+", SOURCE[brace:end]
    )]


def constant_value(name: str):
    match = re.search(rf"{name}\s*=\s*0x([0-9a-fA-F]+)u", SOURCE)
    if not match:
        raise AssertionError(f"missing production constant {name}")
    return int(match.group(1), 16)


BMC = array_values("kBmcLookup[256]")
VUCP_VALUES = array_values("kVucpByPhaseAndChannelStatus[2][2]")
VUCP = [VUCP_VALUES[:2], VUCP_VALUES[2:]]
PREAMBLES = {
    "B": constant_value("kPreambleB"),
    "M": constant_value("kPreambleM"),
    "W": constant_value("kPreambleW"),
}
STATUS = {
    44_100: array_values("kConsumerChannelStatus44100[5]"),
    48_000: array_values("kConsumerChannelStatus48000[5]"),
}
EXPECTED_PREAMBLES = {"B": 0xE8, "M": 0xE2, "W": 0xE4}
EXPECTED_STATUS = {
    44_100: [0x04, 0x00, 0x00, 0x00, 0x0B],
    48_000: [0x04, 0x00, 0x00, 0x02, 0x0B],
}


def lsb_first_bits_to_int(bits):
    return sum(bit << index for index, bit in enumerate(bits))


def decode_bmc(encoded: int, width: int, previous_level: int):
    wire = [(encoded >> shift) & 1 for shift in range(width - 1, -1, -1)]
    decoded = []
    for offset in range(0, width, 2):
        first, second = wire[offset:offset + 2]
        if first == previous_level:
            raise AssertionError("missing mandatory BMC boundary transition")
        decoded.append(first ^ second)
        previous_level = second
    return decoded, previous_level


class EncoderModel:
    """Reference model using parsed constants plus guarded production layout."""

    def __init__(self, sample_rate):
        self.status = STATUS[sample_rate]
        self.frame = 0
        self.vucp = VUCP[0][0]

    def _channel(self, sample, preamble, channel_status):
        sample &= 0xFFFF_FFFF
        high = BMC[(sample >> 24) & 0xFF]
        middle = BMC[(sample >> 16) & 0xFF]
        auxiliary = BMC[(sample >> 8) & 0xFF]
        if auxiliary & 1:
            middle = (~middle) & 0xFFFF
        if middle & 1:
            high = (~high) & 0xFFFF
        words = [
            (self.vucp << 24) | (preamble << 16) | auxiliary,
            (middle << 16) | high,
        ]
        self.vucp = VUCP[high & 1][channel_status & 1]
        return words

    def encode(self, stereo_frames):
        words = []
        for left, right in stereo_frames:
            channel_status = (
                (self.status[self.frame // 8] >> (self.frame % 8)) & 1
                if self.frame < 40 else 0
            )
            words.extend(self._channel(
                left,
                PREAMBLES["B"] if self.frame == 0 else PREAMBLES["M"],
                channel_status,
            ))
            words.extend(self._channel(right, PREAMBLES["W"], channel_status))
            self.frame = (self.frame + 1) % 192
        return words


def decode_subframe(words, subframe_index):
    word0 = words[subframe_index * 2]
    word1 = words[subframe_index * 2 + 1]
    next_word0 = words[(subframe_index + 1) * 2]
    preamble = (word0 >> 16) & 0xFF

    auxiliary_bits, phase = decode_bmc(word0 & 0xFFFF, 16, preamble & 1)
    middle_bits, phase = decode_bmc((word1 >> 16) & 0xFFFF, 16, phase)
    high_bits, phase = decode_bmc(word1 & 0xFFFF, 16, phase)
    flag_bits, phase = decode_bmc((next_word0 >> 24) & 0xFF, 8, phase)
    if phase != 0:
        raise AssertionError("VUCP field did not end at the required zero phase")
    return {
        "preamble": preamble,
        "audio": (
            lsb_first_bits_to_int(auxiliary_bits)
            | (lsb_first_bits_to_int(middle_bits) << 8)
            | (lsb_first_bits_to_int(high_bits) << 16)
        ),
        "valid": flag_bits[0],
        "user": flag_bits[1],
        "channel_status": flag_bits[2],
        "parity": flag_bits[3],
    }


def expected_status_bit(status, frame):
    return (status[frame // 8] >> (frame % 8)) & 1 if frame < 40 else 0


class SpdifEncoderBehaviour(unittest.TestCase):
    def setUp(self):
        self.assertEqual(256, len(BMC))
        self.assertEqual(EXPECTED_PREAMBLES, PREAMBLES)
        self.assertEqual(EXPECTED_STATUS, STATUS)
        # Guard the production algorithm/layout that the reference model mirrors.
        for contract in (
            "static_cast<uint32_t>(sample) >> 24",
            "static_cast<uint32_t>(sample) >> 16",
            "static_cast<uint32_t>(sample) >> 8",
            "if (auxiliary & 1u)",
            "if (low & 1u)",
            "static_cast<uint32_t>(vucp) << 24",
            "static_cast<uint32_t>(preamble) << 16",
            "static_cast<uint32_t>(low) << 16",
            "vucp = kVucpByPhaseAndChannelStatus",
            "frameNumber_ == 0 ? kPreambleB : kPreambleM",
            "encodeChannel24(right, kPreambleW",
            "frameNumber_ >= 192",
        ):
            self.assertIn(contract, SOURCE)

    def test_complete_blocks_decode_at_both_rates(self):
        randomizer = random.Random(0x60958)
        frames = [
            (randomizer.randrange(-(1 << 31), 1 << 31),
             randomizer.randrange(-(1 << 31), 1 << 31))
            for _ in range(193)
        ]
        for rate in (44_100, 48_000):
            words = EncoderModel(rate).encode(frames)
            decoded = [decode_subframe(words, index) for index in range(384)]
            for frame in range(192):
                left = decoded[frame * 2]
                right = decoded[frame * 2 + 1]
                self.assertEqual(
                    PREAMBLES["B"] if frame == 0 else PREAMBLES["M"],
                    left["preamble"],
                )
                self.assertEqual(PREAMBLES["W"], right["preamble"])
                self.assertEqual((frames[frame][0] & 0xFFFF_FFFF) >> 8,
                                 left["audio"])
                self.assertEqual((frames[frame][1] & 0xFFFF_FFFF) >> 8,
                                 right["audio"])
                expected_c = expected_status_bit(EXPECTED_STATUS[rate], frame)
                for subframe in (left, right):
                    self.assertEqual(0, subframe["valid"])
                    self.assertEqual(0, subframe["user"])
                    self.assertEqual(expected_c, subframe["channel_status"])
                    total_ones = (
                        subframe["audio"].bit_count()
                        + subframe["valid"]
                        + subframe["user"]
                        + subframe["channel_status"]
                        + subframe["parity"]
                    )
                    self.assertEqual(0, total_ones & 1)

    def test_channel_status_reconstructs_exact_consumer_bytes(self):
        silence = [(0, 0)] * 193
        for rate in (44_100, 48_000):
            words = EncoderModel(rate).encode(silence)
            decoded = [decode_subframe(words, index) for index in range(384)]
            for channel in (0, 1):
                bits = [decoded[frame * 2 + channel]["channel_status"]
                        for frame in range(192)]
                reconstructed = [
                    sum(bits[byte * 8 + bit] << bit for bit in range(8))
                    for byte in range(24)
                ]
                self.assertEqual(EXPECTED_STATUS[rate], reconstructed[:5])
                self.assertEqual([0] * 19, reconstructed[5:])

    def test_block_wrap_emits_b_only_at_frame_zero(self):
        model = EncoderModel(44_100)
        words = model.encode([(0, 0)] * 385)
        left_preambles = [
            (words[frame * 4] >> 16) & 0xFF for frame in range(385)
        ]
        self.assertEqual(PREAMBLES["B"], left_preambles[0])
        self.assertEqual(PREAMBLES["B"], left_preambles[192])
        self.assertEqual(PREAMBLES["B"], left_preambles[384])
        self.assertTrue(all(
            preamble == PREAMBLES["M"]
            for index, preamble in enumerate(left_preambles)
            if index % 192
        ))
        self.assertEqual(1, model.frame)

    def test_partitioned_calls_are_byte_for_byte_identical(self):
        frames = [
            (((index * 0x1020304) ^ 0x89ABCDEF) & 0xFFFF_FFFF,
             ((index * 0x7654321) ^ 0x12345678) & 0xFFFF_FFFF)
            for index in range(385)
        ]
        for rate in (44_100, 48_000):
            whole_model = EncoderModel(rate)
            whole = whole_model.encode(frames)
            split_model = EncoderModel(rate)
            split = []
            offset = 0
            for count in (1, 2, 7, 31, 151, 1, 192):
                split.extend(split_model.encode(frames[offset:offset + count]))
                offset += count
            self.assertEqual(len(frames), offset)
            self.assertEqual(whole, split)
            self.assertEqual(whole_model.frame, split_model.frame)
            self.assertEqual(whole_model.vucp, split_model.vucp)

    def test_bottom_pcm_byte_is_intentionally_ignored(self):
        vectors = [
            (0x00000000, 0x000000FF),
            (0x7FFFFF00, 0x7FFFFFFF),
            (0x80000000, 0x800000A5),
            (0xFFFFFF00, 0xFFFFFFFF),
            (0x12345600, 0x12345678),
            (0x89ABCD00, 0x89ABCDEF),
        ]
        for first, second in vectors:
            a = EncoderModel(48_000).encode([(first, first)])
            b = EncoderModel(48_000).encode([(second, second)])
            self.assertEqual(a, b)

    def test_all_lookup_entries_decode_with_continuous_boundaries(self):
        for value, encoded in enumerate(BMC):
            bits, final_level = decode_bmc(encoded, 16, 0)
            self.assertEqual(value, lsb_first_bits_to_int(bits))
            complemented = (~encoded) & 0xFFFF
            bits, complemented_final = decode_bmc(complemented, 16, 1)
            self.assertEqual(value, lsb_first_bits_to_int(bits))
            self.assertEqual(final_level ^ 1, complemented_final)


if __name__ == "__main__":
    unittest.main(verbosity=2)
