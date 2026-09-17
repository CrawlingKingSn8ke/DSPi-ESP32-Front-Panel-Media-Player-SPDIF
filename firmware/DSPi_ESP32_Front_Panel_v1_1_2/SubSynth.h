#pragma once
#include <cstdint>
#include <cmath>

// DSPi v1.1.6-beta3 vendor commands. No local audio processing or persistence.
enum SubSynthParam : uint8_t {
  SUB_ENABLE, SUB_LOW, SUB_HIGH, SUB_TOP, SUB_SELECT, SUB_DEPTH, SUB_HOLD,
  SUB_CEILING, SUB_BOOST, SUB_MASK, SUB_LINK, SUB_HEADROOM, SUB_PARAM_COUNT,
  SUB_INVALID = 255
};
struct SubSynthSpec { uint8_t set, get, bytes; float minimum, maximum, step; };
static constexpr SubSynthSpec subSynthSpecs[SUB_PARAM_COUNT] = {
  {0x10,0x11,1,0,1,1}, {0x12,0x13,4,-30,12,0.5f},
  {0x14,0x15,4,-30,12,0.5f}, {0x1b,0x1c,4,-30,12,0.5f},
  {0x1d,0x1e,1,0,2,1}, {0xa9,0xaa,4,0,100,5},
  {0xab,0xac,4,50,400,10}, {0xad,0xae,4,-40,0,0.5f},
  {0x16,0x17,4,0,6,0.5f}, {0x18,0x19,2,0,65535,1},
  {0x2e,0x2f,1,0,1,1}, {0,0x1a,4,0,100,0}
};
struct SubSynthState {
  bool known = false, supported = false, outputsKnown = false;
  uint16_t enabledOutputs = 0;
  float value[SUB_PARAM_COUNT] = {0,0,0,-30,0,100,150,0,0,65535,1,0};
};
inline bool subSynthValid(SubSynthParam p, float v) {
  if (p >= SUB_PARAM_COUNT || !std::isfinite(v)) return false;
  const auto &s = subSynthSpecs[p];
  return v >= s.minimum && v <= s.maximum &&
         (s.bytes == 4 || std::floor(v) == v);
}
inline SubSynthParam subSynthParameter(unsigned page, unsigned row) {
  static constexpr SubSynthParam main[] = {SUB_ENABLE,SUB_LOW,SUB_HIGH,SUB_TOP,
    SUB_INVALID,SUB_CEILING,SUB_BOOST,SUB_INVALID,SUB_LINK,SUB_HEADROOM};
  static constexpr SubSynthParam select[] = {SUB_SELECT,SUB_DEPTH,SUB_HOLD};
  if (page == 0 && row < 10) return main[row];
  if (page == 1 && row < 3) return select[row];
  return SUB_INVALID;
}
inline int subSynthOutputAt(uint16_t mask, unsigned index) {
  for (unsigned bit = 0; bit < 9; ++bit) {
    if (!(mask & (1u << bit))) continue;
    if (!index--) return static_cast<int>(bit);
  }
  return -1;
}
inline unsigned subSynthOutputCount(uint16_t mask) {
  unsigned count = 0;
  for (unsigned bit = 0; bit < 9; ++bit) if (mask & (1u << bit)) ++count;
  return count;
}
inline uint16_t subSynthToggleOutput(uint16_t mask, unsigned output) {
  return output < 9 ? static_cast<uint16_t>(mask ^ (1u << output)) : mask;
}
