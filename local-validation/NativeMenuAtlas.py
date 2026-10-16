"""Read-only reference decoder and caption guard for the embedded menu atlas."""
import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / 'firmware/DSPi_ESP32_Front_Panel_v1_1_2/DSPi_ESP32_Front_Panel_v1_1_2.ino'

@dataclass(frozen=True)
class Glyph:
    ch: str
    width: int
    height: int
    advance: int
    x: int
    y: int
    data_len: int
    data: tuple

def read_atlas(source=None, family='FontMenu'):
    source = SKETCH.read_text() if source is None else source
    # New glyph data can live in an included header while metadata stays in .ino.
    for header in re.findall(r'#include "([^"]+)"', source):
        path = SKETCH.parent / header
        if path.is_file() and path.suffix == '.h':
            source += '\n' + path.read_text()
    pattern = rf"\{{'(.)', (\d+), (\d+), (\d+), (-?\d+), (-?\d+), (\d+), ({family}_[0-9A-F]+)\}}"
    table = re.search(rf'static const GlyphDef {family}_glyphs\[\] = \{{(.*?)\n\}};', source, re.S)
    if table is None:
        raise ValueError(f'missing glyph table {family}')
    result = {}
    for match in re.finditer(pattern, table.group(1)):
        ch, *fields, name = match.groups()
        body = re.search(rf'{name}\[\] PROGMEM = \{{(.*?)\}};', source, re.S)
        if body is None:
            raise ValueError(f'missing bitmap {name}')
        data = tuple(int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', body.group(1)))
        result[ch] = Glyph(ch, *(int(field) for field in fields), data)
    return result

def decode(glyph):
    if glyph.width <= 0 or glyph.height <= 0:
        raise ValueError('empty bitmap extents')
    if glyph.data_len != len(glyph.data) or len(glyph.data) % 2:
        raise ValueError('invalid RLE data length')
    if any(not 1 <= n <= 255 for n in glyph.data[::2]):
        raise ValueError('invalid RLE run count')
    if any(not 0 <= a <= 255 for a in glyph.data[1::2]):
        raise ValueError('invalid RLE alpha')
    if sum(glyph.data[::2]) != glyph.width * glyph.height:
        raise ValueError('RLE area does not match bitmap extents')
    return bytes(a for n, a in zip(glyph.data[::2], glyph.data[1::2]) for _ in range(n))

def caption_bounds(atlas, text):
    cursor, left, right = 0, None, 0
    for ch in text:
        glyph = atlas.get(ch)
        if glyph is None:
            raise ValueError(f'missing native menu glyph {ch!r}')
        left = cursor + glyph.x if left is None else min(left, cursor + glyph.x)
        right = max(right, cursor + glyph.x + glyph.width)
        cursor += glyph.advance
    return (0 if left is None else left), right
