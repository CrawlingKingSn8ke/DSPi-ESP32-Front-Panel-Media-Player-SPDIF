import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (
    ROOT
    / "firmware"
    / "DSPi_ESP32_Front_Panel_v1_1_2"
    / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
).read_text(encoding="utf-8")


def define_value(name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", INO, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing numeric define: {name}")
    return int(match.group(1))


class EncoderPhysicalDetentContracts(unittest.TestCase):
    def test_one_physical_detent_is_four_quadrature_edges(self):
        self.assertEqual(define_value("ENCODER_COUNTS_PER_DETENT"), 4)

    def test_menu_and_edit_consume_one_physical_detent_per_step(self):
        self.assertEqual(define_value("ENCODER_MENU_DETENTS_PER_STEP"), 1)

    def test_home_volume_remains_one_db_per_physical_detent(self):
        self.assertIn("changeVolume((float)detents * 1.0f)", INO)


if __name__ == "__main__":
    unittest.main(verbosity=2)
