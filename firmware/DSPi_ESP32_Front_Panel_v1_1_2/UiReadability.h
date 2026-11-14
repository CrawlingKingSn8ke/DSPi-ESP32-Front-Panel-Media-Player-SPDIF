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
  UI_FEATURE_TUBE = 1u << 5,
  UI_FEATURE_LIMITER = 1u << 6,
  UI_FEATURE_ALL = 0x7f
};

constexpr uint8_t uiFeatureIconWidth(uint8_t feature)
{
  return feature == UI_FEATURE_LOUDNESS ? 11 :
         feature == UI_FEATURE_CROSSFEED ? 15 :
         feature == UI_FEATURE_LEVELLER ? 17 :
         feature == UI_FEATURE_PSY_BASS ? 16 :
         feature == UI_FEATURE_SUB_SYNTH ? 20 :
         feature == UI_FEATURE_TUBE ? 15 :
         feature == UI_FEATURE_LIMITER ? 14 : 0;
}

constexpr int16_t uiFeatureIconSpan(uint8_t activeMask)
{
  int16_t span = 0;
  uint8_t count = 0;
  for (uint8_t bit = 1; bit <= UI_FEATURE_LIMITER; bit <<= 1) {
    if (!(activeMask & bit)) continue;
    span += uiFeatureIconWidth(bit);
    ++count;
  }
  return count ? static_cast<int16_t>(span + (count - 1) * 5) : 0;
}

static_assert(uiFeatureIconSpan(UI_FEATURE_ALL) == 138,
              "All seven feature icons must fit the Home header");

constexpr int16_t uiFeatureIconOffset(uint8_t activeMask, uint8_t target)
{
  int16_t offset = 0;
  for (uint8_t bit = 1; bit < target; bit <<= 1) {
    if (activeMask & bit) offset += uiFeatureIconWidth(bit) + 5;
  }
  return offset;
}

constexpr int16_t uiFeatureIconScaledWidth(uint8_t feature,
                                            uint8_t scalePercent)
{
  return static_cast<int16_t>((uiFeatureIconWidth(feature) * scalePercent + 50) / 100);
}

constexpr int16_t uiFeatureIconScaledSpan(uint8_t activeMask,
                                           uint8_t scalePercent,
                                           int16_t gap)
{
  int16_t span = 0;
  uint8_t count = 0;
  for (uint8_t bit = 1; bit <= UI_FEATURE_LIMITER; bit <<= 1) {
    if (!(activeMask & bit)) continue;
    span += uiFeatureIconScaledWidth(bit, scalePercent);
    ++count;
  }
  return count ? static_cast<int16_t>(span + (count - 1) * gap) : 0;
}

constexpr int16_t uiFeatureIconScaledOffset(uint8_t activeMask,
                                             uint8_t target,
                                             uint8_t scalePercent,
                                             int16_t gap)
{
  int16_t offset = 0;
  for (uint8_t bit = 1; bit < target; bit <<= 1) {
    if (activeMask & bit) offset += uiFeatureIconScaledWidth(bit, scalePercent) + gap;
  }
  return offset;
}

static_assert(uiFeatureIconScaledSpan(UI_FEATURE_ALL, 125, 7) == 178,
              "Enlarged Home icons must fit between bounded source and preset labels");

struct UiSourceHeader {uint8_t iconScale;int16_t gap,iconSpan,sourceBudget;};
struct UiRect {int16_t x,y,w,h;};
struct UiHomeHeaderLayout {
 UiRect source,preset,wifi,icons[7];
 int16_t sourceBudget=0,iconSpan=0,gap=7;uint8_t iconScale=125;
 bool secondRow=false;
};
inline UiSourceHeader uiSourceHeader(uint8_t mask,int16_t presetWidth,int16_t sourceWidth,int16_t twoChars,bool retro);
inline UiHomeHeaderLayout uiHomeHeader(int16_t presetWidth,int16_t sourceWidth,bool retro,
                                     uint8_t mask=UI_FEATURE_ALL,int16_t twoChars=32){
 UiHomeHeaderLayout result{};
 const auto fit=uiSourceHeader(mask,presetWidth,sourceWidth,twoChars,retro);
 result.gap=fit.gap;result.iconScale=fit.iconScale;result.iconSpan=fit.iconSpan;
 result.preset={static_cast<int16_t>(302-presetWidth),18,presetWidth,27};
 // This right-side area is outside the Spectrum plot (which ends at x=273).
 // It does not reserve any space in the original source/effects/preset row.
 result.wifi={280,49,20,20};
 result.sourceBudget=fit.sourceBudget;
 const int16_t shownWidth=sourceWidth<result.sourceBudget?sourceWidth:result.sourceBudget;
 result.source={18,18,shownWidth,27};
 const int16_t left=18+shownWidth+8;
 const int16_t centred=(320-result.iconSpan)/2;
 const int16_t right=result.preset.x-result.iconSpan-8;
 const int16_t start=left>(centred<right?centred:right)?left:(centred<right?centred:right);
 for(unsigned index=0;index<7;++index){
  const uint8_t feature=static_cast<uint8_t>(1u<<index);
  const int16_t width=uiFeatureIconScaledWidth(feature,result.iconScale);
  if(mask&feature)result.icons[index]={static_cast<int16_t>(start+uiFeatureIconScaledOffset(mask,feature,result.iconScale,result.gap)),20,width,22};
 }
 return result;
}
inline UiSourceHeader uiSourceHeader(uint8_t mask,int16_t presetWidth,int16_t sourceWidth,int16_t twoChars,bool retro){
  UiSourceHeader fit{125,static_cast<int16_t>(retro?6:7),0,0};
  const int16_t available=302-presetWidth-18-(retro?16:28);
  fit.iconSpan=uiFeatureIconScaledSpan(mask,fit.iconScale,fit.gap);
  const int16_t oldBudget=available>fit.iconSpan?available-fit.iconSpan:0;
  const int16_t wanted=sourceWidth<oldBudget+twoChars?sourceWidth:oldBudget+twoChars;
  while(available-fit.iconSpan<wanted && (fit.gap>3||fit.iconScale>100)){
    if(fit.gap>3)--fit.gap;else fit.iconScale-=5;
    fit.iconSpan=uiFeatureIconScaledSpan(mask,fit.iconScale,fit.gap);
  }
  fit.sourceBudget=available>fit.iconSpan?available-fit.iconSpan:0;
  return fit;
}
constexpr uint8_t UI_SOURCE_MIN_SCALE=50;
constexpr uint8_t uiThemeRowCount(bool retro){return retro?6:5;}
inline uint8_t uiSourceAnnouncementScale(int16_t width,int16_t maximum){
  if(width<=maximum)return 100;
  const int scale=maximum*100L/width;
  return static_cast<uint8_t>(scale<UI_SOURCE_MIN_SCALE?UI_SOURCE_MIN_SCALE:scale);
}

constexpr int16_t uiAnalogSwatchX(uint8_t index)
{
  return static_cast<int16_t>(65 + index * 38);
}
