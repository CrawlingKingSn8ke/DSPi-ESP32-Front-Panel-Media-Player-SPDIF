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
  assert(uiFeatureIconSpan(UI_FEATURE_ALL) == 99);

  assert(uiAnalogSwatchX(0) == 65);
  assert(uiAnalogSwatchX(5) == 255);

  std::puts("UI readability layout policy passed");
}
