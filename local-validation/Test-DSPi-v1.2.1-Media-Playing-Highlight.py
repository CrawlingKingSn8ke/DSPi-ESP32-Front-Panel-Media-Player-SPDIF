import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INO = (
    ROOT
    / "firmware"
    / "DSPi_ESP32_Front_Panel_v1_1_2"
    / "DSPi_ESP32_Front_Panel_v1_1_2.ino"
).read_text()


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


class MediaPlayingHighlightContracts(unittest.TestCase):
    def test_playing_file_uses_exact_path_identity(self):
        body = function_body(INO, "bool mediaBrowserItemIsPlaying(")
        self.assertIn("strcmp(entry.path, mediaCurrentPath) == 0", body)

    def test_containing_folder_requires_a_path_boundary(self):
        body = function_body(INO, "bool mediaBrowserItemIsPlaying(")
        self.assertIn(
            "strncmp(entry.path, mediaCurrentPath, directoryLength) == 0", body
        )
        self.assertIn("mediaCurrentPath[directoryLength] == '/'", body)

    def test_highlight_is_render_only_and_uses_live_accent_role(self):
        body = function_body(INO, "void drawMediaBrowserRow(")
        self.assertIn("mediaBrowserItemIsPlaying(itemIndex)", body)
        self.assertIn("? uiAccent() : uiMainText()", body)
        self.assertIn("drawMediaBrowserIcon(itemIndex, 10, rowY + 7, rowColour)", body)
        self.assertGreaterEqual(body.count("rowColour"), 5)

    def test_navigation_cursor_remains_an_independent_marker(self):
        body = function_body(INO, "void drawMediaBrowserRow(")
        self.assertIn("if (selected)", body)
        self.assertIn("canvas->fillRect(4, rowY, 3, rowH, uiMainText())", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
