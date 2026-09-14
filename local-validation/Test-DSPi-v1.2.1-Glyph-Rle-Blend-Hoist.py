import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (
    ROOT
    / "firmware"
    / "DSPi_ESP32_Front_Panel_v1_1_2"
    / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
).read_text(encoding="utf-8")


def function_body(name: str) -> str:
    signature = f"void {name}("
    start = INO.index(signature)
    opening = INO.index("{", start)
    depth = 0
    for index in range(opening, len(INO)):
        if INO[index] == "{":
            depth += 1
        elif INO[index] == "}":
            depth -= 1
            if depth == 0:
                return INO[opening + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


class GlyphRleBlendHoistContracts(unittest.TestCase):
    def test_full_precision_blend_is_computed_once_before_each_pixel_run(self):
        for name in ("drawGlyphRle", "drawGlyphRleClipped", "drawGlyphRleScaled"):
            with self.subTest(function=name):
                body = function_body(name)
                blend = body.find(
                    "const uint16_t runColour = "
                    "blend565(C_BLACK, colour, alpha);"
                )
                transparent_skip = body.find("if (alpha == 0) {")
                pixel_loop = body.find("for (uint8_t n = 0; n < count; n++)")
                self.assertGreaterEqual(blend, 0)
                self.assertGreater(blend, transparent_skip)
                self.assertGreater(pixel_loop, blend)
                self.assertEqual(
                    body.count("blend565(C_BLACK, colour, alpha)"), 1
                )
                self.assertIn("canvas->drawPixel(px, py, runColour);", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
