#pragma once

#include <Arduino.h>

// Block-oriented consumer S/PDIF encoder for the media output task.
//
// The 24-bit wire coding is derived from squeezelite-esp32's tested MIT-licensed
// S/PDIF output. This interface deliberately accepts the player's existing
// signed 32-bit PCM and emits complete DMA-sized blocks instead of performing
// one blocking I2S write for every stereo sample.
class SpdifBlockEncoder {
public:
  static constexpr size_t kWordsPerStereoFrame = 4;

  void reset();

  // Encodes the most-significant 24 bits of signed, MSB-aligned stereo PCM.
  // A null PCM pointer generates valid digital silence while preserving the
  // 192-frame channel-status block cadence.
  bool encode(const int32_t *stereoPcm, size_t frames,
              uint32_t *encodedWords, size_t encodedWordCapacity);

private:
  uint8_t frameNumber_ = 0;
  uint8_t vucp_ = 0xCC;
};
