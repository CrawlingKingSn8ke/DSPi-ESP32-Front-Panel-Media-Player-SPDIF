#pragma once

#include <stdint.h>
#include <string.h>

// DSPi spectrum analyser wire format V3 (v1.1.6 and later). The ESP requests
// only the selected two channels; the 0x0F all-channel command is USB-only.
namespace SpectrumRta {
constexpr uint8_t kVersion = 3;
constexpr uint8_t kBands = 37;
constexpr uint8_t kBandFrameBytes = 82;
constexpr uint8_t kConfigBytes = 12;
constexpr uint8_t kCapsBytes = 16;
constexpr uint8_t kOutputTap = 1;
constexpr uint8_t kInputTap = 0;

struct SelectionRecord {
  uint8_t version = 1;
  uint8_t tap = kOutputTap;
  uint8_t upper = 0;
  uint8_t lower = 1;
};
static_assert(sizeof(SelectionRecord) == 4, "Spectrum selection NVS layout");

inline bool validSelection(const SelectionRecord &selection) {
  return selection.version == 1 && selection.tap <= kOutputTap &&
         selection.upper < 16 && selection.lower < 16;
}

inline uint16_t channelMask(const SelectionRecord &selection) {
  return static_cast<uint16_t>((1u << selection.upper) |
                               (1u << selection.lower));
}

inline uint8_t firstEnabled(uint16_t mask) {
  for (uint8_t i = 0; i < 16; ++i) if (mask & (1u << i)) return i;
  return 0xff;
}

inline uint8_t nextEnabled(uint16_t mask, uint8_t current, int direction,
                           uint8_t avoid = 0xff) {
  if (!mask) return 0xff;
  for (uint8_t step = 0; step < 16; ++step) {
    current = static_cast<uint8_t>((current + (direction > 0 ? 1 : 15)) & 15);
    if ((mask & (1u << current)) && current != avoid) return current;
  }
  return 0xff;
}

inline void fitSelection(SelectionRecord &selection, uint16_t mask) {
  if (!mask) return;
  if (!(mask & (1u << selection.upper))) selection.upper = firstEnabled(mask);
  if (!(mask & (1u << selection.lower)) || selection.lower == selection.upper) {
    const uint8_t next = nextEnabled(mask, selection.upper, 1, selection.upper);
    selection.lower = next == 0xff ? selection.upper : next;
  }
}

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

inline void makeConfig(uint8_t *wire, uint8_t fftOrder,
                       const SelectionRecord &selection) {
  memset(wire, 0, kConfigBytes);
  wire[0] = kVersion;
  wire[1] = selection.tap;
  const uint16_t mask = channelMask(selection);
  wire[2] = static_cast<uint8_t>(mask);
  wire[3] = static_cast<uint8_t>(mask >> 8);
  wire[4] = fftOrder;
  wire[6] = 120; // Faster 120 ms averaging; no manual run flag.
  wire[8] = 24;  // Peak decay is harmless even though this page shows average.
}

inline bool parseBandFrame(const uint8_t *wire, uint16_t length,
                           uint8_t expectedChannel, BandFrame &frame) {
  if (!wire || length != kBandFrameBytes || wire[0] != kVersion ||
      wire[1] != expectedChannel || expectedChannel >= 16 ||
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
  // Render -60..0 dBFS; DSPi encodes each half-dB as one byte.
  const int16_t floor = static_cast<int16_t>(levelZero) - 120;
  if (encoded <= floor) return 0;
  if (encoded >= levelZero) return height;
  return static_cast<uint8_t>((static_cast<uint16_t>(encoded - floor) * height) / 120);
}

inline uint8_t interpolateHeight(uint8_t from, uint8_t to,
                                 uint32_t elapsedMs, uint32_t durationMs) {
  if (!durationMs || elapsedMs >= durationMs) return to;
  const uint32_t distance = to >= from ? to - from : from - to;
  const uint32_t step = (distance * elapsedMs + durationMs / 2) / durationMs;
  return to >= from ? static_cast<uint8_t>(from + step)
                    : static_cast<uint8_t>(from - step);
}
} // namespace SpectrumRta
