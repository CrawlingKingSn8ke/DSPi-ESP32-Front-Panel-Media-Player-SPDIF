#pragma once
#include <cstdint>
#include <cmath>

// DSPi v1.1.6 indexed commands. The ESP owns no DSP settings or flash copies.
static constexpr uint8_t TUBE_SET_REQUEST = 0x3e;
static constexpr uint8_t TUBE_GET_REQUEST = 0x3f;
static constexpr uint8_t LIMITER_REQUEST = 0x81;

enum TubeParam : uint8_t {
  TUBE_ENABLE = 0, TUBE_MASK = 1, TUBE_TYPE = 2, TUBE_DRIVE = 3,
  TUBE_MIX = 12, TUBE_INVALID = 255
};
inline bool tubeBasicWritable(TubeParam p) {
  return p == TUBE_ENABLE || p == TUBE_MASK || p == TUBE_TYPE ||
         p == TUBE_DRIVE || p == TUBE_MIX;
}
inline bool tubeValid(TubeParam p, float v) {
  if (!tubeBasicWritable(p) || !std::isfinite(v)) return false;
  switch (p) {
    case TUBE_ENABLE: return v == 0 || v == 1;
    case TUBE_MASK: return v >= 0 && v <= 65535 && std::floor(v) == v;
    case TUBE_TYPE: return v >= 0 && v <= 16 && std::floor(v) == v;
    case TUBE_DRIVE: return v >= -30 && v <= 24;
    case TUBE_MIX: return v >= 0 && v <= 100;
    default: return false;
  }
}
static constexpr const char *TUBE_TYPE_NAMES[] = {
  "Custom", "12AX7", "5751", "12AT7", "12AY7", "12AU7", "6SN7",
  "6SL7", "6DJ8", "EF86", "6SJ7", "EL84", "EL34", "6L6",
  "6V6", "KT88", "300B"
};
inline int tubeOutputAt(uint16_t mask, unsigned index) {
  for (unsigned bit = 0; bit < 9; ++bit) {
    if (!(mask & (1u << bit))) continue;
    if (!index--) return static_cast<int>(bit);
  }
  return -1;
}
inline unsigned tubeOutputCount(uint16_t mask) {
  unsigned count = 0;
  for (unsigned bit = 0; bit < 9; ++bit) if (mask & (1u << bit)) ++count;
  return count;
}

struct TubeState {
  bool known = false, supported = false, outputsKnown = false;
  uint16_t enabledOutputs = 0;
  float enabled = 0, mask = 0, type = 1, drive = -12, mix = 100;
};

enum LimiterParam : uint8_t {
  LIMITER_ENABLE = 0, LIMITER_THRESHOLD = 1,
  LIMITER_RELEASE = 2, LIMITER_LINK = 3
};
inline bool limiterValid(LimiterParam p, float v) {
  if (!std::isfinite(v)) return false;
  switch (p) {
    case LIMITER_ENABLE: return v == 0 || v == 1;
    case LIMITER_THRESHOLD: return v >= -30 && v <= 0;
    case LIMITER_RELEASE: return v >= 10 && v <= 1000;
    case LIMITER_LINK: return v >= 0 && v <= 4 && std::floor(v) == v;
    default: return false;
  }
}
struct LimiterOutputState {
  float value[4] = {0, -1, 100, 0};
};
struct LimiterState {
  bool known = false, supported = false, outputsKnown = false;
  uint16_t enabledOutputs = 0;
  LimiterOutputState output[9];
};
