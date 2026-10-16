"""User-authorized custom native k; NOT a recovered original font face.

Use the stored h's upper stem, repeated unscaled, and two native-size outlined
diagonals. 16x coverage sampling is only edge antialiasing of new vector geometry,
not enlargement of an existing smaller glyph. Quantize to the atlas's 4-bit alpha.
Print the generated header; apply_patch installs the output in the firmware.
"""
from PIL import Image, ImageDraw
from NativeMenuAtlas import decode, read_atlas

W, H, AA = 32, 45, 16
geometry = [
    [(9.4, 27.2), (22.0, 13.0), (30.5, 13.0), (14.8, 30.7), (9.4, 36.5)],
    [(13.5, 26.2), (18.9, 21.4), (31.0, 45.0), (23.5, 45.0)],
]
mask = Image.new('L', (W*AA, H*AA))
draw = ImageDraw.Draw(mask)
for polygon in geometry:
    draw.polygon([(round(x*AA), round(y*AA)) for x, y in polygon], fill=255)
mask = mask.resize((W, H), Image.Resampling.BOX)
pixels = [((alpha + 8) // 17) * 17 for alpha in mask.get_flattened_data()]
stem = decode(read_atlas()['h'])[:10]
for y in range(H):
    for x, alpha in enumerate(stem):
        pixels[y*W+x] = max(pixels[y*W+x], alpha)
encoded = []
for alpha in pixels:
    if encoded and encoded[-1] == alpha and encoded[-2] < 255:
        encoded[-2] += 1
    else:
        encoded.extend([1, alpha])
lines = ['#pragma once', '',
         '// Custom native-size k approved for Speakers; original font unavailable.',
         '// Stem: unscaled existing FontMenu h upper stem; diagonals: custom outline.',
         '// Cell 32x45, advance32, offsets0/16; 4-bit alpha in (count, alpha) RLE.',
         '// Generator/provenance: local-validation/Generate-NativeMenuK.py',
         'static const uint8_t FontMenu_6B[] PROGMEM = {']
for start in range(0, len(encoded), 24):
    lines.append('  ' + ', '.join(f'0x{value:02X}' for value in encoded[start:start+24]) + ',')
lines += ['};', f'static_assert(sizeof(FontMenu_6B) == {len(encoded)}, "Native menu k RLE length");', '']
print('\n'.join(lines))
