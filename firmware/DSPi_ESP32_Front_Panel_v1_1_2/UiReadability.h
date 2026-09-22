#pragma once

#include <cstdint>

struct UiListColumns {
  int16_t labelX;
  int16_t labelWidth;
  int16_t valueWidth;
  int16_t valueRight;
  int16_t gap;
};

constexpr int16_t uiMin16(int16_t a, int16_t b)
{
  return a < b ? a : b;
}

constexpr UiListColumns uiListColumns(int16_t screenWidth,
                                      int16_t requestedValueWidth)
{
  return UiListColumns{
    13,
    static_cast<int16_t>(screenWidth - 26 - 8 -
                         uiMin16(requestedValueWidth, 140)),
    uiMin16(requestedValueWidth, 140),
    static_cast<int16_t>(screenWidth - 13),
    8
  };
}

enum UiFeatureIcon : uint8_t {
  UI_FEATURE_LOUDNESS = 1u << 0,
  UI_FEATURE_CROSSFEED = 1u << 1,
  UI_FEATURE_LEVELLER = 1u << 2,
  UI_FEATURE_PSY_BASS = 1u << 3,
  UI_FEATURE_SUB_SYNTH = 1u << 4,
  UI_FEATURE_ALL = 0x1f
};

constexpr uint8_t uiFeatureIconWidth(uint8_t feature)
{
  return feature == UI_FEATURE_LOUDNESS ? 11 :
         feature == UI_FEATURE_CROSSFEED ? 15 :
         feature == UI_FEATURE_LEVELLER ? 17 :
         feature == UI_FEATURE_PSY_BASS ? 16 :
         feature == UI_FEATURE_SUB_SYNTH ? 20 : 0;
}

constexpr int16_t uiFeatureIconSpan(uint8_t activeMask)
{
  int16_t span = 0;
  uint8_t count = 0;
  for (uint8_t bit = 1; bit <= UI_FEATURE_SUB_SYNTH; bit <<= 1) {
    if (!(activeMask & bit)) continue;
    span += uiFeatureIconWidth(bit);
    ++count;
  }
  return count ? static_cast<int16_t>(span + (count - 1) * 5) : 0;
}

constexpr int16_t uiFeatureIconOffset(uint8_t activeMask, uint8_t target)
{
  int16_t offset = 0;
  for (uint8_t bit = 1; bit < target; bit <<= 1) {
    if (activeMask & bit) offset += uiFeatureIconWidth(bit) + 5;
  }
  return offset;
}

constexpr int16_t uiAnalogSwatchX(uint8_t index)
{
  return static_cast<int16_t>(65 + index * 38);
}

constexpr uint8_t uiMenuGlyphAlpha(char glyph, uint8_t x, uint8_t y,
                                   uint8_t alpha)
{
  // The legacy R bitmap contains two isolated pixels inside its counter.
  return glyph == 'R' && y == 20 && (x == 20 || x == 21) ? 0 : alpha;
}
