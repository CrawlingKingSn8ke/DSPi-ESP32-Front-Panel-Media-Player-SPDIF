#pragma once

#include <stdint.h>
#include <string.h>

// DSPi spectrum analyser wire format V3 (v1.1.6 and later). The ESP requests
// only the two output channels; the 0x0F all-channel command is USB-only.
namespace SpectrumRta {
constexpr uint8_t kVersion = 3;
constexpr uint8_t kBands = 37;
constexpr uint8_t kBandFrameBytes = 82;
constexpr uint8_t kConfigBytes = 12;
constexpr uint8_t kCapsBytes = 16;
constexpr uint16_t kOutputPairMask = 0x0003;
constexpr uint8_t kOutputTap = 1;

struct BandFrame {
  uint8_t channel = 0;
  uint8_t sequence = 0;
  uint8_t count = 0;
  uint16_t ageMs = 0;
  uint8_t average[kBands] = {};
};

inline bool parseCaps(const uint8_t *wire, uint16_t length, uint8_t &fftOrder,
                      uint8_t &levelZero) {
  if (!wire || length != kCapsBytes || wire[0] != kVersion ||
      wire[2] < 2 || wire[7] != kBands || wire[4] < 8 || wire[4] > 10 ||
      wire[8] == 0) return false;
  fftOrder = wire[5] >= wire[3] && wire[5] <= wire[4] ? wire[5] : wire[4];
  levelZero = wire[8];
  return true;
}

inline void makeOutputPairConfig(uint8_t *wire, uint8_t fftOrder) {
  memset(wire, 0, kConfigBytes);
  wire[0] = kVersion;
  wire[1] = kOutputTap;
  wire[2] = static_cast<uint8_t>(kOutputPairMask);
  wire[4] = fftOrder;
  wire[6] = 180; // 180 ms power-domain averaging; no manual run flag.
  wire[8] = 24;  // Peak decay is harmless even though this page shows average.
}

inline bool isOutputPairConfig(const uint8_t *wire, uint16_t length) {
  return wire && length == kConfigBytes && wire[0] == kVersion &&
         wire[1] == kOutputTap && wire[2] == 3 && wire[3] == 0 &&
         wire[9] == 0;
}

inline bool parseBandFrame(const uint8_t *wire, uint16_t length,
                           uint8_t expectedChannel, BandFrame &frame) {
  if (!wire || length != kBandFrameBytes || wire[0] != kVersion ||
      wire[1] != expectedChannel || expectedChannel > 1 ||
      wire[3] == 0 || wire[3] > kBands) return false;
  frame.channel = wire[1];
  frame.sequence = wire[2];
  frame.count = wire[3];
  frame.ageMs = static_cast<uint16_t>(wire[4]) |
                (static_cast<uint16_t>(wire[5]) << 8);
  if (frame.ageMs == 0xffff || frame.ageMs > 1000) return false;
  memcpy(frame.average, wire + 8, kBands);
  return true;
}

inline uint8_t barHeight(uint8_t encoded, uint8_t levelZero, uint8_t height) {
  // Render -72..0 dBFS; DSPi encodes each half-dB as one byte.
  const int16_t floor = static_cast<int16_t>(levelZero) - 144;
  if (encoded <= floor) return 0;
  if (encoded >= levelZero) return height;
  return static_cast<uint8_t>((static_cast<uint16_t>(encoded - floor) * height) / 144);
}
} // namespace SpectrumRta
