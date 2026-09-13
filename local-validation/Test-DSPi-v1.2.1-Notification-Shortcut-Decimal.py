"""Focused source contracts for notification routing and single-pass decimal rendering."""
import re
import unittest
from pathlib import Path

INO = (Path(__file__).resolve().parents[1] / 'firmware' /
       'DSPi_ESP32_Front_Panel_v1_1_2' /
       'DSPi_ESP32_Front_Panel_v1_1_2.ino').read_text(encoding='utf-8')


class NotificationAndDecimalContracts(unittest.TestCase):
    def test_home_notification_mapping_before_context_and_held_action(self):
        start = INO.index('UiAction action = actionForRemotePacket(packet);')
        end = INO.index('bleHeldAction = action;', start)
        routing = INO[start:end]
        self.assertIn('uiView == VIEW_HOME || uiView == VIEW_CHANGE_OVERLAY ||', routing)
        self.assertIn('(uiView == VIEW_FEATURE_CONFIRM && featureConfirmReturnView == VIEW_HOME)', routing)
        self.assertLess(routing.index('resolveHomeShortcut(action)'),
                        routing.index('contextualizeUiAction(action)'))

    def test_only_final_centred_pass_draws_decimal(self):
        calls = re.findall(r'^  drawVolumeTextCustom\((.*)\);$', INO, re.M)
        self.assertEqual(len(calls), 6)
        self.assertTrue(all(c.endswith('glow, false') for c in calls[:5]))
        self.assertEqual(calls[5], 'volX, y, value, uiMainText(), true')

    def test_decimal_spacing_and_theme_are_preserved(self):
        self.assertIn('if (drawDecimal) drawRoundDecimalPoint(x + 9, y + 67);', INO)
        self.assertIn('canvas->fillCircle(x, y, 4, glow);', INO)
        self.assertIn('canvas->fillCircle(x, y, 3, uiMainText());', INO)
        self.assertRegex(INO, r'if \(drawDecimal\) drawRoundDecimalPoint[^\n]+\n\s+x \+= 20;')


if __name__ == '__main__':
    unittest.main()
