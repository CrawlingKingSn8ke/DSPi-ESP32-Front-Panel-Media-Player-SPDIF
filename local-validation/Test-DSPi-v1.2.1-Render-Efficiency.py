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


def function_body(signature: str) -> str:
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
    raise AssertionError(f"unterminated function: {signature}")


GLYPH_RENDERERS = (
    "void drawGlyphRle(",
    "void drawGlyphRleClipped(",
    "void drawGlyphRleScaled(",
)


class RenderEfficiencyContracts(unittest.TestCase):
    def test_coordinate_walking_matches_linear_positions_for_every_glyph(self):
        arrays = {
            match.group(1): [
                int(value, 16)
                for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(2))
            ]
            for match in re.finditer(
                r"static const uint8_t (Font\w+)\[\] PROGMEM = \{(.*?)\};",
                INO,
                re.DOTALL,
            )
        }
        glyphs = re.findall(
            r"\{'[^']',\s*(\d+),\s*(\d+),.*?,\s*(Font\w+)\}", INO
        )
        self.assertEqual(230, len(glyphs))
        for width_text, height_text, array_name in glyphs:
            width = int(width_text)
            total = width * int(height_text)
            encoded = arrays[array_name]
            old_pixels = []
            new_pixels = []
            old_pos = 0
            new_pos = 0
            for offset in range(0, len(encoded), 2):
                count, alpha = encoded[offset : offset + 2]
                for _ in range(count):
                    if old_pos >= total:
                        break
                    if alpha:
                        old_pixels.append(
                            (old_pos % width, old_pos // width, alpha)
                        )
                    old_pos += 1

                if new_pos >= total:
                    continue
                if not alpha:
                    new_pos += count
                    continue
                source_x = new_pos % width
                source_y = new_pos // width
                for _ in range(count):
                    if new_pos >= total:
                        break
                    new_pixels.append((source_x, source_y, alpha))
                    new_pos += 1
                    source_x += 1
                    if source_x >= width:
                        source_x = 0
                        source_y += 1
            self.assertEqual(old_pixels, new_pixels, array_name)

    def test_transparent_rle_runs_skip_the_pixel_loop(self):
        for signature in GLYPH_RENDERERS:
            with self.subTest(function=signature):
                body = function_body(signature)
                self.assertIn("if (alpha == 0) {", body)
                transparent = body.index("if (alpha == 0) {")
                advance = body.index("pos += count;", transparent)
                resume = body.index("continue;", advance)
                pixel_loop = body.index(
                    "for (uint8_t n = 0; n < count; n++)"
                )
                self.assertLess(transparent, advance)
                self.assertLess(advance, resume)
                self.assertLess(resume, pixel_loop)

    def test_visible_rle_pixels_walk_coordinates_without_per_pixel_division(self):
        for signature in GLYPH_RENDERERS:
            with self.subTest(function=signature):
                body = function_body(signature)
                pixel_loop = body.index(
                    "for (uint8_t n = 0; n < count; n++)"
                )
                loop_body = body[pixel_loop:]
                self.assertIn("uint16_t sourceX = pos % g->w;", body[:pixel_loop])
                self.assertIn("uint16_t sourceY = pos / g->w;", body[:pixel_loop])
                self.assertNotIn("pos % g->w", loop_body)
                self.assertNotIn("pos / g->w", loop_body)
                self.assertIn("sourceX++;", loop_body)
                self.assertIn("if (sourceX >= g->w)", loop_body)
                self.assertIn("sourceY++;", loop_body)

    def test_album_art_reuses_precomputed_source_columns(self):
        body = function_body("void drawMediaArtwork(")
        self.assertIn("static uint16_t sourceColumns[UI_W];", body)
        precompute = body.index("static uint16_t sourceColumns[UI_W];")
        row_loop = body.index("for (int16_t dy = 0; dy < edge; dy++)")
        self.assertLess(precompute, row_loop)
        self.assertIn(
            "sourceColumns[dx] = sourceX + (uint32_t)dx * sourceEdge / edge;",
            body[:row_loop],
        )
        self.assertIn(
            "mediaArtworkPixels[(uint32_t)sy * mediaArtworkWidth + "
            "sourceColumns[dx]]",
            body[row_loop:],
        )
        self.assertNotIn("uint16_t sx =", body[row_loop:])

    def test_common_font_lookup_uses_verified_direct_indices(self):
        self.assertIn("int16_t commonGlyphIndex(", INO)
        indexer = function_body("int16_t commonGlyphIndex(")
        lookup = function_body("const GlyphDef *findGlyph(")
        for contract in (
            "ch - 'A'",
            "26 + ch - 'a'",
            "52 + ch - '0'",
            "case '.': return 62;",
            "case '-': return 63;",
            "case '/': return 64;",
        ):
            self.assertIn(contract, indexer)
        self.assertIn("&font == &FontSmall", lookup)
        self.assertIn("&font == &FontMedium", lookup)
        self.assertIn("&font == &FontLarge", lookup)
        self.assertIn("font.glyphs[index].ch == ch", lookup)
        self.assertIn("for (uint8_t i = 0; i < font.count; i++)", lookup)

    def test_direct_index_order_matches_all_three_common_font_tables(self):
        expected = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/"
        for font_name in ("FontSmall", "FontMedium", "FontLarge"):
            match = re.search(
                rf"static const GlyphDef {font_name}_glyphs\[\] = \{{(.*?)\}};",
                INO,
                re.DOTALL,
            )
            self.assertIsNotNone(match)
            characters = "".join(re.findall(r"\{'(.)',", match.group(1)))
            self.assertEqual(expected, characters, font_name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
