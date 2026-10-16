"""Guards against silently omitted caption characters and malformed native RLE.

Breaks caught: missing 'k' hiding behind a smaller measured caption; partial,
oversized or zero-sized alpha masks; out-of-screen native caption extents.
"""
import unittest
from dataclasses import replace
from NativeMenuAtlas import Glyph, SKETCH, caption_bounds, decode, read_atlas


class NativeMenuGlyphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.atlas = read_atlas()

    def test_caption_guard_rejects_missing_characters_or_measures_complete_text(self):
        for text in ['Speakers', 'Outputs', 'System', 'Effects', 'Remote']:
            with self.subTest(text=text):
                if set(text) - self.atlas.keys():
                    with self.assertRaisesRegex(ValueError, 'missing native menu glyph'):
                        caption_bounds(self.atlas, text)
                else:
                    left, right = caption_bounds(self.atlas, text)
                    self.assertGreater(right - left, 0)
                    self.assertLessEqual(right - left, 320)

    def test_missing_glyph_does_not_masquerade_as_a_narrow_caption(self):
        with self.assertRaisesRegex(ValueError, "missing native menu glyph 'k'"):
            caption_bounds({ch: glyph for ch, glyph in self.atlas.items() if ch != 'k'}, 'Speakers')

    def test_existing_caption_cells_fit_at_native_size(self):
        for text, width in [('System', 209), ('Effects', 191), ('Remote', 216), ('Outputs', 222), ('Input', 141), ('Music', 166), ('Preset', 177)]:
            with self.subTest(text=text):
                left, right = caption_bounds(self.atlas, text)
                self.assertEqual((left, right), (0, width))
                start = (320 - (right - left)) // 2 - left
                self.assertGreaterEqual(start + left, 0)
                self.assertLessEqual(start + right, 320)
                for ch in text:
                    glyph = self.atlas[ch]
                    self.assertGreaterEqual(80 + glyph.y, 0)
                    self.assertLessEqual(80 + glyph.y + glyph.height, 240)

    def test_all_stored_menu_bitmaps_decode_to_their_exact_cell_area(self):
        for ch, glyph in self.atlas.items():
            with self.subTest(ch=ch):
                pixels = decode(glyph)
                self.assertEqual(len(pixels), glyph.width * glyph.height)
                self.assertTrue(any(pixels))

    def test_reference_pixel_run_order_is_row_major(self):
        fixture = Glyph('x', 3, 2, 3, 0, 0, 6, (2, 0, 3, 255, 1, 17))
        self.assertEqual(decode(fixture), bytes([0, 0, 255, 255, 255, 17]))

    def test_malformed_bitmaps_are_rejected_before_rendering(self):
        valid = Glyph('x', 3, 2, 3, 0, 0, 6, (2, 0, 3, 255, 1, 17))
        invalid = {
            'odd length': replace(valid, data=(2, 0, 4), data_len=3),
            'metadata length': replace(valid, data_len=8),
            'zero run': replace(valid, data=(0, 0, 6, 255), data_len=4),
            'underrun': replace(valid, data=(5, 255), data_len=2),
            'overrun': replace(valid, data=(7, 255), data_len=2),
            'empty width': replace(valid, width=0),
            'empty height': replace(valid, height=0),
            'invalid alpha': replace(valid, data=(6, 256), data_len=2),
        }
        for defect, glyph in invalid.items():
            with self.subTest(defect=defect), self.assertRaises(ValueError):
                decode(glyph)


class CustomMenuKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.atlas = read_atlas()

    def test_custom_k_completes_speakers_without_scaled_fallback(self):
        self.assertTrue('k' in self.atlas, 'missing custom native k in production table/includes')
        self.assertEqual(caption_bounds(self.atlas, 'Speakers'), (0, 259))
        self.assertLessEqual(259, 304)

    def test_custom_k_reuses_existing_ascender_and_has_one_connected_body(self):
        self.assertTrue('k' in self.atlas, 'missing custom native k in production table/includes')
        glyph = self.atlas['k']
        self.assertEqual((glyph.width, glyph.height, glyph.advance, glyph.x, glyph.y), (32, 45, 32, 0, 16))
        pixels = decode(glyph)
        self.assertTrue(all(alpha % 17 == 0 for alpha in pixels))
        stem = decode(self.atlas['h'])
        for y in range(45):
            self.assertEqual(pixels[y*32:y*32+9], stem[:9])
        active = {(x, y) for y in range(45) for x in range(32) if pixels[y*32+x]}
        todo = [next(iter(active))]
        reached = set(todo)
        while todo:
            x, y = todo.pop()
            for dx, dy in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
                point = x+dx, y+dy
                if point in active and point not in reached:
                    todo.append(point)
                    reached.add(point)
        self.assertEqual(reached, active, 'detached accent, dot or antialias island')
        self.assertFalse(any(any(pixels[y*32+11:y*32+32]) for y in range(13)))
        self.assertTrue(any(pixels[13*32+20:13*32+30]))
        self.assertTrue(any(pixels[44*32+24:44*32+32]))


if __name__ == '__main__':
    unittest.main()
