#include <cassert>
#include <cstdio>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h"

int main()
{
  const UiListColumns shortValue = uiListColumns(320, 28);
  assert(shortValue.labelX == 13);
  assert(shortValue.valueRight == 307);
  assert(shortValue.gap == 8);
  assert(shortValue.labelWidth + shortValue.valueWidth + shortValue.gap == 294);

  const UiListColumns longValue = uiListColumns(320, 220);
  assert(longValue.valueWidth == 140);
  assert(longValue.labelWidth == 146);
  assert(longValue.labelWidth + longValue.valueWidth + longValue.gap == 294);

  assert(uiFeatureIconWidth(UI_FEATURE_PSY_BASS) == 16);
  assert(uiFeatureIconWidth(UI_FEATURE_SUB_SYNTH) == 20);
  assert(uiFeatureIconOffset(UI_FEATURE_PSY_BASS | UI_FEATURE_SUB_SYNTH,
                             UI_FEATURE_SUB_SYNTH) == 21);
  assert(uiFeatureIconSpan(UI_FEATURE_PSY_BASS | UI_FEATURE_SUB_SYNTH) == 41);
  assert(uiFeatureIconSpan(UI_FEATURE_ALL) == 138);
  assert(uiFeatureIconScaledSpan(UI_FEATURE_ALL, 125, 7) == 178);
  // Every scaled icon and inter-icon gap must sit inside the combined span.
  for (uint8_t bit = 1; bit <= UI_FEATURE_LIMITER; bit <<= 1) {
    const int16_t start = uiFeatureIconScaledOffset(UI_FEATURE_ALL, bit, 125, 7);
    const int16_t end = start + uiFeatureIconScaledWidth(bit, 125);
    assert(start >= 0 && end <= 178);
    if (bit < UI_FEATURE_LIMITER) {
      const int16_t next = uiFeatureIconScaledOffset(
          UI_FEATURE_ALL, static_cast<uint8_t>(bit << 1), 125, 7);
      assert(next - end == 7);
    }
  }

  assert(uiAnalogSwatchX(0) == 65);
  assert(uiAnalogSwatchX(5) == 255);

  std::puts("UI readability layout policy passed");
}
