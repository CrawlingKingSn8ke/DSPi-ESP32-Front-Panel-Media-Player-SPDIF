import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "DSPi_ESP32_Front_Panel_v1_1_2.ino").read_text()
LAYOUT = (ROOT / "firmware" / "DSPi_ESP32_Front_Panel_v1_1_2" /
          "UiMenuLayout.h").read_text()


def body(signature):
    start = SKETCH.index(signature)
    opening = SKETCH.index("{", start)
    while ";" in SKETCH[start:opening]:
        start = SKETCH.index(signature, start + len(signature))
        opening = SKETCH.index("{", start)
    depth = 0
    for position in range(opening, len(SKETCH)):
        if SKETCH[position] == "{":
            depth += 1
        elif SKETCH[position] == "}":
            depth -= 1
            if depth == 0:
                return SKETCH[opening:position + 1]
    raise AssertionError(signature)


class UiPolishR2Contracts(unittest.TestCase):
    def test_remote_uses_vertical_navigation_and_shortcut_values_align(self):
        dispatch = body("void dispatchUiAction(UiAction action)")
        ble = dispatch[dispatch.index("if (uiView == VIEW_BLE)"):]
        ble = ble[:ble.index("if (uiView == VIEW_MEDIA_NOW_PLAYING)")]
        self.assertIn("action == ACT_NAV_UP", ble)
        self.assertIn("action == ACT_NAV_DOWN", ble)
        repeat = body("bool remoteActionRepeatsNavigation(UiAction action)")
        self.assertIn("action == ACT_NAV_UP || action == ACT_NAV_DOWN", repeat)
        row = body("void drawBleListRow(")
        self.assertIn("alignValueLeft", row)
        self.assertIn("drawFontText(FontMedium, 106", row)
        screen = body("void drawBleScreen()")
        self.assertIn("homeShortcuts[entry] != ACT_NONE, true", screen)
        self.assertIn("bleRemoveYes ? \"Remove\" : \"Cancel\"", screen)
        self.assertIn("drawFontCentredGlowColour(FontMedium, 150", screen)

    def test_system_order_and_volume_editor_are_consistent(self):
        names = body("String menuItemName(MenuPage page, uint8_t index)")
        self.assertIn('"Remote", "Volume Limit", "Screen Settings",', names)
        self.assertIn('"WiFi Transfer/Update", "Status"', names)
        values = body("String menuItemValue(MenuPage page, uint8_t index)")
        self.assertIn("if (index == 1) return String(dspi.masterVolumeDb", values)
        selection = body("void selectMenuItem()")
        self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 0", selection)
        self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 2", selection)
        self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 4", selection)
        for signature in ("String currentEditValue()", "void drawMenuValue(",
                          "void beginEdit()", "void adjustEdit(int direction)",
                          "void applyEdit()"):
            self.assertIn("menuPage == PAGE_SYSTEM && menuIndex == 1",
                          body(signature), signature)

    def test_dsp_setup_uses_native_font_and_icons_share_meter_colour(self):
        names = body("String menuItemName(MenuPage page, uint8_t index)")
        self.assertIn('"Input", "Music", "Preset", "DSP Setup", "System"', names)
        title = body("String pageTitle(MenuPage page)")
        self.assertIn('case PAGE_FEATURES: return "DSP Setup"', title)
        menu = body("void drawMenu()")
        self.assertIn("drawMenuTextNative(80, selected, uiMainText())", menu)
        self.assertNotIn("drawFontTextScaledKerned(FontMenu", menu)
        home = body("void drawTopStatus()")
        self.assertIn("uiVolumeMeterColour()", home)
        self.assertIn("sourceRight", home)
        self.assertIn("presetLeft", home)
        self.assertIn("const int16_t iconY = 20", home)
        self.assertIn("uiFeatureIconScaledSpan(", home)
        features = body("void drawSystemSettingsList()")
        self.assertIn("menuPage == PAGE_FEATURES", features)
        self.assertIn("drawFeatureIconForIndex(", features)
        icon = body("void drawFeatureIconForIndex(")
        self.assertIn("uiVolumeMeterColour()", icon)

    def test_preset_hint_and_final_page_up_arrow(self):
        self.assertIn("uiShowUpArrow", LAYOUT)
        preset = body("void drawPresetList()")
        self.assertIn('drawFontCentredGlowColour(FontMedium, 186,', preset)
        self.assertIn('"Hold Select to save"', preset)
        self.assertIn("drawListUpArrow(", preset)
        for signature in ("void drawSystemSettingsList()", "void drawBleScreen()"):
            self.assertIn("drawListUpArrow(", body(signature), signature)


if __name__ == "__main__":
    unittest.main()
