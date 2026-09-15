#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// One entry, owned exclusively by the artwork worker. UI/result queues receive
// independent pixel buffers. Exact compressed-byte identity preserves covers
// embedded per track, including after files are replaced through Wi-Fi.
class ArtworkCache {
 public:
  using Allocate = void *(*)(size_t);
  using Release = void (*)(void *);
  ArtworkCache(Allocate allocate, Release release)
      : allocate_(allocate), release_(release) {}
  ~ArtworkCache() { release_(entry_); }
  ArtworkCache(const ArtworkCache &) = delete;
  ArtworkCache &operator=(const ArtworkCache &) = delete;

  bool copy(const uint8_t *jpeg, size_t length, uint16_t *&pixels,
            uint16_t &width, uint16_t &height) const {
    pixels = nullptr;
    width = height = 0;
    if (!entry_ || !jpeg || length != jpegLength_ ||
        std::memcmp(entry_, jpeg, length) != 0) return false;
    pixels = static_cast<uint16_t *>(allocate_(pixelBytes_));
    if (!pixels) return false;
    std::memcpy(pixels, entry_ + jpegLength_, pixelBytes_);
    width = width_;
    height = height_;
    return true;
  }

  bool remember(const uint8_t *jpeg, size_t length, const uint16_t *pixels,
                uint16_t width, uint16_t height) {
    const uint64_t bytes = uint64_t(width) * height * sizeof(uint16_t);
    if (!jpeg || !length || length > 256U * 1024U || !pixels || !bytes ||
        bytes > 128U * 1024U) return false;
    // Release the previous entry before allocating: cache residency never
    // exceeds 384 KiB. Failure affects caching only, not the decoded result.
    release_(entry_);
    entry_ = nullptr;
    jpegLength_ = pixelBytes_ = 0;
    entry_ = static_cast<uint8_t *>(allocate_(length + size_t(bytes)));
    if (!entry_) return false;
    std::memcpy(entry_, jpeg, length);
    std::memcpy(entry_ + length, pixels, size_t(bytes));
    jpegLength_ = length;
    pixelBytes_ = size_t(bytes);
    width_ = width;
    height_ = height;
    return true;
  }

 private:
  Allocate allocate_;
  Release release_;
  uint8_t *entry_ = nullptr;
  size_t jpegLength_ = 0;
  size_t pixelBytes_ = 0;
  uint16_t width_ = 0, height_ = 0;
};
