#include "MediaPlayerPoC.h"
#include "Mp3SeekPolicy.h"
#include "Mp3ArtworkPolicy.h"
#include "SpdifBlockEncoder.h"
#include "DirectoryPageCache.h"

#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_libs/dr_wav.h"
#include "third_party/dr_libs/dr_flac.h"
#include "third_party/dr_libs/dr_mp3.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/semphr.h>
#include <new>
#include <soc/soc_caps.h>

namespace {

void *allocateDirectoryPage(size_t bytes)
{
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (heap_caps_get_free_size(caps) < bytes + 512U * 1024U) return nullptr;
  return heap_caps_malloc(bytes, caps);
}

// Only the foreground browser/storage owner accesses this optional cache.
// Audio tasks never allocate, invalidate or consult it.
DirectoryPageCache<MediaBrowserEntry, MediaDirectoryPageInfo>
    directoryPageCache(allocateDirectoryPage, heap_caps_free);

// Playback uses a conservative shared-SPI clock with automatic lower-speed
// fallbacks for card compatibility.
constexpr uint32_t kSdFrequenciesHz[] = {
  20000000, 10000000, 4000000
};
constexpr uint8_t kSdMountAttemptsPerFrequency = 2;
constexpr uint32_t kSpiLockTimeoutMs = 2000;
constexpr size_t kMp3ScanLimit = 64 * 1024;
// Hold enough decoded PCM to ride through a clustered SD/FLAC latency burst.
// At 44.1 kHz these provide about 5.94 s, 2.97 s and 1.49 s of audio.
constexpr size_t kPreferredRingFrames = 262144;
constexpr size_t kFallbackRingFrames = 131072;
constexpr size_t kEmergencyRingFrames = 65536;
constexpr size_t kDecodeChunkFrames = 512;
constexpr size_t kOutputChunkFrames = 256;
// The I2S peripheral runs at 2x the source sample rate and emits two 32-bit
// slots per I2S frame. Therefore 384 I2S frames carry exactly one complete
// 192-frame stereo S/PDIF channel-status block. Keeping each retained DMA
// descriptor block-aligned means a late writer repeats valid framed data
// instead of an arbitrary fragment with malformed B-preamble cadence.
constexpr uint32_t kSpdifDmaFramesPerDescriptor = 384;
constexpr uint32_t kSpdifDmaDescriptorCount = 16;
constexpr int kSpdifInterruptPriority = 2;
constexpr uint64_t kMp3SeekYieldFrames = 1152ULL * 4ULL;
constexpr size_t kStartPrefillFrames = 16384;
constexpr size_t kSeekPrefillFrames = 8192;
constexpr uint32_t kMinimumPsramBandwidthTenthsMiB = 40;  // 4.0 MiB/s.
constexpr UBaseType_t kDecoderTaskPriority = 3;
constexpr UBaseType_t kOutputTaskPriority = 6;
// Keep the time-critical S/PDIF writer isolated on CPU1. Let decode/SD work use
// whichever CPU is available: output always preempts it on CPU1, while an idle
// CPU0 can absorb it between Bluetooth controller work. Automatic BLE scans
// are already suspended for active playback.
constexpr BaseType_t kDecoderTaskCore = tskNO_AFFINITY;
constexpr BaseType_t kOutputTaskCore = 1;
constexpr size_t kTailSilenceFrames = 512;
constexpr uint32_t kTaskStopTimeoutMs = 3000;
constexpr uint32_t kSeekQuiesceTimeoutMs = 750;
constexpr uint32_t kSeekPrefillTimeoutMs = 2500;
// Keep decoder I/O cooperative with the other front-panel tasks.
constexpr uint32_t kDecoderIoYieldIntervalMs = 20;
constexpr uint32_t kSlowDecoderCallMs = 100;
constexpr uint32_t kDecoderSlowReadThresholdMs = 100;
// dr_flac normally asks for 4 KiB. Keep that request contiguous so SdFat can
// issue a multi-sector read instead of four separately-arbitrated 1 KiB reads.
// Low-buffer UI guards bound shared-bus impact during abnormal card latency.
constexpr size_t kDecoderReadSliceBytes = 4096;
constexpr uint32_t kDecoderTaskStackBytes = 32768;
constexpr uint32_t kSeekMinimumFreeStackBytes = 8192;
constexpr EventBits_t kSeekActiveBit = BIT0;
constexpr EventBits_t kOutputQuiescentBit = BIT1;
constexpr EventBits_t kExternalHoldBit = BIT2;
constexpr size_t kArtworkMaximumBytes = 2 * 1024 * 1024;
constexpr uint32_t kMetadataMaximumBytes = 8 * 1024 * 1024;
constexpr size_t kArtworkReadChunkBytes = 2 * 1024;

static_assert((kPreferredRingFrames & (kPreferredRingFrames - 1)) == 0,
              "Preferred PCM ring frame count must be a power of two");
static_assert((kFallbackRingFrames & (kFallbackRingFrames - 1)) == 0,
              "Fallback PCM ring frame count must be a power of two");
static_assert((kEmergencyRingFrames & (kEmergencyRingFrames - 1)) == 0,
              "Emergency PCM ring frame count must be a power of two");
static_assert(kOutputTaskPriority > kDecoderTaskPriority,
              "S/PDIF output must always preempt media decoding");
static_assert(kSpdifDmaFramesPerDescriptor == 192u * 2u,
              "Each S/PDIF DMA descriptor must hold one complete block");
static_assert(kSpdifDmaFramesPerDescriptor * 2u * sizeof(uint32_t) <= 4092u,
              "ESP-IDF limits each I2S DMA descriptor to 4092 bytes");

SemaphoreHandle_t sharedSpiMutex()
{
  static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutex();
  return mutex;
}

class SharedSpiGuard {
public:
  SharedSpiGuard() { mediaSharedSpiLock(); }
  ~SharedSpiGuard() { mediaSharedSpiUnlock(); }
};

class TrySharedSpiGuard {
public:
  explicit TrySharedSpiGuard(uint32_t timeoutMs)
      : locked_(mediaSharedSpiTryLock(timeoutMs)) {}
  ~TrySharedSpiGuard() {
    if (locked_) mediaSharedSpiUnlock();
  }
  explicit operator bool() const { return locked_; }

  TrySharedSpiGuard(const TrySharedSpiGuard &) = delete;
  TrySharedSpiGuard &operator=(const TrySharedSpiGuard &) = delete;

private:
  bool locked_ = false;
};

uint16_t readLe16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readLe32(const uint8_t *p)
{
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

uint32_t readBe32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

uint32_t readSynchsafe32(const uint8_t *p)
{
  return ((uint32_t)(p[0] & 0x7F) << 21) |
         ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) |
         (uint32_t)(p[3] & 0x7F);
}

bool readExact(MediaFsFile &file, uint8_t *buffer, size_t length)
{
  SharedSpiGuard guard;
  return file.read(buffer, length) == length;
}

bool nativeRateSupported(uint32_t sampleRate)
{
  return sampleRate == 44100 || sampleRate == 48000;
}

bool sourceDepthSupported(MediaFileFormat format, uint8_t bitsPerSample)
{
  return format == MediaFileFormat::Mp3 ||
         bitsPerSample == 16 || bitsPerSample == 24;
}

bool endsWithIgnoreCase(const char *text, const char *suffix)
{
  if (!text || !suffix) return false;
  size_t textLength = std::strlen(text);
  size_t suffixLength = std::strlen(suffix);
  if (suffixLength > textLength) return false;

  const char *candidate = text + textLength - suffixLength;
  for (size_t i = 0; i < suffixLength; i++) {
    char a = candidate[i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

MediaFileFormat formatFromPath(const char *path)
{
  if (endsWithIgnoreCase(path, ".wav")) return MediaFileFormat::Wav;
  if (endsWithIgnoreCase(path, ".flac")) return MediaFileFormat::Flac;
  if (endsWithIgnoreCase(path, ".mp3")) return MediaFileFormat::Mp3;
  return MediaFileFormat::Unknown;
}

int compareIgnoreCase(const char *a, const char *b)
{
  if (!a) a = "";
  if (!b) b = "";
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

void clearBrowserEntry(MediaBrowserEntry &entry)
{
  entry.path[0] = '\0';
  entry.name[0] = '\0';
  entry.directory = false;
  entry.format = MediaFileFormat::Unknown;
}

bool hiddenBrowserName(const char *name)
{
  if (!name || !name[0] || name[0] == '.') return true;
  return compareIgnoreCase(name, "System Volume Information") == 0;
}

int compareBrowserEntries(const MediaBrowserEntry &a,
                          const MediaBrowserEntry &b)
{
  if (a.directory != b.directory) return a.directory ? -1 : 1;
  int compared = compareIgnoreCase(a.name, b.name);
  if (compared != 0) return compared;
  compared = compareIgnoreCase(a.path, b.path);
  if (compared != 0) return compared;
  return std::strcmp(a.path, b.path);
}

// Insert one candidate into an ascending fixed-capacity page. First/next
// pages keep the smallest candidates after their anchor. Previous pages keep
// the largest candidates before their anchor. Memory therefore remains O(page
// size) no matter how many entries exist in the directory.
void insertBrowserPageCandidate(MediaBrowserEntry *entries, size_t &count,
                                size_t capacity,
                                const MediaBrowserEntry &candidate,
                                bool keepLargest)
{
  if (!entries || capacity == 0) return;

  if (count < capacity) {
    size_t position = count;
    while (position > 0 &&
           compareBrowserEntries(candidate, entries[position - 1]) < 0) {
      entries[position] = entries[position - 1];
      position--;
    }
    entries[position] = candidate;
    count++;
    return;
  }

  if (!keepLargest) {
    if (compareBrowserEntries(candidate, entries[count - 1]) >= 0) return;
    size_t position = count - 1;
    while (position > 0 &&
           compareBrowserEntries(candidate, entries[position - 1]) < 0) {
      entries[position] = entries[position - 1];
      position--;
    }
    entries[position] = candidate;
    return;
  }

  if (compareBrowserEntries(candidate, entries[0]) <= 0) return;
  size_t position = 1;
  while (position < count &&
         compareBrowserEntries(entries[position], candidate) < 0) {
    entries[position - 1] = entries[position];
    position++;
  }
  entries[position - 1] = candidate;
}

bool seekFile(MediaFsFile &file, int offset, int origin)
{
  SharedSpiGuard guard;
  int64_t base = 0;
  if (origin == 1) base = (int64_t)file.position();
  else if (origin == 2) base = (int64_t)file.size();
  else if (origin != 0) return false;

  int64_t target = base + offset;
  if (target < 0) return false;
  return file.seek((uint64_t)target);
}

bool readAt(MediaFsFile &file, uint64_t offset, uint8_t *buffer, size_t length)
{
  if (!buffer || length > UINT32_MAX ||
      offset > UINT64_MAX - (uint64_t)length) {
    return false;
  }
  SharedSpiGuard guard;
  return file.seek(offset) && file.read(buffer, length) == length;
}

bool artworkCancelled(const MediaArtworkControl *control)
{
  return control && control->cancelRequested &&
         control->cancelRequested(control->context);
}

bool waitForArtworkPermit(const MediaArtworkControl *control)
{
  if (artworkCancelled(control)) return false;
  if (control && control->waitForPermit &&
      !control->waitForPermit(control->context)) {
    return false;
  }
  return !artworkCancelled(control);
}

bool artworkReadAt(MediaFsFile &file, uint64_t offset, uint8_t *buffer,
                   size_t length, const MediaArtworkControl *control)
{
  return waitForArtworkPermit(control) &&
         readAt(file, offset, buffer, length);
}

uint64_t fileSizeLocked(MediaFsFile &file,
                        const MediaArtworkControl *control = nullptr)
{
  if (!waitForArtworkPermit(control)) return 0;
  SharedSpiGuard guard;
  return file.size();
}

// Locate the actual FLAC container after any leading ID3v2 tags. Some music
// tools write ID3 metadata ahead of a native FLAC stream. dr_flac recognises
// such tags while reading sequentially, but this vendored revision performs
// later absolute seeks relative to byte zero of the whole file. Presenting the
// decoder with a logical stream beginning at the FLAC/Ogg marker keeps every
// seek relative to the correct container start.
bool findFlacStreamOffset(MediaFsFile &file, uint64_t &streamOffset,
                          const MediaArtworkControl *control = nullptr)
{
  const uint64_t fileSize = fileSizeLocked(file, control);
  uint64_t cursor = 0;

  for (uint8_t tagIndex = 0; tagIndex < 8; tagIndex++) {
    if (cursor + 4 > fileSize) return false;

    uint8_t marker[4];
    if (!artworkReadAt(file, cursor, marker, sizeof(marker), control)) return false;
    if (std::memcmp(marker, "fLaC", 4) == 0 ||
        std::memcmp(marker, "OggS", 4) == 0) {
      streamOffset = cursor;
      return true;
    }

    if (std::memcmp(marker, "ID3", 3) != 0 || cursor + 10 > fileSize) {
      return false;
    }

    uint8_t header[10];
    if (!artworkReadAt(file, cursor, header, sizeof(header), control)) return false;
    for (uint8_t i = 6; i < 10; i++) {
      if (header[i] & 0x80) return false;
    }

    uint64_t payloadSize = readSynchsafe32(header + 6);
    uint64_t footerSize = (header[5] & 0x10) ? 10ULL : 0ULL;
    uint64_t next = cursor + 10ULL + payloadSize + footerSize;
    if (next <= cursor || next > fileSize) return false;
    cursor = next;
  }
  return false;
}

// Read the mandatory native-FLAC STREAMINFO block directly.  dr_flac remains
// the decoder, but the container header is the authoritative source for the
// stream rate/depth/channel contract used to configure DSPi.  This also gives
// us a useful cross-check if a decoder/library regression reports a different
// rate for an otherwise valid file.
bool readNativeFlacStreamInfo(MediaFsFile &file, uint64_t streamOffset,
                              MediaFileInfo &info)
{
  info = MediaFileInfo{};

  uint8_t marker[4] = {};
  uint8_t blockHeader[4] = {};
  uint8_t streamInfo[34] = {};
  if (!readAt(file, streamOffset, marker, sizeof(marker)) ||
      std::memcmp(marker, "fLaC", sizeof(marker)) != 0 ||
      !readAt(file, streamOffset + 4, blockHeader, sizeof(blockHeader))) {
    return false;
  }

  const uint8_t blockType = blockHeader[0] & 0x7F;
  const uint32_t blockLength = ((uint32_t)blockHeader[1] << 16) |
                               ((uint32_t)blockHeader[2] << 8) |
                               (uint32_t)blockHeader[3];
  if (blockType != 0 || blockLength != sizeof(streamInfo) ||
      !readAt(file, streamOffset + 8, streamInfo, sizeof(streamInfo))) {
    return false;
  }

  uint64_t packed = 0;
  for (uint8_t i = 10; i < 18; i++) {
    packed = (packed << 8) | streamInfo[i];
  }

  info.format = MediaFileFormat::Flac;
  info.sampleRate = (uint32_t)((packed >> 44) & 0xFFFFFULL);
  info.channels = (uint8_t)(((packed >> 41) & 0x07ULL) + 1ULL);
  info.bitsPerSample = (uint8_t)(((packed >> 36) & 0x1FULL) + 1ULL);
  info.totalFrames = packed & 0xFFFFFFFFFULL;
  info.valid = info.sampleRate > 0 && info.channels > 0 &&
               info.bitsPerSample > 0;
  info.nativeRateSupported = nativeRateSupported(info.sampleRate);
  return info.valid;
}

struct DecoderReadTelemetry {
  MediaPlaybackStats *stats = nullptr;
  uint32_t lastYieldAt = 0;
  bool ioErrorCounted = false;
};

struct DecoderIoContext {
  MediaFsFile *file = nullptr;
  volatile bool *cancelRequested = nullptr;
  DecoderReadTelemetry readTelemetry;
};

struct FlacStreamContext {
  MediaFsFile *file = nullptr;
  volatile bool *cancelRequested = nullptr;
  uint64_t baseOffset = 0;
  DecoderReadTelemetry readTelemetry;
};

void updateAtomicMaximum(uint32_t *target, uint32_t value)
{
  if (!target) return;
  uint32_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
  while (value > observed &&
         !__atomic_compare_exchange_n(target, &observed, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void updateAtomicMinimum(uint32_t *target, uint32_t value)
{
  if (!target) return;
  uint32_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
  while (value < observed &&
         !__atomic_compare_exchange_n(target, &observed, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

bool atomicFlagIsSet(const volatile bool *flag)
{
  return flag && __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

size_t cooperativeDecoderRead(MediaFsFile *file,
                              volatile bool *cancelRequested,
                              DecoderReadTelemetry &telemetry,
                              void *output, size_t bytesToRead)
{
  if (!file || !*file || !output || bytesToRead == 0 ||
      atomicFlagIsSet(cancelRequested)) {
    return 0;
  }

  // Once an I/O error is reported, stop issuing reads from this decoder.
  if (file->hadIoError()) {
    if (telemetry.stats && !telemetry.ioErrorCounted) {
      __atomic_add_fetch(&telemetry.stats->sdReadErrors, 1U,
                         __ATOMIC_RELAXED);
      telemetry.ioErrorCounted = true;
    }
    if (telemetry.stats) {
      __atomic_add_fetch(&telemetry.stats->sdReadYields, 1U,
                         __ATOMIC_RELAXED);
    }
    vTaskDelay(1);
    telemetry.lastYieldAt = millis();
    return 0;
  }

  uint8_t *destination = static_cast<uint8_t *>(output);
  size_t totalRead = 0;
  const uint32_t callbackStartedAt = millis();

  while (totalRead < bytesToRead &&
         !atomicFlagIsSet(cancelRequested) && !file->hadIoError()) {
    const size_t sliceRequest =
        std::min(kDecoderReadSliceBytes, bytesToRead - totalRead);
    const uint32_t sliceStartedAt = millis();
    uint32_t transferStartedAt = sliceStartedAt;
    size_t sliceRead = 0;
    {
      SharedSpiGuard guard;
      transferStartedAt = millis();
      if (!atomicFlagIsSet(cancelRequested) && !file->hadIoError()) {
        sliceRead = file->read(destination + totalRead, sliceRequest);
      }
    }

    const uint32_t sliceFinishedAt = millis();
    const uint32_t spiWaitMs = transferStartedAt - sliceStartedAt;
    const uint32_t transferElapsedMs = sliceFinishedAt - transferStartedAt;
    const uint32_t sliceElapsedMs = sliceFinishedAt - sliceStartedAt;
    if (telemetry.stats) {
      __atomic_add_fetch(&telemetry.stats->sdReadSlices, 1U,
                         __ATOMIC_RELAXED);
      updateAtomicMaximum(&telemetry.stats->sdReadSliceMaxMs,
                          sliceElapsedMs);
      updateAtomicMaximum(&telemetry.stats->sdSpiWaitMaxMs, spiWaitMs);
      updateAtomicMaximum(&telemetry.stats->sdTransferMaxMs,
                          transferElapsedMs);
    }
    totalRead += sliceRead;

    const bool ioError = file->hadIoError();
    if (ioError && telemetry.stats && !telemetry.ioErrorCounted) {
      __atomic_add_fetch(&telemetry.stats->sdReadErrors, 1U,
                         __ATOMIC_RELAXED);
      telemetry.ioErrorCounted = true;
    }

    // Delay outside the shared-SPI guard after slow or failed reads, and at
    // regular intervals, so lower-priority system tasks can run.
    const uint32_t now = millis();
    if (ioError || sliceRead == 0 ||
        sliceElapsedMs >= kDecoderIoYieldIntervalMs ||
        (uint32_t)(now - telemetry.lastYieldAt) >=
            kDecoderIoYieldIntervalMs) {
      if (telemetry.stats) {
        __atomic_add_fetch(&telemetry.stats->sdReadYields, 1U,
                           __ATOMIC_RELAXED);
      }
      vTaskDelay(1);
      telemetry.lastYieldAt = millis();
    }

    if (sliceRead < sliceRequest || ioError) break;
  }

  const uint32_t callbackElapsedMs = millis() - callbackStartedAt;
  if (telemetry.stats) {
    __atomic_add_fetch(&telemetry.stats->sdReadCalls, 1U,
                       __ATOMIC_RELAXED);
    updateAtomicMaximum(&telemetry.stats->sdReadMaxMs, callbackElapsedMs);
    if (callbackElapsedMs >= kDecoderSlowReadThresholdMs) {
      __atomic_add_fetch(&telemetry.stats->sdSlowReads, 1U,
                         __ATOMIC_RELAXED);
    }
  }
  return totalRead;
}

struct ArtworkLocation {
  char path[MEDIA_FS_PATH_CAPACITY] = {0};
  uint32_t offset = 0;
  uint32_t length = 0;
  uint8_t pictureType = 0xFF;
};

bool setArtworkLocation(ArtworkLocation &location, const char *path,
                        uint64_t offset, uint64_t length,
                        uint8_t pictureType = 0xFF)
{
  if (!path || !path[0] || std::strlen(path) >= sizeof(location.path) ||
      length < 3 || length > kArtworkMaximumBytes ||
      offset > UINT32_MAX || offset + length > (uint64_t)UINT32_MAX + 1ULL) {
    return false;
  }
  strlcpy(location.path, path, sizeof(location.path));
  location.offset = (uint32_t)offset;
  location.length = (uint32_t)length;
  location.pictureType = pictureType;
  return true;
}

bool jpegSignatureAt(MediaFsFile &file, uint64_t offset,
                     const MediaArtworkControl *control)
{
  uint8_t signature[3];
  return artworkReadAt(file, offset, signature, sizeof(signature), control) &&
         signature[0] == 0xFF && signature[1] == 0xD8 &&
         signature[2] == 0xFF;
}

bool findEncodedTextEnd(MediaFsFile &file, uint64_t start, uint64_t end,
                        uint8_t encoding, uint64_t &afterTerminator,
                        const MediaArtworkControl *control)
{
  if (start >= end) return false;
  constexpr size_t kMaximumDescriptionBytes = 1024;
  size_t bytes = (size_t)std::min<uint64_t>(
      end - start, kMaximumDescriptionBytes);
  uint8_t text[kMaximumDescriptionBytes];
  if (!artworkReadAt(file, start, text, bytes, control)) return false;

  if (encoding == 0 || encoding == 3) {
    for (size_t i = 0; i < bytes; i++) {
      if (text[i] == 0) {
        afterTerminator = start + i + 1;
        return true;
      }
    }
    return false;
  }

  if (encoding == 1 || encoding == 2) {
    for (size_t i = 0; i + 1 < bytes; i += 2) {
      if (text[i] == 0 && text[i + 1] == 0) {
        afterTerminator = start + i + 2;
        return true;
      }
    }
  }
  return false;
}

bool locateId3ApicPayload(MediaFsFile &file, const char *path,
                          uint64_t frameStart, uint64_t frameSize,
                          bool id3v22, ArtworkLocation &location,
                          const MediaArtworkControl *control)
{
  uint64_t frameEnd = frameStart + frameSize;
  if (frameSize < (id3v22 ? 6U : 5U)) return false;

  uint8_t encoding = 0;
  if (!artworkReadAt(file, frameStart, &encoding, 1, control) ||
      encoding > 3) return false;

  uint64_t pictureTypePosition = 0;
  uint64_t descriptionStart = 0;
  if (id3v22) {
    uint8_t fixed[4];
    if (!artworkReadAt(file, frameStart + 1, fixed, sizeof(fixed), control))
      return false;
    pictureTypePosition = frameStart + 4;
    descriptionStart = frameStart + 5;
  } else {
    uint64_t mimeStart = frameStart + 1;
    size_t mimeBytes = (size_t)std::min<uint64_t>(frameEnd - mimeStart, 64);
    if (mimeBytes == 0) return false;
    uint8_t mime[64];
    if (!artworkReadAt(file, mimeStart, mime, mimeBytes, control)) return false;
    size_t terminator = 0;
    while (terminator < mimeBytes && mime[terminator] != 0) terminator++;
    if (terminator == mimeBytes) return false;
    pictureTypePosition = mimeStart + terminator + 1;
    descriptionStart = pictureTypePosition + 1;
  }

  if (descriptionStart >= frameEnd) return false;
  uint8_t pictureType = 0xFF;
  if (!artworkReadAt(file, pictureTypePosition, &pictureType, 1, control))
    return false;

  uint64_t imageStart = 0;
  if (!findEncodedTextEnd(file, descriptionStart, frameEnd, encoding,
                          imageStart, control) ||
      imageStart >= frameEnd) {
    return false;
  }
  uint64_t imageLength = frameEnd - imageStart;
  if (imageLength > kArtworkMaximumBytes ||
      !jpegSignatureAt(file, imageStart, control)) {
    return false;
  }
  return setArtworkLocation(location, path, imageStart, imageLength,
                            pictureType);
}

bool locateMp3Artwork(MediaFsFile &file, const char *path,
                      ArtworkLocation &location,
                      const MediaArtworkControl *control)
{
  uint64_t fileSize = fileSizeLocked(file, control);
  uint8_t tagHeader[10];
  if (fileSize < sizeof(tagHeader) ||
      !artworkReadAt(file, 0, tagHeader, sizeof(tagHeader), control) ||
      std::memcmp(tagHeader, "ID3", 3) != 0) {
    return false;
  }

  const Mp3ArtworkPolicy::Id3TagInfo tag =
      Mp3ArtworkPolicy::inspectId3TagHeader(
          tagHeader, sizeof(tagHeader), fileSize, kMetadataMaximumBytes);
  if (tag.status != Mp3ArtworkPolicy::Id3TagStatus::Valid) return false;

  const uint8_t version = tag.version;
  const uint64_t tagEnd = tag.tagEnd;

  uint64_t cursor = 10;
  if (version >= 3 && (tagHeader[5] & 0x40)) {
    uint8_t sizeBytes[4];
    if (!artworkReadAt(file, cursor, sizeBytes, sizeof(sizeBytes), control))
      return false;
    uint32_t extendedSize = version == 4
                                ? readSynchsafe32(sizeBytes)
                                : readBe32(sizeBytes);
    uint64_t skip = version == 4 ? extendedSize : 4ULL + extendedSize;
    if (skip < 4 || cursor + skip > tagEnd) return false;
    cursor += skip;
  }

  ArtworkLocation fallback;
  for (uint16_t frameIndex = 0; frameIndex < 512 && cursor < tagEnd;
       frameIndex++) {
    bool id3v22 = version == 2;
    size_t headerSize = id3v22 ? 6 : 10;
    if (cursor + headerSize > tagEnd) break;

    uint8_t frameHeader[10] = {};
    if (!artworkReadAt(file, cursor, frameHeader, headerSize, control)) break;
    if (frameHeader[0] == 0) break;

    bool apic = id3v22
                    ? std::memcmp(frameHeader, "PIC", 3) == 0
                    : std::memcmp(frameHeader, "APIC", 4) == 0;
    uint32_t frameSize = id3v22
                             ? ((uint32_t)frameHeader[3] << 16) |
                                   ((uint32_t)frameHeader[4] << 8) |
                                   frameHeader[5]
                             : (version == 4
                                    ? readSynchsafe32(frameHeader + 4)
                                    : readBe32(frameHeader + 4));
    if (version == 4) {
      for (uint8_t i = 4; i < 8; i++) {
        if (frameHeader[i] & 0x80) return false;
      }
    }

    uint64_t frameStart = cursor + headerSize;
    uint64_t next = frameStart + frameSize;
    if (frameSize == 0 || next > tagEnd) break;

    bool transformed = false;
    if (!id3v22 && version == 3) {
      transformed = (frameHeader[9] & 0xE0) != 0;
    } else if (!id3v22 && version == 4) {
      transformed = (frameHeader[9] & 0x4F) != 0;
    }

    if (apic && !transformed) {
      ArtworkLocation candidate;
      if (locateId3ApicPayload(file, path, frameStart, frameSize,
                               id3v22, candidate, control)) {
        if (candidate.pictureType == 3) {
          location = candidate;
          return true;
        }
        if (!fallback.length) fallback = candidate;
      }
    }
    cursor = next;
  }

  if (fallback.length) {
    location = fallback;
    return true;
  }
  return false;
}

bool locateFlacPicturePayload(MediaFsFile &file, const char *path,
                              uint64_t blockStart, uint32_t blockLength,
                              ArtworkLocation &location,
                              const MediaArtworkControl *control)
{
  uint64_t blockEnd = blockStart + blockLength;
  uint64_t cursor = blockStart;
  uint8_t number[4];

  if (blockLength < 32 ||
      !artworkReadAt(file, cursor, number, 4, control)) return false;
  uint8_t pictureType = (uint8_t)std::min<uint32_t>(readBe32(number), 0xFF);
  cursor += 4;

  if (!artworkReadAt(file, cursor, number, 4, control)) return false;
  uint32_t mimeLength = readBe32(number);
  cursor += 4;
  if (cursor + mimeLength + 4 > blockEnd) return false;
  cursor += mimeLength;

  if (!artworkReadAt(file, cursor, number, 4, control)) return false;
  uint32_t descriptionLength = readBe32(number);
  cursor += 4;
  if (cursor + descriptionLength + 20 > blockEnd) return false;
  cursor += descriptionLength;

  // Width, height, depth and indexed-colour count.
  cursor += 16;
  if (!artworkReadAt(file, cursor, number, 4, control)) return false;
  uint32_t imageLength = readBe32(number);
  cursor += 4;
  if (imageLength < 3 || imageLength > kArtworkMaximumBytes ||
      cursor + imageLength > blockEnd ||
      !jpegSignatureAt(file, cursor, control)) {
    return false;
  }
  return setArtworkLocation(location, path, cursor, imageLength,
                            pictureType);
}

bool locateFlacArtwork(MediaFsFile &file, const char *path,
                       ArtworkLocation &location,
                       const MediaArtworkControl *control)
{
  uint64_t fileSize = fileSizeLocked(file, control);
  uint64_t cursor = 0;
  uint8_t signature[4];
  if (!findFlacStreamOffset(file, cursor, control) ||
      cursor + 4 > fileSize ||
      !artworkReadAt(file, cursor, signature, sizeof(signature), control) ||
      std::memcmp(signature, "fLaC", 4) != 0) {
    return false;
  }
  cursor += 4;

  ArtworkLocation fallback;
  for (uint8_t blockIndex = 0; blockIndex < 64 && cursor + 4 <= fileSize;
       blockIndex++) {
    uint8_t blockHeader[4];
    if (!artworkReadAt(file, cursor, blockHeader, sizeof(blockHeader), control))
      break;
    bool last = (blockHeader[0] & 0x80) != 0;
    uint8_t type = blockHeader[0] & 0x7F;
    uint32_t blockLength = ((uint32_t)blockHeader[1] << 16) |
                           ((uint32_t)blockHeader[2] << 8) |
                           blockHeader[3];
    uint64_t blockStart = cursor + 4;
    uint64_t next = blockStart + blockLength;
    if (next > fileSize) return false;

    if (type == 6) {
      ArtworkLocation candidate;
      if (locateFlacPicturePayload(file, path, blockStart, blockLength,
                                   candidate, control)) {
        if (candidate.pictureType == 3) {
          location = candidate;
          return true;
        }
        if (!fallback.length) fallback = candidate;
      }
    }
    cursor = next;
    if (last) break;
  }

  if (fallback.length) {
    location = fallback;
    return true;
  }
  return false;
}

bool buildSiblingPath(const char *audioPath, const char *fileName,
                      char *output, size_t outputSize)
{
  if (!audioPath || !fileName || !output || outputSize == 0) return false;
  const char *slash = std::strrchr(audioPath, '/');
  size_t directoryLength = slash ? (size_t)(slash - audioPath) : 0;
  int written = 0;
  if (!slash || directoryLength == 0) {
    written = snprintf(output, outputSize, "/%s", fileName);
  } else {
    written = snprintf(output, outputSize, "%.*s/%s",
                       (int)directoryLength, audioPath, fileName);
  }
  return written > 0 && (size_t)written < outputSize;
}

bool locateFolderArtwork(const char *audioPath, ArtworkLocation &location,
                         const MediaArtworkControl *control)
{
  static const char *const candidates[] = {
      "cover.jpg", "Cover.jpg", "folder.jpg", "Folder.jpg",
      "cover.jpeg", "Cover.jpeg"};
  for (const char *candidateName : candidates) {
    if (!waitForArtworkPermit(control)) return false;
    char candidatePath[MEDIA_FS_PATH_CAPACITY];
    if (!buildSiblingPath(audioPath, candidateName, candidatePath,
                          sizeof(candidatePath))) {
      continue;
    }

    SharedSpiGuard guard;
    MediaFsFile file = mediaFs.open(candidatePath);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      continue;
    }
    uint64_t size = file.size();
    uint8_t signature[3] = {};
    bool jpeg = size >= sizeof(signature) &&
                size <= kArtworkMaximumBytes &&
                file.seek(0) &&
                file.read(signature, sizeof(signature)) == sizeof(signature) &&
                signature[0] == 0xFF && signature[1] == 0xD8 &&
                signature[2] == 0xFF;
    file.close();
    if (jpeg && setArtworkLocation(location, candidatePath, 0, size)) {
      return true;
    }
  }
  return false;
}

bool readArtworkLocation(const ArtworkLocation &location, uint8_t **data,
                         size_t *length,
                         const MediaArtworkControl *control)
{
  if (!data || !length || !location.length) return false;
  *data = nullptr;
  *length = 0;

  uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(
      location.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    buffer = static_cast<uint8_t *>(
        heap_caps_malloc(location.length, MALLOC_CAP_8BIT));
  }
  if (!buffer) return false;

  MediaFsFile artworkFile;
  bool artworkOpen = false;
  uint64_t artworkFileSize = 0;
  {
    if (!waitForArtworkPermit(control)) {
      heap_caps_free(buffer);
      return false;
    }
    SharedSpiGuard guard;
    artworkFile = mediaFs.open(location.path);
    artworkOpen = artworkFile && !artworkFile.isDirectory();
    if (artworkOpen) artworkFileSize = artworkFile.size();
  }
  if (!artworkOpen ||
      (uint64_t)location.offset + location.length > artworkFileSize) {
    if (artworkFile) {
      SharedSpiGuard guard;
      artworkFile.close();
    }
    heap_caps_free(buffer);
    return false;
  }

  size_t copied = 0;
  while (copied < location.length) {
    size_t chunk = std::min(kArtworkReadChunkBytes,
                            (size_t)location.length - copied);
    if (!artworkReadAt(artworkFile,
                       (uint64_t)location.offset + copied,
                       buffer + copied, chunk, control)) {
      break;
    }
    copied += chunk;
    vTaskDelay(1);
  }
  {
    SharedSpiGuard guard;
    artworkFile.close();
  }

  if (copied != location.length ||
      buffer[0] != 0xFF || buffer[1] != 0xD8 || buffer[2] != 0xFF) {
    heap_caps_free(buffer);
    return false;
  }

  *data = buffer;
  *length = location.length;
  return true;
}

size_t decoderRead(void *context, void *output, size_t bytesToRead)
{
  DecoderIoContext *io = static_cast<DecoderIoContext *>(context);
  if (!io) return 0;
  return cooperativeDecoderRead(io->file, io->cancelRequested,
                                io->readTelemetry, output, bytesToRead);
}

drwav_bool32 wavSeek(void *context, int offset, drwav_seek_origin origin)
{
  DecoderIoContext *io = static_cast<DecoderIoContext *>(context);
  if (!io || !io->file || !*io->file ||
      atomicFlagIsSet(io->cancelRequested)) {
    return DRWAV_FALSE;
  }
  return seekFile(*io->file, offset, (int)origin)
             ? DRWAV_TRUE : DRWAV_FALSE;
}

drwav_bool32 wavTell(void *context, drwav_int64 *cursor)
{
  DecoderIoContext *io = static_cast<DecoderIoContext *>(context);
  if (!io || !io->file || !*io->file || !cursor ||
      atomicFlagIsSet(io->cancelRequested)) {
    return DRWAV_FALSE;
  }
  SharedSpiGuard guard;
  *cursor = (drwav_int64)io->file->position();
  return DRWAV_TRUE;
}

size_t flacRead(void *context, void *output, size_t bytesToRead)
{
  FlacStreamContext *stream = static_cast<FlacStreamContext *>(context);
  if (!stream) return 0;
  return cooperativeDecoderRead(stream->file, stream->cancelRequested,
                                stream->readTelemetry, output, bytesToRead);
}

drflac_bool32 flacSeek(void *context, int offset, drflac_seek_origin origin)
{
  FlacStreamContext *stream = static_cast<FlacStreamContext *>(context);
  if (!stream || !stream->file || !*stream->file ||
      atomicFlagIsSet(stream->cancelRequested)) {
    return DRFLAC_FALSE;
  }

  SharedSpiGuard guard;
  if (atomicFlagIsSet(stream->cancelRequested)) return DRFLAC_FALSE;
  int64_t base = 0;
  if (origin == DRFLAC_SEEK_SET) {
    base = (int64_t)stream->baseOffset;
  } else if (origin == DRFLAC_SEEK_CUR) {
    base = (int64_t)stream->file->position();
  } else if (origin == DRFLAC_SEEK_END) {
    base = (int64_t)stream->file->size();
  } else {
    return DRFLAC_FALSE;
  }

  int64_t target = base + (int64_t)offset;
  if (target < (int64_t)stream->baseOffset ||
      (uint64_t)target > stream->file->size()) {
    return DRFLAC_FALSE;
  }
  return stream->file->seek((uint64_t)target) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 flacTell(void *context, drflac_int64 *cursor)
{
  FlacStreamContext *stream = static_cast<FlacStreamContext *>(context);
  if (!stream || !stream->file || !*stream->file || !cursor ||
      atomicFlagIsSet(stream->cancelRequested)) {
    return DRFLAC_FALSE;
  }

  SharedSpiGuard guard;
  if (atomicFlagIsSet(stream->cancelRequested)) return DRFLAC_FALSE;
  uint64_t physical = stream->file->position();
  if (physical < stream->baseOffset) return DRFLAC_FALSE;
  *cursor = (drflac_int64)(physical - stream->baseOffset);
  return DRFLAC_TRUE;
}

// Open the decoder on a logical stream that begins at the real native FLAC or
// Ogg FLAC marker. This makes probing and playback agree and fixes valid files
// carrying a leading ID3v2 tag.
drflac *openFlacStream(MediaFsFile &file, FlacStreamContext &stream)
{
  uint64_t baseOffset = 0;
  if (!findFlacStreamOffset(file, baseOffset)) return nullptr;

  stream.file = &file;
  stream.baseOffset = baseOffset;
  {
    SharedSpiGuard guard;
    if (!file.seek(baseOffset)) return nullptr;
  }
  return drflac_open(flacRead, flacSeek, flacTell, &stream, nullptr);
}

drmp3_bool32 mp3Seek(void *context, int offset, drmp3_seek_origin origin)
{
  DecoderIoContext *io = static_cast<DecoderIoContext *>(context);
  if (!io || !io->file || !*io->file ||
      atomicFlagIsSet(io->cancelRequested)) {
    return DRMP3_FALSE;
  }
  return seekFile(*io->file, offset, (int)origin)
             ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 mp3Tell(void *context, drmp3_int64 *cursor)
{
  DecoderIoContext *io = static_cast<DecoderIoContext *>(context);
  if (!io || !io->file || !*io->file || !cursor ||
      atomicFlagIsSet(io->cancelRequested)) {
    return DRMP3_FALSE;
  }
  SharedSpiGuard guard;
  *cursor = (drmp3_int64)io->file->position();
  return DRMP3_TRUE;
}

}  // namespace

void mediaSharedSpiLock()
{
  SemaphoreHandle_t mutex = sharedSpiMutex();
  while (!mutex) {
    vTaskDelay(pdMS_TO_TICKS(10));
    mutex = sharedSpiMutex();
  }

  // The LCD and SD card share one SPI bus. Wait cooperatively rather than
  // restarting the panel on normal contention. Media tasks are never forcibly
  // deleted, so a task cannot be abandoned while it owns this recursive mutex.
  uint32_t waits = 0;
  while (xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(kSpiLockTimeoutMs)) !=
         pdTRUE) {
    waits++;
    Serial.printf("MEDIA SPI: waiting for shared bus (%lu ms)\n",
                  (unsigned long)(waits * kSpiLockTimeoutMs));
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

bool mediaSharedSpiTryLock(uint32_t timeoutMs)
{
  SemaphoreHandle_t mutex = sharedSpiMutex();
  if (!mutex) return false;
  TickType_t ticks = timeoutMs ? pdMS_TO_TICKS(timeoutMs) : 0;
  return xSemaphoreTakeRecursive(mutex, ticks) == pdTRUE;
}

void mediaSharedSpiUnlock()
{
  SemaphoreHandle_t mutex = sharedSpiMutex();
  if (mutex) xSemaphoreGiveRecursive(mutex);
}

struct MediaPlayerPoC::SeekCommand {
  uint64_t targetFrame = 0;
  uint32_t generation = 0;
  bool resumePaused = false;
};

enum class MediaPlayerPoC::Mp3SeekAttemptResult : uint8_t {
  Completed = 0,
  Timeout,
  Stopped,
  Superseded,
  DecodeFailure,
  HeapIntegrityFailure,
  OpenFailure,
  AllocationFailure,
  PrefillFailure
};

struct MediaPlayerPoC::Mp3SeekAttempt {
  Mp3SeekAttemptResult result = Mp3SeekAttemptResult::DecodeFailure;
  uint64_t framesScanned = 0;
  uint32_t elapsedMs = 0;
};

struct MediaPlayerPoC::DecoderState {
  MediaFsFile file;
  DecoderIoContext io;
  FlacStreamContext flacStream;
  drwav wav = {};
  drflac *flac = nullptr;
  drmp3 mp3 = {};
  bool wavOpen = false;
  bool mp3Open = false;
  drwav_int32 s32[kDecodeChunkFrames * 2] = {};
  drmp3_int16 s16[kDecodeChunkFrames * 2] = {};
  int32_t stereo[kDecodeChunkFrames * 2] = {};
};

bool MediaPlayerPoC::begin(SPIClass &spi, int8_t chipSelectPin, int8_t clockPin,
                           int8_t misoPin, int8_t mosiPin)
{
  if (!seekCommandQueue) {
    seekCommandQueue = xQueueCreate(1, sizeof(SeekCommand));
  }
  if (!seekEventQueue) {
    seekEventQueue = xQueueCreate(1, sizeof(MediaSeekEvent));
  }
  if (!mediaControlEvents) {
    mediaControlEvents = xEventGroupCreate();
  }
  if (!seekCommandQueue || !seekEventQueue || !mediaControlEvents) {
    Serial.println("MEDIA INIT: control allocation failed");
    return false;
  }

  mediaSpi = &spi;
  mediaChipSelectPin = chipSelectPin;
  mediaClockPin = clockPin;
  mediaMisoPin = misoPin;
  mediaMosiPin = mosiPin;
  xQueueReset(seekCommandQueue);
  xQueueReset(seekEventQueue);
  xEventGroupClearBits(mediaControlEvents,
                       kSeekActiveBit | kOutputQuiescentBit |
                       kExternalHoldBit);
  suppressSpdifTimeoutsUntil = 0;

  // Media subsystem initialisation is deliberately separate from card mount.
  // A full-flash reset is only a warm reset for the separately powered SD card;
  // touching it under the boot logo made the first boot depend on peripheral
  // timing. Keep both chip selects inactive and mount only when Media is opened.
  pinMode(mediaChipSelectPin, OUTPUT);
  digitalWrite(mediaChipSelectPin, HIGH);
  cardMounted = false;
  mountedCardType = MEDIA_CARD_NONE;
  mountedCardSizeBytes = 0;
  mountedFrequencyHz = 0;
  Serial.println("MEDIA INIT: ready; SD mount deferred until Media selection");
  return true;
}

bool MediaPlayerPoC::mountCard(uint32_t lockTimeoutMs)
{
  return mountCardWithMode(MediaFsAccessMode::NormalReadOnly,
                           lockTimeoutMs);
}

bool MediaPlayerPoC::mountCardForTransfer(uint32_t lockTimeoutMs)
{
  return mountCardWithMode(MediaFsAccessMode::TransferReadWrite,
                           lockTimeoutMs);
}

bool MediaPlayerPoC::switchMountedCardAccessMode(
    MediaFsAccessMode accessMode, uint32_t lockTimeoutMs)
{
  directoryPageCache.clear();
  if (accessMode == MediaFsAccessMode::Unmounted || !cardMounted ||
      !mediaFs.mounted()) {
    return false;
  }
  if (state != MediaPlaybackState::Stopped || decoder ||
      decoderTaskRunning() || outputTaskRunning()) {
    Serial.println(
        "MEDIA SD: access-mode switch rejected; playback resources active");
    return false;
  }

  TrySharedSpiGuard guard(lockTimeoutMs);
  if (!guard) {
    Serial.printf(
        "MEDIA SD: access-mode switch deferred; shared SPI busy timeout=%lu ms\n",
        static_cast<unsigned long>(lockTimeoutMs));
    return false;
  }

  const bool directoryWasOpen = static_cast<bool>(browserDirectoryScratch);
  const bool entryWasOpen = static_cast<bool>(browserEntryScratch);
  browserDirectoryScratch.close();
  browserEntryScratch.close();

  const bool usable = mediaFs.mountedCardUsable();
  if (!usable || !mediaFs.setAccessMode(accessMode)) {
    Serial.println("MEDIA SD: access-mode switch rejected; mount invalid");
    return false;
  }

  Serial.printf(
      "MEDIA SD: access mode switched to=%s browser_dir=%s browser_entry=%s\n",
      accessMode == MediaFsAccessMode::TransferReadWrite
          ? "transfer-rw" : "normal-ro",
      directoryWasOpen ? "closed" : "idle",
      entryWasOpen ? "closed" : "idle");
  return true;
}

bool MediaPlayerPoC::unmountCard(uint32_t lockTimeoutMs)
{
  directoryPageCache.clear();
  if (state != MediaPlaybackState::Stopped || decoder ||
      decoderTaskRunning() || outputTaskRunning()) {
    Serial.println("MEDIA SD: unmount rejected; playback resources active");
    return false;
  }

  // Never block loopTask indefinitely while the LCD/artwork task owns the
  // shared SPI bus.  The Wi-Fi lifecycle retries this bounded operation and
  // keeps the display responsive until every SD user has released the bus.
  if (!mediaSharedSpiTryLock(lockTimeoutMs)) {
    Serial.printf("MEDIA SD: unmount deferred; shared SPI busy timeout=%lu ms\n",
                  static_cast<unsigned long>(lockTimeoutMs));
    return false;
  }

  const bool directoryWasOpen = static_cast<bool>(browserDirectoryScratch);
  const bool entryWasOpen = static_cast<bool>(browserEntryScratch);
  browserDirectoryScratch.close();
  browserEntryScratch.close();
  mediaFs.end();
  mediaSharedSpiUnlock();

  cardMounted = false;
  mountedCardType = MEDIA_CARD_NONE;
  mountedCardSizeBytes = 0;
  mountedFrequencyHz = 0;
  Serial.printf("MEDIA SD: unmount complete browser_dir=%s browser_entry=%s\n",
                directoryWasOpen ? "closed" : "idle",
                entryWasOpen ? "closed" : "idle");
  return true;
}

bool MediaPlayerPoC::prepareCardForControllerRestart(
    uint32_t lockTimeoutMs)
{
  if (state != MediaPlaybackState::Stopped || decoder ||
      decoderTaskRunning() || outputTaskRunning()) {
    Serial.println(
        "MEDIA SD: restart preparation rejected; playback resources active");
    return false;
  }
  if (!mediaSpi || mediaChipSelectPin < 0 || mediaClockPin < 0 ||
      mediaMisoPin < 0 || mediaMosiPin < 0) {
    Serial.println(
        "MEDIA SD: restart preparation rejected; media bus unavailable");
    return false;
  }
  if (!mediaSharedSpiTryLock(lockTimeoutMs)) {
    Serial.printf(
        "MEDIA SD: restart preparation deferred; shared SPI busy timeout=%lu ms\n",
        static_cast<unsigned long>(lockTimeoutMs));
    return false;
  }

  browserDirectoryScratch.close();
  browserEntryScratch.close();
  mediaFs.end();

  SPIClass &spi = *mediaSpi;
  pinMode(mediaChipSelectPin, OUTPUT);
  digitalWrite(mediaChipSelectPin, HIGH);

  // Give a still-powered card a complete idle train before the ESP32 resets.
  // The v13.2.1 hardware trace showed ACMD41 error 0x17 after a warm restart
  // when the mounted card was left live across ESP.restart().
  spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (uint8_t clockByte = 0; clockByte < 32U; ++clockByte) {
    spi.transfer(0xFF);
  }
  spi.endTransaction();
  delay(2);
  spi.end();

  // Hold the bus in the SD SPI idle state until ROM/startup code takes over.
  pinMode(mediaClockPin, OUTPUT);
  digitalWrite(mediaClockPin, LOW);
  pinMode(mediaMosiPin, OUTPUT);
  digitalWrite(mediaMosiPin, HIGH);
  pinMode(mediaMisoPin, INPUT_PULLUP);
  digitalWrite(mediaChipSelectPin, HIGH);

  cardMounted = false;
  mountedCardType = MEDIA_CARD_NONE;
  mountedCardSizeBytes = 0;
  mountedFrequencyHz = 0;
  mediaSharedSpiUnlock();
  Serial.println(
      "MEDIA SD: prepared for controller restart; filesystem closed SPI idle");
  return true;
}

bool MediaPlayerPoC::mountCardWithMode(MediaFsAccessMode accessMode,
                                       uint32_t lockTimeoutMs)
{
  directoryPageCache.clear();
  if (accessMode == MediaFsAccessMode::Unmounted) return false;

  // Normal browsing and transfer writing use the same SdFs instance.  The
  // access mode is an application-level ownership gate, so a healthy mounted
  // card can change owner without spi.end()/spi.begin().  This avoids the
  // hardware freeze observed when a full-screen LCD DMA flush was followed by
  // an immediate SPI peripheral restart after browsing an album.
  if (cardMounted && mediaFs.mounted()) {
    TrySharedSpiGuard guard(lockTimeoutMs);
    if (!guard) {
      Serial.println("MEDIA SD: mounted mode switch deferred; shared SPI busy");
      return false;
    }
    browserDirectoryScratch.close();
    browserEntryScratch.close();
    const bool stillUsable = mediaFs.mountedCardUsable();
    if (stillUsable && mediaFs.setAccessMode(accessMode)) {
      Serial.printf("MEDIA SD: reused mounted card access=%s\n",
                    accessMode == MediaFsAccessMode::TransferReadWrite
                        ? "transfer-rw" : "normal-ro");
      return true;
    }

    Serial.println("MEDIA SD: cached mount invalid; remounting");
    mediaFs.end();
    cardMounted = false;
    mountedCardType = MEDIA_CARD_NONE;
    mountedCardSizeBytes = 0;
    mountedFrequencyHz = 0;
  }
  if (!cardMounted && mediaFs.mounted() && storageIoFault) {
    // A decoder-side I/O fault is not a deliberate unmount. Keep the panel
    // responsive, quarantine the stale mount and request a full power cycle.
    browserDirectoryScratch.close();
    browserEntryScratch.close();
    strlcpy(errorText, "SD read fault; power cycle required",
            sizeof(errorText));
    Serial.printf(
        "MEDIA SD: faulted mount quarantined clock=%lu Hz code=0x%02X "
        "data=0x%08lX; power cycle required\n",
        static_cast<unsigned long>(mountedFrequencyHz),
        static_cast<unsigned>(mediaFs.errorCode()),
        static_cast<unsigned long>(mediaFs.errorData()));
    return false;
  }
  if (!cardMounted && mediaFs.mounted()) {
    TrySharedSpiGuard guard(lockTimeoutMs);
    if (!guard) {
      Serial.println("MEDIA SD: stale mount release deferred; shared SPI busy");
      return false;
    }
    browserDirectoryScratch.close();
    browserEntryScratch.close();
    mediaFs.end();
  }
  if (!mediaSpi || mediaChipSelectPin < 0 || mediaClockPin < 0 ||
      mediaMisoPin < 0 || mediaMosiPin < 0) {
    Serial.println("MEDIA SD: mount rejected; media subsystem not initialized");
    return false;
  }

  TrySharedSpiGuard guard(lockTimeoutMs);
  if (!guard) {
    Serial.println("MEDIA SD: mount deferred; shared SPI busy");
    return false;
  }
  SPIClass &spi = *mediaSpi;
  const int8_t chipSelectPin = mediaChipSelectPin;
  cardMounted = false;
  mountedCardType = MEDIA_CARD_NONE;
  mountedCardSizeBytes = 0;
  mountedFrequencyHz = 0;

  // The LCD and SD share the bus. Every attempt starts from a known deselected
  // state, clocks the card idle at 400 kHz, and then lets SdFat negotiate.
  // This path is user-initiated and never delays the boot logo.
  for (uint32_t frequencyHz : kSdFrequenciesHz) {
    for (uint8_t attempt = 0; attempt < kSdMountAttemptsPerFrequency; attempt++) {
      mediaFs.end();
      pinMode(chipSelectPin, OUTPUT);
      digitalWrite(chipSelectPin, HIGH);
      spi.end();
      delay(attempt == 0 ? 20 : 80);
      spi.begin(mediaClockPin, mediaMisoPin, mediaMosiPin, chipSelectPin);

      spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
      digitalWrite(chipSelectPin, HIGH);
      for (uint8_t clockByte = 0; clockByte < 16; clockByte++) {
        spi.transfer(0xFF);
      }
      spi.endTransaction();
      delay(attempt == 0 ? 40 : 140);

      Serial.printf("MEDIA SD: mount attempt frequency=%lu Hz try=%u/%u\n",
                    (unsigned long)frequencyHz, (unsigned)(attempt + 1),
                    (unsigned)kSdMountAttemptsPerFrequency);
      if (!mediaFs.begin(chipSelectPin, spi, frequencyHz, "/sd", 8, false,
                         accessMode)) {
        Serial.printf(
            "MEDIA SD: mount failed frequency=%lu Hz code=0x%02X "
            "data=0x%08lX\n",
            (unsigned long)frequencyHz, (unsigned)mediaFs.errorCode(),
            (unsigned long)mediaFs.errorData());
        continue;
      }

      mountedCardType = mediaFs.cardType();
      mountedCardSizeBytes = mediaFs.cardSize();
      if (mountedCardType != MEDIA_CARD_NONE && mountedCardSizeBytes > 0) {
        cardMounted = true;
        mountedFrequencyHz = frequencyHz;
        Serial.printf("MEDIA SD: mount complete access=%s\n",
                      accessMode == MediaFsAccessMode::TransferReadWrite
                          ? "transfer-rw" : "normal-ro");
        return true;
      }

      Serial.printf("MEDIA SD: card recognised but metadata invalid "
                    "type=%u size=%llu\n",
                    (unsigned)mountedCardType,
                    (unsigned long long)mountedCardSizeBytes);
      mediaFs.end();
      mountedCardType = MEDIA_CARD_NONE;
      mountedCardSizeBytes = 0;
    }
  }

  mediaFs.end();
  pinMode(chipSelectPin, OUTPUT);
  digitalWrite(chipSelectPin, HIGH);
  spi.begin(mediaClockPin, mediaMisoPin, mediaMosiPin, chipSelectPin);
  Serial.println("MEDIA SD: no usable card after bounded mount attempts");
  return false;
}


bool MediaPlayerPoC::prepareAudioBuffer()
{
  return allocateRing();
}

bool MediaPlayerPoC::mounted() const
{
  return cardMounted;
}

MediaFsAccessMode MediaPlayerPoC::cardAccessMode() const
{
  return mediaFs.accessMode();
}

void MediaPlayerPoC::printCardSummary(Stream &out) const
{
  if (!cardMounted) {
    out.println("MEDIA SD: no card mounted");
    return;
  }

  const char *type = "UNKNOWN";
  if (mountedCardType == MEDIA_CARD_SDV1) type = "SDSC v1";
  else if (mountedCardType == MEDIA_CARD_SDV2) type = "SDSC v2";
  else if (mountedCardType == MEDIA_CARD_SDHC) type = "SDHC";
  else if (mountedCardType == MEDIA_CARD_SDXC) type = "SDXC";

  if (mediaFs.accessMode() == MediaFsAccessMode::TransferReadWrite) {
    out.printf("MEDIA SD: mounted type=%s filesystem=%s backend=SdFat-%s "
               "size=%llu MiB clock=%lu Hz access=transfer-rw\n",
               type, mediaFs.fileSystemName(), SD_FAT_VERSION_STR,
               (unsigned long long)(
                   mountedCardSizeBytes / (1024ULL * 1024ULL)),
               (unsigned long)mountedFrequencyHz);
  } else {
    // Preserve the established normal-mode line for monitor compatibility.
    out.printf("MEDIA SD: mounted type=%s filesystem=%s backend=SdFat-%s "
               "size=%llu MiB clock=%lu Hz read-policy=read-only\n",
               type, mediaFs.fileSystemName(), SD_FAT_VERSION_STR,
               (unsigned long long)(
                   mountedCardSizeBytes / (1024ULL * 1024ULL)),
               (unsigned long)mountedFrequencyHz);
  }
}

bool MediaPlayerPoC::isPlayablePath(const char *path)
{
  return endsWithIgnoreCase(path, ".wav") ||
         endsWithIgnoreCase(path, ".flac") ||
         endsWithIgnoreCase(path, ".mp3");
}

const char *MediaPlayerPoC::formatName(MediaFileFormat format)
{
  switch (format) {
    case MediaFileFormat::Wav: return "WAV";
    case MediaFileFormat::Flac: return "FLAC";
    case MediaFileFormat::Mp3: return "MP3";
    default: return "UNKNOWN";
  }
}

const char *MediaPlayerPoC::stateName(MediaPlaybackState state)
{
  switch (state) {
    case MediaPlaybackState::Starting: return "STARTING";
    case MediaPlaybackState::Playing: return "PLAYING";
    case MediaPlaybackState::Paused: return "PAUSED";
    case MediaPlaybackState::Seeking: return "SEEKING";
    case MediaPlaybackState::Draining: return "DRAINING";
    case MediaPlaybackState::Finished: return "FINISHED";
    case MediaPlaybackState::Error: return "ERROR";
    default: return "STOPPED";
  }
}

void MediaPlayerPoC::printDirectory(Stream &out, const char *path, size_t maxEntries)
{
  if (!cardMounted) {
    out.println("MEDIA LIST: card is not mounted");
    return;
  }

  MediaFsFile directory;
  {
    SharedSpiGuard guard;
    directory = mediaFs.open(path);
  }
  if (!directory || !directory.isDirectory()) {
    out.printf("MEDIA LIST: cannot open directory %s\n", path ? path : "(null)");
    return;
  }

  out.printf("MEDIA LIST: %s (maximum %u entries; no recursive scan)\n",
             path, (unsigned)maxEntries);

  size_t shown = 0;
  size_t playable = 0;
  bool truncated = false;
  while (shown < maxEntries) {
    char entryPath[MEDIA_FS_PATH_CAPACITY] = {0};
    bool directoryEntry = false;
    bool haveEntry = false;
    {
      SharedSpiGuard guard;
      MediaFsFile entry = directory.openNextFile();
      if (entry) {
        strlcpy(entryPath, entry.path(), sizeof(entryPath));
        directoryEntry = entry.isDirectory();
        entry.close();
        haveEntry = true;
      }
    }
    if (!haveEntry) break;

    if (directoryEntry) {
      out.printf("  DIR   %s\n", entryPath);
    } else if (isPlayablePath(entryPath)) {
      MediaFileInfo info;
      bool probed = probeFile(entryPath, info);
      playable++;
      if (probed) {
        out.printf("  %-4s  %6lu Hz  %2u-bit  %uch  %-11s  %s\n",
                   formatName(info.format),
                   (unsigned long)info.sampleRate,
                   info.bitsPerSample,
                   info.channels,
                   info.nativeRateSupported ? "NATIVE_OK" : "RATE_REJECT",
                   entryPath);
      } else {
        out.printf("  BAD   header probe failed                     %s\n", entryPath);
      }
    } else {
      out.printf("  FILE  %s\n", entryPath);
    }
    shown++;
  }

  if (shown == maxEntries) {
    SharedSpiGuard guard;
    MediaFsFile extra = directory.openNextFile();
    truncated = (bool)extra;
    if (extra) extra.close();
  }
  {
    SharedSpiGuard guard;
    directory.close();
  }
  out.printf("MEDIA LIST: shown=%u playable=%u%s\n",
             (unsigned)shown, (unsigned)playable,
             truncated ? " truncated=yes" : "");
}

size_t MediaPlayerPoC::listDirectoryPage(
    const char *path, MediaBrowserEntry *entries, size_t capacity,
    MediaDirectoryPageMode mode, const MediaBrowserEntry *anchor,
    MediaDirectoryPageInfo &pageInfo)
{
  pageInfo = MediaDirectoryPageInfo{};
  if (!entries || capacity == 0) return 0;
  for (size_t i = 0; i < capacity; i++) clearBrowserEntry(entries[i]);
  if (!cardMounted) { directoryPageCache.clear(); return 0; }
  if (!path || !path[0]) return 0;

  if (mode != MediaDirectoryPageMode::First && !anchor) {
    mode = MediaDirectoryPageMode::First;
  }

  const bool cacheAllowed =
      mediaFs.accessMode() == MediaFsAccessMode::NormalReadOnly;
  if (!cacheAllowed) directoryPageCache.clear();
  size_t cachedCount = 0;
  if (cacheAllowed && directoryPageCache.copy(
          path, capacity, static_cast<unsigned>(mode), anchor, entries,
          pageInfo, cachedCount)) {
    // Probe the mounted card, rather than allowing a cached page to conceal
    // an observed removal/read failure. No directory rescan is needed.
    SharedSpiGuard guard;
    if (cardMounted && mediaFs.mountedCardUsable() && !mediaFs.errorCode()) {
      return cachedCount;
    }
    directoryPageCache.clear();
    pageInfo = MediaDirectoryPageInfo{};
    for (size_t i = 0; i < capacity; ++i) clearBrowserEntry(entries[i]);
  }

  MediaFsFile &directory = browserDirectoryScratch;
  MediaFsFile &entry = browserEntryScratch;
  MediaBrowserEntry &candidate = browserCandidateScratch;
  entry.close();
  directory.close();
  clearBrowserEntry(candidate);
  {
    SharedSpiGuard guard;
    directory = mediaFs.open(path);
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      return 0;
    }
    if (!directory.rewindDirectory()) {
      Serial.printf("MEDIA FS: directory rewind failed path=%s\n", path);
      directory.close();
      return 0;
    }
  }

  const bool keepLargest =
      mode == MediaDirectoryPageMode::Before ||
      mode == MediaDirectoryPageMode::AtOrBefore;
  size_t count = 0;
  uint32_t entriesBeforeAnchor = 0;
  uint32_t entriesAfterAnchor = 0;
  uint32_t entriesEqualAnchor = 0;
  uint8_t emptyDirectoryReopenCount = 0;

  while (true) {
    entry.close();
    clearBrowserEntry(candidate);
    bool accepted = false;

    {
      SharedSpiGuard guard;
      uint32_t skippedEntries = 0;
      bool haveEntry = directory.openNextFile(entry, &skippedEntries);
      if (skippedEntries) {
        const uint32_t rawRoom = UINT32_MAX - pageInfo.rawEntriesScanned;
        pageInfo.rawEntriesScanned += std::min(rawRoom, skippedEntries);
        const uint32_t skipRoom = UINT32_MAX - pageInfo.skippedLongPaths;
        pageInfo.skippedLongPaths += std::min(skipRoom, skippedEntries);
      }
      if (!haveEntry) {
        bool reopened = false;
        if (pageInfo.rawEntriesScanned == 0U &&
            emptyDirectoryReopenCount == 0U) {
          entry.close();
          directory.close();
          directory = mediaFs.open(path);
          reopened = directory && directory.isDirectory() &&
                     directory.rewindDirectory();
          if (reopened) {
            emptyDirectoryReopenCount++;
            Serial.printf(
                "MEDIA FS: empty directory reopened once path=%s\n", path);
          } else if (directory) {
            directory.close();
          }
        }
        if (reopened) continue;
        break;
      }

      if (pageInfo.rawEntriesScanned != UINT32_MAX) {
        pageInfo.rawEntriesScanned++;
      }

      const char *entryPath = entry.path();
      const char *entryName = entry.name();
      candidate.directory = entry.isDirectory();
      candidate.format = MediaFileFormat::Unknown;
      if (!candidate.directory) {
        candidate.format = formatFromPath(entryPath);
      }
      // SdFat supplies both a full reconstructed path and the leaf name. The
      // path should normally be authoritative, but accepting a valid leaf
      // extension as a fallback prevents a harmless path-reconstruction edge
      // case from making an otherwise ordinary FLAC/WAV/MP3 folder look empty.
      if (!candidate.directory &&
          candidate.format == MediaFileFormat::Unknown) {
        candidate.format = formatFromPath(entryName);
      }

      const bool named = entryPath && entryPath[0] &&
                         entryName && entryName[0];
      const bool hidden = named && hiddenBrowserName(entryName);
      const bool supported = candidate.directory ||
                             candidate.format != MediaFileFormat::Unknown;
      if (!supported && !candidate.directory &&
          pageInfo.skippedUnsupportedFiles != UINT32_MAX) {
        pageInfo.skippedUnsupportedFiles++;
      }
      if (hidden && pageInfo.skippedHiddenEntries != UINT32_MAX) {
        pageInfo.skippedHiddenEntries++;
      }
      if (supported && named && !hidden) {
        if (std::strlen(entryPath) >= sizeof(candidate.path) ||
            std::strlen(entryName) >= sizeof(candidate.name)) {
          if (pageInfo.skippedLongPaths != UINT32_MAX) {
            pageInfo.skippedLongPaths++;
          }
        } else {
          strlcpy(candidate.path, entryPath, sizeof(candidate.path));
          strlcpy(candidate.name, entryName, sizeof(candidate.name));
          accepted = true;
        }
      }
      entry.close();
    }

    if ((pageInfo.rawEntriesScanned & 0x1FU) == 0) delay(0);
    if (!accepted) continue;

    if (pageInfo.totalEntries != UINT32_MAX) pageInfo.totalEntries++;

    int relation = anchor ? compareBrowserEntries(candidate, *anchor) : 1;
    if (anchor) {
      if (relation < 0 && entriesBeforeAnchor != UINT32_MAX) {
        entriesBeforeAnchor++;
      } else if (relation > 0 && entriesAfterAnchor != UINT32_MAX) {
        entriesAfterAnchor++;
      } else if (relation == 0 && entriesEqualAnchor != UINT32_MAX) {
        entriesEqualAnchor++;
      }
    }

    bool eligible = false;
    switch (mode) {
      case MediaDirectoryPageMode::First:
        eligible = true;
        break;
      case MediaDirectoryPageMode::After:
        eligible = relation > 0;
        break;
      case MediaDirectoryPageMode::Before:
        eligible = relation < 0;
        break;
      case MediaDirectoryPageMode::AtOrAfter:
        eligible = relation >= 0;
        break;
      case MediaDirectoryPageMode::AtOrBefore:
        eligible = relation <= 0;
        break;
    }
    if (!eligible) continue;

    if (pageInfo.eligibleEntries != UINT32_MAX) pageInfo.eligibleEntries++;
    insertBrowserPageCandidate(entries, count, capacity, candidate, keepLargest);
  }

  {
    SharedSpiGuard guard;
    entry.close();
    directory.close();
  }

  switch (mode) {
    case MediaDirectoryPageMode::First:
      pageInfo.firstEntryIndex = 0;
      pageInfo.hasPrevious = false;
      pageInfo.hasNext = pageInfo.eligibleEntries > count;
      break;
    case MediaDirectoryPageMode::After:
      pageInfo.firstEntryIndex =
          pageInfo.totalEntries - pageInfo.eligibleEntries;
      pageInfo.hasPrevious = entriesBeforeAnchor > 0 || entriesEqualAnchor > 0;
      pageInfo.hasNext = pageInfo.eligibleEntries > count;
      break;
    case MediaDirectoryPageMode::Before:
      pageInfo.firstEntryIndex = pageInfo.eligibleEntries > count
          ? pageInfo.eligibleEntries - count : 0;
      pageInfo.hasPrevious = pageInfo.eligibleEntries > count;
      pageInfo.hasNext = entriesAfterAnchor > 0 || entriesEqualAnchor > 0;
      break;
    case MediaDirectoryPageMode::AtOrAfter:
      pageInfo.firstEntryIndex = entriesBeforeAnchor;
      pageInfo.hasPrevious = entriesBeforeAnchor > 0;
      pageInfo.hasNext = pageInfo.eligibleEntries > count;
      break;
    case MediaDirectoryPageMode::AtOrBefore:
      pageInfo.firstEntryIndex = pageInfo.eligibleEntries > count
          ? pageInfo.eligibleEntries - count : 0;
      pageInfo.hasPrevious = pageInfo.eligibleEntries > count;
      pageInfo.hasNext = entriesAfterAnchor > 0;
      break;
  }
  if (cacheAllowed && cardMounted) {
    bool storageHealthy;
    {
      SharedSpiGuard guard;
      storageHealthy = !mediaFs.errorCode();
    }
    // PSRAM allocation/copy must not extend shared-SPI ownership.
    if (storageHealthy) {
      directoryPageCache.remember(path, capacity, static_cast<unsigned>(mode),
                                  anchor, entries, count, pageInfo);
    } else {
      directoryPageCache.clear();
    }
  }
  return count;
}

size_t MediaPlayerPoC::listRootPlayable(MediaPlayableEntry *entries,
                                        size_t capacity, size_t maxEntries)
{
  if (!entries || capacity == 0) return 0;
  for (size_t i = 0; i < capacity; i++) entries[i] = MediaPlayableEntry{};
  if (!cardMounted) return 0;

  SharedSpiGuard guard;
  MediaFsFile directory = mediaFs.open("/");
  if (!directory || !directory.isDirectory()) return 0;

  size_t count = 0;
  size_t shown = 0;
  MediaFsFile entry = directory.openNextFile();
  while (entry && shown < maxEntries) {
    const char *entryPath = entry.path();
    if (!entry.isDirectory() && isPlayablePath(entryPath)) {
      MediaFileInfo info;
      bool ok = probeFile(entryPath, info) &&
                info.nativeRateSupported &&
                (info.channels == 1 || info.channels == 2) &&
                sourceDepthSupported(info.format, info.bitsPerSample);
      if (ok && count < capacity) {
        strlcpy(entries[count].path, entryPath, sizeof(entries[count].path));
        entries[count].info = info;
        count++;
      }
    }

    entry.close();
    shown++;
    entry = directory.openNextFile();
  }

  if (entry) entry.close();
  directory.close();
  return count;
}

bool MediaPlayerPoC::loadArtworkJpeg(
    const char *audioPath, uint8_t **data, size_t *length,
    const MediaArtworkControl *control, MediaArtworkSource *source)
{
  if (data) *data = nullptr;
  if (length) *length = 0;
  if (source) *source = MediaArtworkSource::None;
  ArtworkLocation location;
  if (!data || !length || !cardMounted || !audioPath || !audioPath[0] ||
      std::strlen(audioPath) >= sizeof(location.path)) {
    return false;
  }

  MediaArtworkSource selectedSource = MediaArtworkSource::None;
  MediaFsFile audioFile;
  bool audioOpen = false;
  {
    if (!waitForArtworkPermit(control)) return false;
    SharedSpiGuard guard;
    audioFile = mediaFs.open(audioPath);
    audioOpen = audioFile && !audioFile.isDirectory();
  }

  if (audioOpen) {
    if (endsWithIgnoreCase(audioPath, ".mp3") &&
        locateMp3Artwork(audioFile, audioPath, location, control)) {
      selectedSource = MediaArtworkSource::Mp3Apic;
    } else if (endsWithIgnoreCase(audioPath, ".flac") &&
               locateFlacArtwork(audioFile, audioPath, location, control)) {
      selectedSource = MediaArtworkSource::FlacPicture;
    }
  }
  if (audioFile) {
    SharedSpiGuard guard;
    audioFile.close();
  }

  if (!location.length) {
    if (!locateFolderArtwork(audioPath, location, control)) return false;
    selectedSource = MediaArtworkSource::FolderJpeg;
  }

  if (!readArtworkLocation(location, data, length, control)) return false;
  if (source) *source = selectedSource;
  return true;
}

bool MediaPlayerPoC::loadFolderArtworkJpeg(
    const char *audioPath, uint8_t **data, size_t *length,
    const MediaArtworkControl *control)
{
  if (data) *data = nullptr;
  if (length) *length = 0;
  if (!data || !length || !cardMounted || !audioPath || !audioPath[0]) {
    return false;
  }
  ArtworkLocation location;
  return locateFolderArtwork(audioPath, location, control) &&
         readArtworkLocation(location, data, length, control);
}

const char *MediaPlayerPoC::artworkSourceName(MediaArtworkSource source)
{
  switch (source) {
    case MediaArtworkSource::Mp3Apic: return "mp3-apic";
    case MediaArtworkSource::FlacPicture: return "flac-picture";
    case MediaArtworkSource::FolderJpeg: return "folder-jpeg";
    case MediaArtworkSource::None: return "none";
  }
  return "unknown";
}

void MediaPlayerPoC::freeArtworkJpeg(uint8_t *data)
{
  if (data) heap_caps_free(data);
}

bool MediaPlayerPoC::probeFile(const char *path, MediaFileInfo &info)
{
  info = MediaFileInfo{};
  if (!cardMounted || !path) return false;

  SharedSpiGuard guard;
  MediaFsFile file = mediaFs.open(path);
  if (!file || file.isDirectory()) return false;

  bool ok = false;
  if (endsWithIgnoreCase(path, ".wav")) ok = probeWav(file, info);
  else if (endsWithIgnoreCase(path, ".flac")) ok = probeFlac(file, info);
  else if (endsWithIgnoreCase(path, ".mp3")) ok = probeMp3(file, info);

  file.close();
  if (ok) {
    info.valid = true;
    info.nativeRateSupported = nativeRateSupported(info.sampleRate);
  }
  return ok;
}

bool MediaPlayerPoC::probeWav(MediaFsFile &file, MediaFileInfo &info)
{
  uint8_t header[12];
  file.seek(0);
  if (!readExact(file, header, sizeof(header))) return false;
  if (std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "WAVE", 4) != 0) return false;

  bool foundFormat = false;
  for (uint8_t chunkIndex = 0; chunkIndex < 32 && file.available(); chunkIndex++) {
    uint8_t chunkHeader[8];
    if (!readExact(file, chunkHeader, sizeof(chunkHeader))) break;
    uint32_t chunkSize = readLe32(chunkHeader + 4);
    uint64_t chunkDataPosition = file.position();

    if (std::memcmp(chunkHeader, "fmt ", 4) == 0 && chunkSize >= 16) {
      uint8_t format[16];
      if (!readExact(file, format, sizeof(format))) return false;
      uint16_t encoding = readLe16(format);
      if (encoding != 1 && encoding != 3 && encoding != 0xFFFE) return false;
      info.format = MediaFileFormat::Wav;
      info.channels = (uint8_t)readLe16(format + 2);
      info.sampleRate = readLe32(format + 4);
      info.bitsPerSample = (uint8_t)readLe16(format + 14);
      if (encoding == 0xFFFE) {
        if (chunkSize < 40) return false;
        uint8_t extensible[24];
        if (!readExact(file, extensible, sizeof(extensible))) return false;
        uint16_t validBits = readLe16(extensible + 2);
        if (validBits) info.bitsPerSample = (uint8_t)validBits;
      }
      foundFormat = info.channels > 0 && info.sampleRate > 0 &&
                    info.bitsPerSample > 0;
    }

    uint64_t next = (uint64_t)chunkDataPosition + chunkSize + (chunkSize & 1U);
    if (next > file.size()) break;
    if (!file.seek(next)) break;
    if (foundFormat) return true;
  }
  return false;
}

bool MediaPlayerPoC::probeFlac(MediaFsFile &file, MediaFileInfo &info)
{
  uint64_t streamOffset = 0;
  MediaFileInfo streamInfo;
  bool haveNativeStreamInfo =
      findFlacStreamOffset(file, streamOffset) &&
      readNativeFlacStreamInfo(file, streamOffset, streamInfo);

  FlacStreamContext stream;
  drflac *flac = openFlacStream(file, stream);
  if (!flac) {
    Serial.printf("MEDIA FLAC: decoder probe failed path=%s size=%llu\n",
                  file.path(), (unsigned long long)file.size());
    return false;
  }
  if (stream.baseOffset > 0) {
    Serial.printf("MEDIA FLAC: skipped %llu leading metadata bytes path=%s\n",
                  (unsigned long long)stream.baseOffset, file.path());
  }

  MediaFileInfo decoderInfo;
  decoderInfo.format = MediaFileFormat::Flac;
  decoderInfo.sampleRate = flac->sampleRate;
  decoderInfo.channels = flac->channels;
  decoderInfo.bitsPerSample = flac->bitsPerSample;
  decoderInfo.totalFrames = flac->totalPCMFrameCount;
  decoderInfo.valid = decoderInfo.sampleRate > 0 &&
                      decoderInfo.channels > 0 &&
                      decoderInfo.bitsPerSample > 0;
  decoderInfo.nativeRateSupported =
      nativeRateSupported(decoderInfo.sampleRate);

  if (haveNativeStreamInfo) {
    info = streamInfo;
    if (decoderInfo.sampleRate != streamInfo.sampleRate ||
        decoderInfo.channels != streamInfo.channels ||
        decoderInfo.bitsPerSample != streamInfo.bitsPerSample) {
      Serial.printf(
          "MEDIA FLAC: STREAMINFO/decoder mismatch path=%s "
          "stream=%luHz/%ubit/%uch decoder=%luHz/%ubit/%uch; "
          "using STREAMINFO\n",
          file.path(), (unsigned long)streamInfo.sampleRate,
          streamInfo.bitsPerSample, streamInfo.channels,
          (unsigned long)decoderInfo.sampleRate,
          decoderInfo.bitsPerSample, decoderInfo.channels);
    }
  } else {
    info = decoderInfo;
  }
  drflac_close(flac);

  return info.sampleRate > 0 && info.channels > 0 &&
         info.bitsPerSample > 0;
}

bool MediaPlayerPoC::probeMp3(MediaFsFile &file, MediaFileInfo &info)
{
  file.seek(0);
  uint8_t firstTen[10];
  uint32_t scanStart = 0;
  if (readExact(file, firstTen, sizeof(firstTen)) &&
      std::memcmp(firstTen, "ID3", 3) == 0) {
    for (uint8_t i = 6; i < 10; i++) {
      if (firstTen[i] & 0x80) return false;
    }
    uint32_t tagSize = ((uint32_t)firstTen[6] << 21) |
                       ((uint32_t)firstTen[7] << 14) |
                       ((uint32_t)firstTen[8] << 7) |
                       firstTen[9];
    scanStart = 10 + tagSize + ((firstTen[5] & 0x10) ? 10 : 0);
  }

  if (scanStart >= file.size() || !file.seek(scanStart)) return false;
  size_t remaining = (size_t)std::min<uint64_t>(kMp3ScanLimit,
                                                file.size() - scanStart);
  if (remaining < 4) return false;

  uint8_t window[4];
  if (!readExact(file, window, sizeof(window))) return false;
  remaining -= sizeof(window);

  while (true) {
    uint32_t header = ((uint32_t)window[0] << 24) |
                      ((uint32_t)window[1] << 16) |
                      ((uint32_t)window[2] << 8) |
                      window[3];
    bool sync = (header & 0xFFE00000U) == 0xFFE00000U;
    uint8_t version = (header >> 19) & 0x03;
    uint8_t layer = (header >> 17) & 0x03;
    uint8_t bitrateIndex = (header >> 12) & 0x0F;
    uint8_t rateIndex = (header >> 10) & 0x03;

    if (sync && version != 1 && layer == 1 &&
        bitrateIndex != 0 && bitrateIndex != 15 && rateIndex != 3) {
      static const uint32_t mpeg1Rates[3] = {44100, 48000, 32000};
      uint32_t rate = mpeg1Rates[rateIndex];
      if (version == 2) rate /= 2;
      else if (version == 0) rate /= 4;

      info.format = MediaFileFormat::Mp3;
      info.sampleRate = rate;
      info.channels = ((header >> 6) & 0x03) == 3 ? 1 : 2;
      info.bitsPerSample = 16;
      return true;
    }

    if (remaining == 0) break;
    int next = file.read();
    if (next < 0) break;
    window[0] = window[1];
    window[1] = window[2];
    window[2] = window[3];
    window[3] = (uint8_t)next;
    remaining--;
  }
  return false;
}

bool MediaPlayerPoC::openDecoder(const char *path, MediaFileInfo &info)
{
  closeDecoder();
  return openDecoderState(path, decoder, info);
}

bool MediaPlayerPoC::openDecoderState(const char *path, DecoderState *&target,
                                      MediaFileInfo &info)
{
  closeDecoderState(target);
  target = new (std::nothrow) DecoderState();
  if (!target) return false;

  {
    SharedSpiGuard guard;
    target->file = mediaFs.open(path);
    if (!target->file || target->file.isDirectory()) {
      closeDecoderState(target);
      return false;
    }
  }

  target->io.file = &target->file;
  target->io.cancelRequested = &stopRequested;
  target->io.readTelemetry.stats = &stats;
  target->flacStream.cancelRequested = &stopRequested;
  target->flacStream.readTelemetry.stats = &stats;

  info = MediaFileInfo{};
  if (endsWithIgnoreCase(path, ".wav")) {
    target->wavOpen = drwav_init(&target->wav, decoderRead, wavSeek, wavTell,
                                 &target->io, nullptr) == DRWAV_TRUE;
    if (target->wavOpen) {
      info.format = MediaFileFormat::Wav;
      info.sampleRate = target->wav.sampleRate;
      info.channels = (uint8_t)target->wav.channels;
      info.bitsPerSample = target->wav.fmt.validBitsPerSample
                               ? (uint8_t)target->wav.fmt.validBitsPerSample
                               : (uint8_t)target->wav.bitsPerSample;
      info.totalFrames = target->wav.totalPCMFrameCount;
    }
  } else if (endsWithIgnoreCase(path, ".flac")) {
    uint64_t streamOffset = 0;
    MediaFileInfo streamInfo;
    bool haveNativeStreamInfo =
        findFlacStreamOffset(target->file, streamOffset) &&
        readNativeFlacStreamInfo(target->file, streamOffset, streamInfo);

    target->flac = openFlacStream(target->file, target->flacStream);
    if (target->flac) {
      if (haveNativeStreamInfo) {
        info = streamInfo;
        if (target->flac->sampleRate != streamInfo.sampleRate ||
            target->flac->channels != streamInfo.channels ||
            target->flac->bitsPerSample != streamInfo.bitsPerSample) {
          Serial.printf(
              "MEDIA FLAC: playback metadata corrected path=%s "
              "stream=%luHz/%ubit/%uch decoder=%luHz/%ubit/%uch\n",
              path, (unsigned long)streamInfo.sampleRate,
              streamInfo.bitsPerSample, streamInfo.channels,
              (unsigned long)target->flac->sampleRate,
              target->flac->bitsPerSample, target->flac->channels);
        }
      } else {
        info.format = MediaFileFormat::Flac;
        info.sampleRate = target->flac->sampleRate;
        info.channels = target->flac->channels;
        info.bitsPerSample = target->flac->bitsPerSample;
        info.totalFrames = target->flac->totalPCMFrameCount;
      }
    }
  } else if (endsWithIgnoreCase(path, ".mp3")) {
    target->mp3Open =
        drmp3_init(&target->mp3, decoderRead, mp3Seek, mp3Tell, nullptr,
                   &target->io, nullptr) == DRMP3_TRUE;
    if (target->mp3Open) {
      info.format = MediaFileFormat::Mp3;
      info.sampleRate = target->mp3.sampleRate;
      info.channels = (uint8_t)target->mp3.channels;
      info.bitsPerSample = 16;
      // Xing/Info headers provide this immediately. Do not force a full-file
      // scan when the stream has no native frame count; zero continues to mean
      // "duration unknown" for long files without an index.
      info.totalFrames =
          target->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX
              ? drmp3_get_pcm_frame_count(&target->mp3)
              : 0;
    }
  }

  info.valid = info.format != MediaFileFormat::Unknown &&
               info.sampleRate > 0 && info.channels > 0;
  info.nativeRateSupported = nativeRateSupported(info.sampleRate);
  if (!info.valid || !info.nativeRateSupported ||
      (info.channels != 1 && info.channels != 2) ||
      !sourceDepthSupported(info.format, info.bitsPerSample)) {
    closeDecoderState(target);
    return false;
  }
  return true;
}

bool MediaPlayerPoC::seekDecoderToFrame(uint64_t frame, uint32_t generation)
{
  if (!decoder) return false;
  if (decoder->wavOpen) {
    return drwav_seek_to_pcm_frame(&decoder->wav, frame) == DRWAV_TRUE;
  }
  if (decoder->flac) {
    return drflac_seek_to_pcm_frame(decoder->flac, frame) == DRFLAC_TRUE;
  }
  // MP3 is intentionally excluded. performSeek() uses an isolated candidate
  // decoder so a failed unindexed scan can never move the live decoder.
  return false;
}

MediaPlayerPoC::Mp3SeekAttempt MediaPlayerPoC::seekMp3CandidateToFrame(
    DecoderState &candidate, uint64_t frame, uint32_t generation,
    uint32_t budgetMs)
{
  Mp3SeekAttempt attempt;
  const uint32_t startedAt = millis();
  if (!candidate.mp3Open) {
    attempt.result = Mp3SeekAttemptResult::OpenFailure;
    return attempt;
  }

  drmp3 &mp3 = candidate.mp3;
  if (mp3.currentPCMFrame != 0 &&
      drmp3_seek_to_pcm_frame(&mp3, 0) != DRMP3_TRUE) {
    attempt.result = Mp3SeekAttemptResult::DecodeFailure;
    attempt.elapsedMs = millis() - startedAt;
    return attempt;
  }

  uint64_t remaining = frame;
  while (remaining > 0) {
    if (stopIsRequested()) {
      attempt.result = Mp3SeekAttemptResult::Stopped;
      break;
    }
    if (generation &&
        __atomic_load_n(&seekGeneration, __ATOMIC_ACQUIRE) != generation) {
      attempt.result = Mp3SeekAttemptResult::Superseded;
      break;
    }
    if ((uint32_t)(millis() - startedAt) >= budgetMs) {
      attempt.result = Mp3SeekAttemptResult::Timeout;
      break;
    }

    const uint64_t chunk =
        std::min<uint64_t>(remaining, kMp3SeekYieldFrames);
    const uint64_t read =
        drmp3_read_pcm_frames_s16(&mp3, chunk, nullptr);
    attempt.framesScanned += read;
    if (read != chunk) {
      attempt.result = Mp3SeekAttemptResult::DecodeFailure;
      break;
    }
    remaining -= read;

    // A yield alone can immediately reschedule this priority-3 task. Block for
    // one RTOS tick so CPU0's idle task and control work remain responsive.
    vTaskDelay(1);
  }

  if (remaining == 0) {
    attempt.result = Mp3SeekAttemptResult::Completed;
  }
  attempt.elapsedMs = millis() - startedAt;
  return attempt;
}

bool MediaPlayerPoC::prefillMp3Candidate(
    DecoderState &candidate, int32_t *prefill, size_t capacityFrames,
    size_t &framesWritten, bool &reachedEnd, uint32_t generation,
    Mp3SeekAttemptResult &failure)
{
  framesWritten = 0;
  reachedEnd = false;
  failure = Mp3SeekAttemptResult::PrefillFailure;
  if (!prefill || capacityFrames == 0) return false;

  const uint32_t startedAt = millis();
  while (framesWritten < capacityFrames) {
    if (stopIsRequested()) {
      failure = Mp3SeekAttemptResult::Stopped;
      return false;
    }
    if (generation &&
        __atomic_load_n(&seekGeneration, __ATOMIC_ACQUIRE) != generation) {
      failure = Mp3SeekAttemptResult::Superseded;
      return false;
    }
    if ((uint32_t)(millis() - startedAt) >= kSeekPrefillTimeoutMs) {
      failure = Mp3SeekAttemptResult::PrefillFailure;
      return false;
    }

    const size_t request =
        std::min(kDecodeChunkFrames, capacityFrames - framesWritten);
    bool ioFault = false;
    const size_t frames =
        decodeFramesFrom(candidate, currentFile, candidate.stereo, request,
                         ioFault);
    if (frames == 0) {
      if (ioFault) {
        failure = Mp3SeekAttemptResult::DecodeFailure;
        return false;
      }
      reachedEnd = true;
      break;
    }
    memcpy(prefill + framesWritten * 2, candidate.stereo,
           frames * 2 * sizeof(prefill[0]));
    framesWritten += frames;
    updateTaskStackWatermark(true);
    vTaskDelay(1);
  }

  if (framesWritten == 0) return false;
  failure = Mp3SeekAttemptResult::Completed;
  return true;
}

const char *MediaPlayerPoC::mp3SeekAttemptName(Mp3SeekAttemptResult result)
{
  switch (result) {
    case Mp3SeekAttemptResult::Completed: return "completed";
    case Mp3SeekAttemptResult::Timeout: return "timeout";
    case Mp3SeekAttemptResult::Stopped: return "stop";
    case Mp3SeekAttemptResult::Superseded: return "superseded";
    case Mp3SeekAttemptResult::DecodeFailure: return "decode-failure";
    case Mp3SeekAttemptResult::HeapIntegrityFailure:
      return "heap-integrity-failure";
    case Mp3SeekAttemptResult::OpenFailure: return "open-failure";
    case Mp3SeekAttemptResult::AllocationFailure: return "allocation-failure";
    case Mp3SeekAttemptResult::PrefillFailure: return "prefill-failure";
    default: return "unknown";
  }
}

void MediaPlayerPoC::resetRing()
{
  __atomic_store_n(&ringReadCount, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&ringWriteCount, 0, __ATOMIC_RELEASE);
}

void MediaPlayerPoC::updateTaskStackWatermark(bool decoderTaskContext)
{
  uint32_t freeStack = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  volatile uint32_t *destination = decoderTaskContext
      ? &stats.decoderStackMinFree : &stats.outputStackMinFree;
  uint32_t previous = __atomic_load_n(destination, __ATOMIC_RELAXED);
  if (previous == 0 || freeStack < previous) {
    __atomic_store_n(destination, freeStack, __ATOMIC_RELAXED);
  }
}

void MediaPlayerPoC::publishSeekEvent(MediaSeekResult result,
                                      uint64_t requestedFrame,
                                      uint64_t actualFrame,
                                      uint32_t generation,
                                      uint32_t elapsedMs)
{
  if (!seekEventQueue) return;
  MediaSeekEvent event;
  event.result = result;
  event.requestedFrame = requestedFrame;
  event.actualFrame = actualFrame;
  event.generation = generation;
  event.elapsedMs = elapsedMs;
  xQueueOverwrite(seekEventQueue, &event);
}

void MediaPlayerPoC::reportStorageReadFault(const char *stage)
{
  // Use cached metadata so fault reporting does not wait on the shared bus.
  uint64_t position = 0;
  uint64_t size = 0;
  if (decoder && decoder->file) {
    position = decoder->file.position();
    size = decoder->file.size();
  }
  const uint8_t errorCode = mediaFs.errorCode();
  const uint32_t errorData = mediaFs.errorData();
  Serial.printf(
      "MEDIA SD FAULT: stage=%s path=%s position=%llu size=%llu "
      "clock=%lu Hz code=0x%02X data=0x%08lX\n",
      stage ? stage : "unknown",
      currentPath[0] ? currentPath : "(none)",
      static_cast<unsigned long long>(position),
      static_cast<unsigned long long>(size),
      static_cast<unsigned long>(mountedFrequencyHz),
      static_cast<unsigned>(errorCode),
      static_cast<unsigned long>(errorData));
}

bool MediaPlayerPoC::prefillAfterSeek()
{
  if (!decoder) return false;
  unsigned long deadline = millis() + kSeekPrefillTimeoutMs;
  const size_t target = std::min(kSeekPrefillFrames,
                                 ringFrameCapacity / 4);
  while (!stopIsRequested() && ringAvailable() < target &&
         ringWritable() > 0 && (long)(millis() - deadline) < 0) {
    size_t request = std::min(kDecodeChunkFrames, ringWritable());
    size_t frames = decodeFrames(decoder->stereo, request);
    updateTaskStackWatermark(true);
    if (frames == 0) {
      if (storageIoFault) {
        reportStorageReadFault("seek-prefill");
        cardMounted = false;
        setError("SD read failed or card removed");
        setStopRequested(true);
        return false;
      }
      decoderComplete = true;
      break;
    }
    __atomic_add_fetch(&stats.decodedFrames, frames, __ATOMIC_RELAXED);
    if (writeRing(decoder->stereo, frames) != frames) return false;
  }
  return ringAvailable() > 0;
}

bool MediaPlayerPoC::performSeek(SeekCommand command)
{
  if (!decoder || !seekCommandQueue || !mediaControlEvents) {
    publishSeekEvent(MediaSeekResult::Failed, command.targetFrame,
                     __atomic_load_n(&stats.outputFrames, __ATOMIC_ACQUIRE),
                     command.generation, 0);
    return false;
  }

  const uint32_t startedAt = millis();
  const uint64_t originalFrame =
      __atomic_load_n(&stats.outputFrames, __ATOMIC_ACQUIRE);
  SeekCommand latest = command;
  const uint32_t stackBefore =
      (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  updateTaskStackWatermark(true);
  if (stackBefore < kSeekMinimumFreeStackBytes) {
    if (!stopIsRequested()) {
      state = seekResumePaused ? MediaPlaybackState::Paused
                               : MediaPlaybackState::Playing;
    }
    __atomic_add_fetch(&stats.seekFailed, 1U, __ATOMIC_RELAXED);
    Serial.printf(
        "MEDIA SEEK: refused low decoder stack free=%lu minimum=%lu\n",
        (unsigned long)stackBefore,
        (unsigned long)kSeekMinimumFreeStackBytes);
    publishSeekEvent(MediaSeekResult::Failed, latest.targetFrame,
                     originalFrame, latest.generation, 0);
    return false;
  }

  xEventGroupClearBits(mediaControlEvents, kOutputQuiescentBit);
  xEventGroupSetBits(mediaControlEvents, kSeekActiveBit);
  EventBits_t bits = xEventGroupWaitBits(
      mediaControlEvents, kOutputQuiescentBit, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(kSeekQuiesceTimeoutMs));
  if ((bits & kOutputQuiescentBit) == 0) {
    xEventGroupClearBits(mediaControlEvents,
                         kSeekActiveBit | kOutputQuiescentBit);
    if (!stopIsRequested()) {
      state = seekResumePaused ? MediaPlaybackState::Paused
                               : MediaPlaybackState::Playing;
    }
    uint32_t elapsed = millis() - startedAt;
    __atomic_add_fetch(&stats.seekFailed, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&stats.lastSeekElapsedMs, elapsed, __ATOMIC_RELAXED);
    Serial.printf(
        "MEDIA SEEK: output quiesce timeout target=%llu elapsed=%lu ms\n",
        (unsigned long long)latest.targetFrame, (unsigned long)elapsed);
    publishSeekEvent(MediaSeekResult::Failed, latest.targetFrame,
                     originalFrame, latest.generation, elapsed);
    return false;
  }

  Serial.printf(
      "MEDIA SEEK BEGIN generation=%lu target=%llu original=%llu "
      "stack_free=%lu\n",
      (unsigned long)latest.generation,
      (unsigned long long)latest.targetFrame,
      (unsigned long long)originalFrame,
      (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
  Serial.println("MEDIA SEEK: output quiescent");

  bool completed = false;
  bool restored = false;
  uint64_t actualFrame = originalFrame;

  if (decoder->mp3Open) {
    // MP3 seeks are transactional. The active decoder, buffered audio and
    // counters remain untouched until a separate decoder has reached the
    // target and produced a complete staging prefill.
    while (!stopIsRequested()) {
      if (currentFile.totalFrames > 0) {
        latest.targetFrame = std::min<uint64_t>(
            latest.targetFrame, currentFile.totalFrames - 1);
      }

      const DspiMp3SeekPolicy::Plan plan =
          DspiMp3SeekPolicy::candidatePlan(originalFrame,
                                           latest.targetFrame);
      const uint64_t requestedDistance =
          latest.targetFrame >= originalFrame
              ? latest.targetFrame - originalFrame
              : originalFrame - latest.targetFrame;
      Serial.printf(
          "MEDIA SEEK MP3 PLAN strategy=candidate-restart generation=%lu "
          "source=%llu target=%llu distance=%llu scan=%llu budget=%lu "
          "cap=%lu original_intact=yes\n",
          (unsigned long)latest.generation,
          (unsigned long long)originalFrame,
          (unsigned long long)latest.targetFrame,
          (unsigned long long)requestedDistance,
          (unsigned long long)plan.scanFrames,
          (unsigned long)plan.budgetMs,
          (unsigned long)DspiMp3SeekPolicy::kAbsoluteBudgetMs);

      DecoderState *candidate = nullptr;
      int32_t *candidatePrefill = static_cast<int32_t *>(heap_caps_malloc(
          kSeekPrefillFrames * 2 * sizeof(int32_t),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      Mp3SeekAttempt attempt;
      MediaFileInfo candidateInfo;
      size_t candidatePrefillFrames = 0;
      bool candidateReachedEnd = false;
      bool candidateReady = false;
      const uint32_t candidateStartedAt = millis();

      if (!candidatePrefill) {
        attempt.result = Mp3SeekAttemptResult::AllocationFailure;
      } else if (!heap_caps_check_integrity_all(true)) {
        attempt.result = Mp3SeekAttemptResult::HeapIntegrityFailure;
      } else if (!openDecoderState(currentPath, candidate, candidateInfo) ||
                 candidateInfo.format != MediaFileFormat::Mp3 ||
                 candidateInfo.sampleRate != currentFile.sampleRate ||
                 candidateInfo.channels != currentFile.channels) {
        attempt.result = Mp3SeekAttemptResult::OpenFailure;
      } else {
        attempt = seekMp3CandidateToFrame(
            *candidate, latest.targetFrame, latest.generation,
            plan.budgetMs);
        if (attempt.result == Mp3SeekAttemptResult::Completed) {
          Mp3SeekAttemptResult prefillFailure =
              Mp3SeekAttemptResult::PrefillFailure;
          candidateReady = prefillMp3Candidate(
              *candidate, candidatePrefill, kSeekPrefillFrames,
              candidatePrefillFrames, candidateReachedEnd,
              latest.generation, prefillFailure);
          if (!candidateReady) attempt.result = prefillFailure;
        }
        if (!heap_caps_check_integrity_all(true)) {
          candidateReady = false;
          attempt.result = Mp3SeekAttemptResult::HeapIntegrityFailure;
        }
      }
      attempt.elapsedMs = millis() - candidateStartedAt;

      SeekCommand replacement;
      const bool haveReplacement =
          xQueueReceive(seekCommandQueue, &replacement, 0) == pdTRUE;
      if (haveReplacement ||
          __atomic_load_n(&seekGeneration, __ATOMIC_ACQUIRE) !=
              latest.generation) {
        candidateReady = false;
        if (!stopIsRequested()) {
          attempt.result = Mp3SeekAttemptResult::Superseded;
        }
      }

      Serial.printf(
          "MEDIA SEEK MP3 ATTEMPT generation=%lu result=%s "
          "frames_scanned=%llu prefill=%u elapsed=%lu ms "
          "original_intact=yes\n",
          (unsigned long)latest.generation,
          mp3SeekAttemptName(attempt.result),
          (unsigned long long)attempt.framesScanned,
          (unsigned)candidatePrefillFrames,
          (unsigned long)attempt.elapsedMs);

      if (candidateReady) {
        // The empty-ring write is deterministic because the staged frame count
        // is bounded by both kSeekPrefillFrames and ring capacity.
        const size_t commitFrames =
            std::min(candidatePrefillFrames, ringFrameCapacity);
        if (commitFrames > 0) {
          resetRing();
          const size_t committed =
              writeRing(candidatePrefill, commitFrames);
          DecoderState *previous = decoder;
          decoder = candidate;
          candidate = nullptr;
          decoderComplete = candidateReachedEnd;
          __atomic_store_n(&stats.outputFrames, latest.targetFrame,
                           __ATOMIC_RELEASE);
          __atomic_store_n(&stats.decodedFrames,
                           latest.targetFrame + committed,
                           __ATOMIC_RELEASE);
          closeDecoderState(previous);
          completed = true;
          actualFrame = latest.targetFrame;
          Serial.printf(
              "MEDIA SEEK MP3 COMMIT generation=%lu frame=%llu "
              "prefill=%u original_intact=until-commit\n",
              (unsigned long)latest.generation,
              (unsigned long long)actualFrame, (unsigned)committed);
        }
      }

      closeDecoderState(candidate);
      if (candidatePrefill) heap_caps_free(candidatePrefill);

      if (completed) break;
      if (stopIsRequested()) break;
      if (haveReplacement) {
        Serial.printf(
            "MEDIA SEEK: superseded generation=%lu by generation=%lu "
            "target=%llu original_intact=yes\n",
            (unsigned long)latest.generation,
            (unsigned long)replacement.generation,
            (unsigned long long)replacement.targetFrame);
        latest = replacement;
        continue;
      }

      restored = true;
      actualFrame = originalFrame;
      Serial.printf(
          "MEDIA SEEK MP3 ROLLBACK generation=%lu reason=%s frame=%llu "
          "original_intact=yes terminal_error=no\n",
          (unsigned long)latest.generation,
          mp3SeekAttemptName(attempt.result),
          (unsigned long long)originalFrame);
      break;
    }
  } else {
    // Preserve the proven v12.1 WAV/FLAC in-place path unchanged.
    while (!stopIsRequested()) {
      if (currentFile.totalFrames > 0) {
        latest.targetFrame = std::min<uint64_t>(
            latest.targetFrame, currentFile.totalFrames - 1);
      }

      resetRing();
      decoderComplete = false;
      bool heapBefore = heap_caps_check_integrity_all(true);
      updateTaskStackWatermark(true);
      bool seekOk =
          heapBefore &&
          seekDecoderToFrame(latest.targetFrame, latest.generation);
      if (stopIsRequested()) break;
      updateTaskStackWatermark(true);
      bool heapAfter = heap_caps_check_integrity_all(true);
      seekOk = seekOk && heapAfter;

      SeekCommand replacement;
      if (xQueueReceive(seekCommandQueue, &replacement, 0) == pdTRUE) {
        Serial.printf(
            "MEDIA SEEK: superseded generation=%lu by generation=%lu "
            "target=%llu\n",
            (unsigned long)latest.generation,
            (unsigned long)replacement.generation,
            (unsigned long long)replacement.targetFrame);
        latest = replacement;
        continue;
      }

      if (seekOk) {
        __atomic_store_n(&stats.decodedFrames, latest.targetFrame,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&stats.outputFrames, latest.targetFrame,
                         __ATOMIC_RELEASE);
        seekOk = prefillAfterSeek();
      }

      if (xQueueReceive(seekCommandQueue, &replacement, 0) == pdTRUE) {
        latest = replacement;
        continue;
      }

      if (seekOk) {
        completed = true;
        actualFrame = latest.targetFrame;
        break;
      }

      Serial.printf(
          "MEDIA SEEK: target failed generation=%lu target=%llu; "
          "restoring=%llu\n",
          (unsigned long)latest.generation,
          (unsigned long long)latest.targetFrame,
          (unsigned long long)originalFrame);
      resetRing();
      decoderComplete = false;
      restored = seekDecoderToFrame(originalFrame, latest.generation);
      if (restored) {
        __atomic_store_n(&stats.decodedFrames, originalFrame,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&stats.outputFrames, originalFrame,
                         __ATOMIC_RELEASE);
        restored = prefillAfterSeek();
      }
      actualFrame = originalFrame;
      break;
    }
  }

  const uint32_t elapsed = millis() - startedAt;
  __atomic_store_n(&stats.lastSeekElapsedMs, elapsed, __ATOMIC_RELAXED);
  xEventGroupClearBits(mediaControlEvents,
                       kSeekActiveBit | kOutputQuiescentBit);

  if (stopIsRequested()) {
    publishSeekEvent(MediaSeekResult::Cancelled, latest.targetFrame,
                     actualFrame, latest.generation, elapsed);
    return false;
  }

  if (completed) {
    __atomic_add_fetch(&stats.seekCompleted, 1U, __ATOMIC_RELAXED);
    state = seekResumePaused ? MediaPlaybackState::Paused
                             : MediaPlaybackState::Playing;
    Serial.printf(
        "MEDIA SEEK COMPLETE generation=%lu frame=%llu elapsed=%lu ms "
        "prefill=%u stack_free=%lu\n",
        (unsigned long)latest.generation,
        (unsigned long long)actualFrame, (unsigned long)elapsed,
        (unsigned)ringAvailable(),
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr));
    publishSeekEvent(MediaSeekResult::Completed, latest.targetFrame,
                     actualFrame, latest.generation, elapsed);
    return true;
  }

  __atomic_add_fetch(&stats.seekFailed, 1U, __ATOMIC_RELAXED);
  if (restored) {
    state = seekResumePaused ? MediaPlaybackState::Paused
                             : MediaPlaybackState::Playing;
    strlcpy(errorText, "seek unavailable", sizeof(errorText));
    Serial.printf(
        "MEDIA SEEK FAILED generation=%lu restored=%llu elapsed=%lu ms\n",
        (unsigned long)latest.generation,
        (unsigned long long)actualFrame, (unsigned long)elapsed);
    publishSeekEvent(MediaSeekResult::Failed, latest.targetFrame,
                     actualFrame, latest.generation, elapsed);
    return false;
  }

  Serial.printf(
      "MEDIA SEEK FAILED generation=%lu restore-failed elapsed=%lu ms\n",
      (unsigned long)latest.generation, (unsigned long)elapsed);
  publishSeekEvent(MediaSeekResult::Failed, latest.targetFrame,
                   actualFrame, latest.generation, elapsed);
  setError("decoder seek and recovery failed");
  setStopRequested(true);
  return false;
}

size_t MediaPlayerPoC::decodeFrames(int32_t *stereoOutput,
                                    size_t maximumFrames)
{
  if (!decoder || !stereoOutput || maximumFrames == 0) return 0;
  bool ioFault = false;
  size_t framesRead =
      decodeFramesFrom(*decoder, currentFile, stereoOutput, maximumFrames,
                       ioFault);
  if (ioFault) storageIoFault = true;
  return framesRead;
}

size_t MediaPlayerPoC::decodeFramesFrom(
    DecoderState &source, const MediaFileInfo &info, int32_t *stereoOutput,
    size_t maximumFrames, bool &ioFault)
{
  ioFault = false;
  if (!stereoOutput || maximumFrames == 0) return 0;
  maximumFrames = std::min(maximumFrames, kDecodeChunkFrames);

  const uint32_t decodeStartedAt = millis();
  size_t framesRead = 0;
  if (source.wavOpen) {
    framesRead = (size_t)drwav_read_pcm_frames_s32(
        &source.wav, maximumFrames, source.s32);
  } else if (source.flac) {
    framesRead = (size_t)drflac_read_pcm_frames_s32(
        source.flac, maximumFrames, source.s32);
  } else if (source.mp3Open) {
    framesRead = (size_t)drmp3_read_pcm_frames_s16(
        &source.mp3, maximumFrames, source.s16);
  }
  const uint32_t decodeElapsedMs = millis() - decodeStartedAt;
  updateAtomicMaximum(&stats.decoderCallMaxMs, decodeElapsedMs);
  __atomic_store_n(&stats.decoderLastCallMs, decodeElapsedMs,
                   __ATOMIC_RELEASE);
  if (decodeElapsedMs >= kSlowDecoderCallMs) {
    __atomic_add_fetch(&stats.decoderSlowCalls, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&stats.decoderLastSlowAtMs, millis(), __ATOMIC_RELEASE);
  }
  if (decodeElapsedMs >= kDecoderIoYieldIntervalMs) {
    vTaskDelay(1);
  }

  if (framesRead == 0 && source.file.hadIoError()) {
    ioFault = true;
  }

  if (info.format == MediaFileFormat::Mp3) {
    if (info.channels == 1) {
      for (size_t frame = 0; frame < framesRead; frame++) {
        int32_t sample = (int32_t)source.s16[frame] * 65536;
        stereoOutput[frame * 2] = sample;
        stereoOutput[frame * 2 + 1] = sample;
      }
    } else {
      for (size_t sample = 0; sample < framesRead * 2; sample++) {
        stereoOutput[sample] = (int32_t)source.s16[sample] * 65536;
      }
    }
  } else if (info.channels == 1) {
    for (size_t frame = 0; frame < framesRead; frame++) {
      int32_t sample = source.s32[frame];
      stereoOutput[frame * 2] = sample;
      stereoOutput[frame * 2 + 1] = sample;
    }
  } else if (framesRead) {
    memcpy(stereoOutput, source.s32,
           framesRead * 2 * sizeof(stereoOutput[0]));
  }
  return framesRead;
}

void MediaPlayerPoC::closeDecoderState(DecoderState *&target)
{
  if (!target) return;
  if (target->wavOpen) {
    drwav_uninit(&target->wav);
    target->wavOpen = false;
  }
  if (target->flac) {
    drflac_close(target->flac);
    target->flac = nullptr;
  }
  if (target->mp3Open) {
    drmp3_uninit(&target->mp3);
    target->mp3Open = false;
  }
  if (target->file) {
    SharedSpiGuard guard;
    target->file.close();
  }
  delete target;
  target = nullptr;
}

void MediaPlayerPoC::closeDecoder()
{
  closeDecoderState(decoder);
}

bool MediaPlayerPoC::allocateRing()
{
  // Keep the PSRAM ring for the lifetime of the media subsystem. Reusing the
  // same allocation avoids fragmentation and removes a large allocation from
  // every automatic track transition.
  if (pcmRing && ringFrameCapacity) {
    resetRing();
    return true;
  }
  freeRing();

  auto allocateFrames = [&](size_t frames) -> int32_t * {
    const size_t bytes = frames * 2 * sizeof(int32_t);
    return static_cast<int32_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  };

  pcmRing = allocateFrames(kPreferredRingFrames);
  ringFrameCapacity = pcmRing ? kPreferredRingFrames : 0;
  if (pcmRing) {
    const size_t words = ringFrameCapacity * 2;
    const size_t bytes = words * sizeof(int32_t);
    const uint32_t started = micros();
    for (size_t index = 0; index < words; index++) {
      pcmRing[index] = (int32_t)(0x5A5A0000U ^ (uint32_t)index);
    }
    volatile uint32_t checksum = 0;
    for (size_t index = 0; index < words; index++) {
      checksum ^= (uint32_t)pcmRing[index];
    }
    const uint32_t elapsedUs = std::max<uint32_t>(1, micros() - started);
    const uint64_t transferredBytes = (uint64_t)bytes * 2ULL;
    const uint32_t tenthsMiB = (uint32_t)(
        (transferredBytes * 10ULL * 1000000ULL) /
        ((uint64_t)elapsedUs * 1024ULL * 1024ULL));
    Serial.printf(
        "MEDIA RING: PSRAM test frames=%u bytes=%u bandwidth=%lu.%lu MiB/s checksum=%08lX\n",
        (unsigned)ringFrameCapacity, (unsigned)bytes,
        (unsigned long)(tenthsMiB / 10), (unsigned long)(tenthsMiB % 10),
        (unsigned long)checksum);
    if (tenthsMiB < kMinimumPsramBandwidthTenthsMiB) {
      Serial.printf(
          "MEDIA RING: preferred buffer below bandwidth floor %lu.%lu MiB/s; falling back\n",
          (unsigned long)(kMinimumPsramBandwidthTenthsMiB / 10),
          (unsigned long)(kMinimumPsramBandwidthTenthsMiB % 10));
      heap_caps_free(pcmRing);
      pcmRing = nullptr;
      ringFrameCapacity = 0;
    }
  }

  if (!pcmRing) {
    pcmRing = allocateFrames(kFallbackRingFrames);
    ringFrameCapacity = pcmRing ? kFallbackRingFrames : 0;
  }
  if (!pcmRing) {
    pcmRing = allocateFrames(kEmergencyRingFrames);
    ringFrameCapacity = pcmRing ? kEmergencyRingFrames : 0;
  }
  if (!pcmRing || ringFrameCapacity == 0) return false;

  memset(pcmRing, 0, ringFrameCapacity * 2 * sizeof(int32_t));
  resetRing();
  Serial.printf("MEDIA RING: selected=%u frames (%u bytes)\n",
                (unsigned)ringFrameCapacity,
                (unsigned)(ringFrameCapacity * 2 * sizeof(int32_t)));
  return true;
}

void MediaPlayerPoC::freeRing()
{
  if (pcmRing) {
    heap_caps_free(pcmRing);
    pcmRing = nullptr;
  }
  ringFrameCapacity = 0;
  resetRing();
}

size_t MediaPlayerPoC::ringAvailable() const
{
  uint32_t write = __atomic_load_n(&ringWriteCount, __ATOMIC_ACQUIRE);
  uint32_t read = __atomic_load_n(&ringReadCount, __ATOMIC_ACQUIRE);
  return (size_t)(write - read);
}

size_t MediaPlayerPoC::ringWritable() const
{
  if (ringFrameCapacity == 0) return 0;
  return ringFrameCapacity - ringAvailable();
}

size_t MediaPlayerPoC::writeRing(const int32_t *stereoInput, size_t frames)
{
  if (!pcmRing || !stereoInput || frames == 0 || ringFrameCapacity == 0) {
    return 0;
  }
  uint32_t write = __atomic_load_n(&ringWriteCount, __ATOMIC_RELAXED);
  uint32_t read = __atomic_load_n(&ringReadCount, __ATOMIC_ACQUIRE);
  size_t writable = ringFrameCapacity - (size_t)(write - read);
  frames = std::min(frames, writable);

  size_t index = write & (ringFrameCapacity - 1);
  size_t first = std::min(frames, ringFrameCapacity - index);
  memcpy(pcmRing + index * 2, stereoInput, first * 2 * sizeof(int32_t));
  if (frames > first) {
    memcpy(pcmRing, stereoInput + first * 2,
           (frames - first) * 2 * sizeof(int32_t));
  }
  __atomic_store_n(&ringWriteCount, write + (uint32_t)frames,
                   __ATOMIC_RELEASE);

  uint32_t fill = (uint32_t)((write + frames) - read);
  uint32_t highWater =
      __atomic_load_n(&stats.ringHighWaterFrames, __ATOMIC_RELAXED);
  while (fill > highWater &&
         !__atomic_compare_exchange_n(&stats.ringHighWaterFrames, &highWater,
                                      fill, false, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
  }
  return frames;
}

size_t MediaPlayerPoC::readRing(int32_t *stereoOutput, size_t frames)
{
  if (!pcmRing || !stereoOutput || frames == 0 || ringFrameCapacity == 0) {
    return 0;
  }
  uint32_t read = __atomic_load_n(&ringReadCount, __ATOMIC_RELAXED);
  uint32_t write = __atomic_load_n(&ringWriteCount, __ATOMIC_ACQUIRE);
  frames = std::min(frames, (size_t)(write - read));

  size_t index = read & (ringFrameCapacity - 1);
  size_t first = std::min(frames, ringFrameCapacity - index);
  memcpy(stereoOutput, pcmRing + index * 2, first * 2 * sizeof(int32_t));
  if (frames > first) {
    memcpy(stereoOutput + first * 2, pcmRing,
           (frames - first) * 2 * sizeof(int32_t));
  }
  __atomic_store_n(&ringReadCount, read + (uint32_t)frames,
                   __ATOMIC_RELEASE);
  return frames;
}

bool MediaPlayerPoC::stopIsRequested() const
{
  return __atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE);
}

void MediaPlayerPoC::setStopRequested(bool requested)
{
  __atomic_store_n(&stopRequested, requested, __ATOMIC_RELEASE);
}

MediaPlayerPoC::SpdifCarrierRetention
MediaPlayerPoC::spdifCarrierRetention() const
{
  return static_cast<SpdifCarrierRetention>(
      __atomic_load_n(&spdifCarrierRetentionState, __ATOMIC_ACQUIRE));
}

void MediaPlayerPoC::setSpdifCarrierRetention(
    SpdifCarrierRetention retention)
{
  __atomic_store_n(&spdifCarrierRetentionState,
                   static_cast<uint8_t>(retention), __ATOMIC_RELEASE);
}

bool MediaPlayerPoC::publishRetainedSpdifCarrier()
{
  uint8_t expected = static_cast<uint8_t>(SpdifCarrierRetention::Requested);
  return __atomic_compare_exchange_n(
      &spdifCarrierRetentionState, &expected,
      static_cast<uint8_t>(SpdifCarrierRetention::Ready), false,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool MediaPlayerPoC::decoderTaskRunning() const
{
  return __atomic_load_n(&decoderTaskHandle, __ATOMIC_ACQUIRE) != nullptr;
}

bool MediaPlayerPoC::outputTaskRunning() const
{
  return __atomic_load_n(&outputTaskHandle, __ATOMIC_ACQUIRE) != nullptr;
}

bool MediaPlayerPoC::startSpdif(int8_t dataOutPin)
{
  if (spdifTxChannel &&
      spdifCarrierRetention() == SpdifCarrierRetention::Ready &&
      spdifDataOutPin == dataOutPin &&
      spdifSampleRate == currentFile.sampleRate) {
    setSpdifCarrierRetention(SpdifCarrierRetention::Off);
    Serial.printf(
        "MEDIA SPDIF TX: reused continuous carrier rate=%lu Hz data=GPIO%d\n",
        (unsigned long)spdifSampleRate, dataOutPin);
    return true;
  }
  const bool replacingCarrier = spdifTxChannel != nullptr;
  stopSpdif();
  pinMode(dataOutPin, OUTPUT);
  digitalWrite(dataOutPin, LOW);
  spdifDataOutPin = dataOutPin;
  i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  channelConfig.dma_desc_num = kSpdifDmaDescriptorCount;
  channelConfig.dma_frame_num = kSpdifDmaFramesPerDescriptor;
  // Preserve the elevated request for non-GDMA targets. ESP32-S3 uses GDMA in
  // IDF 5.5.5, where the public driver selects a LOWMED interrupt and does not
  // expose an exact priority override; its callback remains IRAM-safe.
  channelConfig.intr_priority = kSpdifInterruptPriority;
  // Raw zero DMA words are not S/PDIF silence: they remove the biphase
  // carrier and force the downstream receiver to relock. Retain previously
  // encoded descriptor contents if the producer is ever briefly late.
  channelConfig.auto_clear_after_cb = false;

  i2s_chan_handle_t tx = nullptr;
  if (i2s_new_channel(&channelConfig, &tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t standardConfig = {};
  i2s_std_clk_config_t clockConfig =
      I2S_STD_CLK_DEFAULT_CONFIG(currentFile.sampleRate * 2u);
#if SOC_I2S_SUPPORTS_APLL
  clockConfig.clk_src = I2S_CLK_SRC_APLL;
#endif
  i2s_std_slot_config_t slotConfig =
      I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                     I2S_SLOT_MODE_STEREO);
  standardConfig.clk_cfg = clockConfig;
  standardConfig.slot_cfg = slotConfig;
  standardConfig.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  standardConfig.gpio_cfg.bclk = I2S_GPIO_UNUSED;
  standardConfig.gpio_cfg.ws = I2S_GPIO_UNUSED;
  standardConfig.gpio_cfg.dout = (gpio_num_t)dataOutPin;
  standardConfig.gpio_cfg.din = I2S_GPIO_UNUSED;
  standardConfig.gpio_cfg.invert_flags.mclk_inv = false;
  standardConfig.gpio_cfg.invert_flags.bclk_inv = false;
  standardConfig.gpio_cfg.invert_flags.ws_inv = false;

  esp_err_t result = i2s_channel_init_std_mode(tx, &standardConfig);

  // While the channel is still READY, preload every DMA descriptor with
  // complete, valid S/PDIF silence. One 192-frame block maps exactly to one
  // descriptor, and all 16 blocks total 49,152 bytes. Enabling only after the
  // ring is full prevents an initial raw-zero/partial carrier transmission.
  uint32_t encodedSilence[192 * SpdifBlockEncoder::kWordsPerStereoFrame] = {};
  SpdifBlockEncoder silenceEncoder;
  if (!silenceEncoder.setSampleRate(currentFile.sampleRate)) {
    result = ESP_ERR_INVALID_ARG;
  }
  silenceEncoder.reset();
  size_t preloadedBytes = 0;
  for (uint32_t block = 0;
       block < kSpdifDmaDescriptorCount && result == ESP_OK; ++block) {
    if (!silenceEncoder.encode(nullptr, 192, encodedSilence,
                               sizeof(encodedSilence) / sizeof(uint32_t))) {
      result = ESP_FAIL;
      break;
    }
    size_t loaded = 0;
    result = i2s_channel_preload_data(tx, encodedSilence,
                                     sizeof(encodedSilence), &loaded);
    if (result == ESP_OK && loaded != sizeof(encodedSilence)) {
      result = ESP_FAIL;
    } else if (result == ESP_OK) {
      preloadedBytes += loaded;
    }
  }
  constexpr size_t kExpectedPreloadBytes =
      kSpdifDmaDescriptorCount * 192u *
      SpdifBlockEncoder::kWordsPerStereoFrame * sizeof(uint32_t);
  if (result == ESP_OK && preloadedBytes != kExpectedPreloadBytes) {
    result = ESP_FAIL;
  }
  if (result == ESP_OK) result = i2s_channel_enable(tx);
  if (result != ESP_OK) {
    i2s_del_channel(tx);
    pinMode(dataOutPin, OUTPUT);
    digitalWrite(dataOutPin, LOW);
    return false;
  }
  spdifTxChannel = tx;
  spdifDataOutPin = dataOutPin;
  spdifSampleRate = currentFile.sampleRate;
  setSpdifCarrierRetention(SpdifCarrierRetention::Off);
  if (replacingCarrier) {
    __atomic_add_fetch(&spdifCarrierRestartsLifetime, 1U, __ATOMIC_RELAXED);
  }
  Serial.printf(
      "MEDIA SPDIF TX: enabled rate=%lu Hz carrier=%lu Hz data=GPIO%d "
      "depth=24-bit block-DMA\n",
      (unsigned long)currentFile.sampleRate,
      (unsigned long)(currentFile.sampleRate * 2u), dataOutPin);
  return true;
}

void MediaPlayerPoC::stopSpdif()
{
  setSpdifCarrierRetention(SpdifCarrierRetention::Off);
  if (spdifTxChannel) {
    i2s_chan_handle_t tx = static_cast<i2s_chan_handle_t>(spdifTxChannel);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    spdifTxChannel = nullptr;
  }
  // Do not leave an inactive high-speed link floating or matrix-owned. A
  // defined low level prevents idle coupling into the DSPi output wiring.
  if (spdifDataOutPin >= 0) {
    pinMode(spdifDataOutPin, OUTPUT);
    digitalWrite(spdifDataOutPin, LOW);
    spdifDataOutPin = -1;
  }
  spdifSampleRate = 0;
}

void MediaPlayerPoC::setError(const char *message)
{
  // Retention is published as Ready only after the output task has aligned and
  // replaced a complete ring. Faulted cannot be resurrected by a later track
  // request while this TX/task lifetime is still unwinding.
  setSpdifCarrierRetention(SpdifCarrierRetention::Faulted);
  strlcpy(errorText, message ? message : "unknown media error",
          sizeof(errorText));
  state = MediaPlaybackState::Error;
  terminalEventPending = true;
}

bool MediaPlayerPoC::beginPlay(const char *path, int8_t dataOutPin,
                               MediaRouteCallback routeCallback,
                               void *routeContext, Stream &out)
{
  // A cooperative track transition may leave the DMA engine transmitting
  // block-aligned encoded silence. Keep it alive while probing and prefilling
  // the next file; startSpdif() will reuse it only when pin and rate match.
  const bool retainedCarrierIdle =
      spdifCarrierRetention() == SpdifCarrierRetention::Ready &&
      spdifTxChannel && !decoderTaskRunning() && !outputTaskRunning() &&
      state == MediaPlaybackState::Stopped;
  if (!retainedCarrierIdle) stop();
  if (decoderTaskRunning() || outputTaskRunning()) {
    out.printf("MEDIA PLAY: rejected path=%s reason=previous media task still stopping\n",
               path ? path : "(none)");
    return false;
  }
  if (!seekCommandQueue || !seekEventQueue || !mediaControlEvents) {
    setError("media control queues unavailable");
    // A retained-idle carrier has no task owner here. Do not leave its
    // physical transmitter running after this early setup failure.
    stopSpdif();
    out.printf("MEDIA PLAY: rejected path=%s reason=%s\n",
               path ? path : "(none)", errorText);
    return false;
  }

  xQueueReset(seekCommandQueue);
  xQueueReset(seekEventQueue);
  // Never clear kExternalHoldBit here. A preset/limit transaction may have
  // deliberately retained the hold after an ambiguous DSPi failure; starting a
  // new file must not silently release that safety barrier.
  xEventGroupClearBits(mediaControlEvents,
                       kSeekActiveBit | kOutputQuiescentBit);
  state = MediaPlaybackState::Starting;
  startStage = StartStage::Idle;
  setStopRequested(false);
  decoderComplete = false;
  storageIoFault = false;
  terminalEventPending = false;
  seekResumePaused = false;
  seekGeneration = 0;
  stats = MediaPlaybackStats{};
  currentFile = MediaFileInfo{};
  currentPath[0] = '\0';
  errorText[0] = '\0';

  MediaFileInfo headerInfo;
  if (!cardMounted) {
    setError("SD card is not mounted");
  } else if (!path || !path[0] || !routeCallback) {
    setError("invalid playback request");
  } else if (!probeFile(path, headerInfo)) {
    setError("header probe failed");
  } else if (!headerInfo.nativeRateSupported) {
    char message[96];
    snprintf(message, sizeof(message),
             "unsupported sample rate %lu Hz; maximum supported is 48000",
             (unsigned long)headerInfo.sampleRate);
    setError(message);
  } else if ((headerInfo.channels != 1 && headerInfo.channels != 2) ||
             !sourceDepthSupported(headerInfo.format,
                                   headerInfo.bitsPerSample)) {
    setError("requires mono/stereo 16/24-bit WAV/FLAC or MP3");
  } else if (!openDecoder(path, currentFile)) {
    setError("decoder rejected the file");
  } else if (!allocateRing()) {
    closeDecoder();
    setError("PSRAM ring allocation failed");
  }

  if (state == MediaPlaybackState::Error) {
    startStage = StartStage::Idle;
    // A failed candidate must not strand a previously retained transmitter.
    // No playback task exists at this point, so immediate teardown is safe.
    stopSpdif();
    out.printf("MEDIA PLAY: rejected path=%s reason=%s\n",
               path ? path : "(null)", errorText);
    return false;
  }

  strlcpy(currentPath, path, sizeof(currentPath));
  startDataOutPin = dataOutPin;
  startRouteCallback = routeCallback;
  startRouteContext = routeContext;
  startPrefillTarget = std::min(kStartPrefillFrames,
                                ringFrameCapacity / 2);
  startStage = StartStage::Prefill;
  Serial.printf(
      "MEDIA START: prepared path=%s prefill-target=%u/%u chunks=cooperative\n",
      currentPath, (unsigned)startPrefillTarget,
      (unsigned)ringFrameCapacity);
  return true;
}

MediaStartStatus MediaPlayerPoC::servicePlayStart(Stream &out,
                                                  size_t decodeChunkBudget)
{
  if (state == MediaPlaybackState::Playing ||
      state == MediaPlaybackState::Paused) {
    return MediaStartStatus::Started;
  }
  if (state == MediaPlaybackState::Error) return MediaStartStatus::Failed;
  if (state != MediaPlaybackState::Starting ||
      startStage == StartStage::Idle) {
    return MediaStartStatus::Idle;
  }

  if (startStage == StartStage::Prefill) {
    size_t budget = std::max<size_t>(1, decodeChunkBudget);
    while (!stopIsRequested() && budget-- &&
           ringAvailable() < startPrefillTarget && ringWritable() > 0) {
      size_t request = std::min(kDecodeChunkFrames, ringWritable());
      size_t frames = decodeFrames(decoder->stereo, request);
      if (frames == 0) {
        if (storageIoFault) {
          reportStorageReadFault("start-prefill");
          cardMounted = false;
          closeDecoder();
          resetRing();
          startStage = StartStage::Idle;
          setError("SD read failed or card removed");
          out.printf("MEDIA PLAY: failed path=%s reason=%s\n",
                     currentPath, errorText);
          return MediaStartStatus::Failed;
        }
        decoderComplete = true;
        break;
      }
      __atomic_add_fetch(&stats.decodedFrames, frames, __ATOMIC_RELAXED);
      if (writeRing(decoder->stereo, frames) != frames) {
        closeDecoder();
        resetRing();
        startStage = StartStage::Idle;
        setError("PCM prefill write failed");
        out.printf("MEDIA PLAY: failed path=%s reason=%s\n",
                   currentPath, errorText);
        return MediaStartStatus::Failed;
      }
    }

    if (ringAvailable() == 0 && decoderComplete) {
      closeDecoder();
      resetRing();
      startStage = StartStage::Idle;
      setError("decoder produced no PCM frames");
      out.printf("MEDIA PLAY: failed path=%s reason=%s\n",
                 currentPath, errorText);
      return MediaStartStatus::Failed;
    }

    if (ringAvailable() < startPrefillTarget && !decoderComplete) {
      return MediaStartStatus::Preparing;
    }

    out.printf("MEDIA PREFILL: %u/%u frames before route activation\n",
               (unsigned)ringAvailable(), (unsigned)ringFrameCapacity);
    startStage = StartStage::Activate;
    return MediaStartStatus::Preparing;
  }

  if (startStage != StartStage::Activate) {
    return MediaStartStatus::Preparing;
  }

  if (!startSpdif(startDataOutPin)) {
    closeDecoder();
    resetRing();
    startStage = StartStage::Idle;
    setError("ESP32 S/PDIF TX setup failed");
  } else if (!startRouteCallback ||
             !startRouteCallback(currentFile.sampleRate,
                                 startRouteContext)) {
    stopSpdif();
    closeDecoder();
    resetRing();
    startStage = StartStage::Idle;
    setError("DSPi S/PDIF route verification failed");
  }

  if (state == MediaPlaybackState::Error) {
    out.printf("MEDIA PLAY: failed path=%s reason=%s\n", currentPath,
               errorText);
    return MediaStartStatus::Failed;
  }

  state = MediaPlaybackState::Playing;
  startStage = StartStage::Idle;
  stats.ringLowWaterFrames = (uint32_t)ringAvailable();
  BaseType_t created = xTaskCreatePinnedToCore(
      decoderTaskEntry, "media-decode", kDecoderTaskStackBytes, this,
      kDecoderTaskPriority, &decoderTaskHandle, kDecoderTaskCore);
  if (created != pdPASS) {
    stop();
    setError("decoder task creation failed");
    out.printf("MEDIA PLAY: failed path=%s reason=%s\n", currentPath,
               errorText);
    return MediaStartStatus::Failed;
  }

  created = xTaskCreatePinnedToCore(
      outputTaskEntry, "media-spdif", 8192, this, kOutputTaskPriority,
      &outputTaskHandle, kOutputTaskCore);
  if (created != pdPASS) {
    stop();
    setError("S/PDIF output task creation failed");
    out.printf("MEDIA PLAY: failed path=%s reason=%s\n", currentPath,
               errorText);
    return MediaStartStatus::Failed;
  }

  startRouteCallback = nullptr;
  startRouteContext = nullptr;
  out.printf(
      "MEDIA PLAY: %s %lu Hz %u-bit %uch path=%s "
      "ring=%u frames in PSRAM decoder-stack=%lu bytes\n",
      formatName(currentFile.format), (unsigned long)currentFile.sampleRate,
      currentFile.bitsPerSample, currentFile.channels, currentPath,
      (unsigned)ringFrameCapacity, (unsigned long)kDecoderTaskStackBytes);
  return MediaStartStatus::Started;
}

bool MediaPlayerPoC::play(const char *path, int8_t dataOutPin,
                          MediaRouteCallback routeCallback,
                          void *routeContext, Stream &out)
{
  if (!beginPlay(path, dataOutPin,
                 routeCallback, routeContext, out)) {
    return false;
  }

  while (state == MediaPlaybackState::Starting) {
    MediaStartStatus status = servicePlayStart(out, 64);
    if (status == MediaStartStatus::Started) return true;
    if (status == MediaStartStatus::Failed ||
        status == MediaStartStatus::Idle) {
      // Direct synchronous callers do not have the UI transition state
      // machine to perform failure cleanup on their behalf.
      stop();
      return false;
    }
    delay(0);
  }
  return state == MediaPlaybackState::Playing ||
         state == MediaPlaybackState::Paused;
}

void MediaPlayerPoC::requestStop()
{
  requestStopInternal(false);
}

void MediaPlayerPoC::requestTrackTransitionStop()
{
  requestStopInternal(true);
}

void MediaPlayerPoC::requestStopInternal(bool retainSpdifCarrier)
{
  const bool alreadyStopping = stopIsRequested();
  const bool decoderRunning = decoderTaskRunning();
  const bool outputRunning = outputTaskRunning();
  const bool haveResources = decoderRunning || outputRunning || decoder ||
                             spdifTxChannel;
  if (state == MediaPlaybackState::Stopped && !haveResources) return;

  if (!retainSpdifCarrier) {
    setSpdifCarrierRetention(SpdifCarrierRetention::Off);
  } else if (spdifTxChannel && !alreadyStopping &&
             state != MediaPlaybackState::Error) {
    uint8_t expected = static_cast<uint8_t>(SpdifCarrierRetention::Off);
    __atomic_compare_exchange_n(
        &spdifCarrierRetentionState, &expected,
        static_cast<uint8_t>(SpdifCarrierRetention::Requested), false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  }
  // Publish the retention decision before tasks observe the stop request.
  setStopRequested(true);
  __atomic_add_fetch(&seekGeneration, 1U, __ATOMIC_ACQ_REL);
  if (state != MediaPlaybackState::Error) {
    state = MediaPlaybackState::Draining;
  }
  if (mediaControlEvents) {
    xEventGroupClearBits(mediaControlEvents,
                         kSeekActiveBit | kOutputQuiescentBit);
  }

  // When no task owns a resource, complete cleanup immediately. Otherwise the
  // main-loop transition state machine polls serviceStopCleanup() without
  // blocking the encoder, BLE processing or display service.
  // The decoder polls at a bounded 20 ms maximum, so no notification through
  // a handle that may concurrently self-delete is needed.
  if (!decoderRunning && !outputRunning) {
    serviceStopCleanup();
  }
}

void MediaPlayerPoC::stop()
{
  requestStop();

  // Synchronous Stop remains available for explicit user shutdown and legacy
  // call sites. Track changes use requestStop() and cooperative cleanup.
  unsigned long deadline = millis() + kTaskStopTimeoutMs;
  while ((decoderTaskRunning() || outputTaskRunning()) &&
         (long)(millis() - deadline) < 0) {
    // As soon as the output owner exits, ordinary STOP can disable TX even if
    // a slow SD read keeps the decoder task alive for longer.
    serviceStopCleanup();
    delay(2);
  }

  const bool decoderRunning = decoderTaskRunning();
  const bool outputRunning = outputTaskRunning();
  if (decoderRunning || outputRunning) {
    strlcpy(errorText, "media task is still stopping", sizeof(errorText));
    state = MediaPlaybackState::Draining;
    Serial.printf("MEDIA STOP: timeout decoder=%s output=%s; cleanup deferred\n",
                  decoderRunning ? "active" : "done",
                  outputRunning ? "active" : "done");
    return;
  }
  serviceStopCleanup();
}

bool MediaPlayerPoC::serviceStopCleanup()
{
  // Never delete TX while its output task owns the channel. Once that writer
  // has release-published its exit, a genuine stop can drive GPIO13 low without
  // waiting for an unrelated decoder/SD operation to finish.
  if (outputTaskRunning()) return false;
  if (state != MediaPlaybackState::Draining && !stopIsRequested()) {
    return state == MediaPlaybackState::Stopped;
  }

  if (spdifCarrierRetention() != SpdifCarrierRetention::Ready) stopSpdif();
  if (decoderTaskRunning()) return false;
  closeDecoder();
  resetRing();
  if (seekCommandQueue) xQueueReset(seekCommandQueue);
  if (seekEventQueue) xQueueReset(seekEventQueue);
  if (mediaControlEvents) {
    xEventGroupClearBits(mediaControlEvents,
                         kSeekActiveBit | kOutputQuiescentBit);
  }
  setStopRequested(false);
  decoderComplete = false;
  terminalEventPending = false;
  seekResumePaused = false;
  startStage = StartStage::Idle;
  startDataOutPin = -1;
  startRouteCallback = nullptr;
  startRouteContext = nullptr;
  startPrefillTarget = 0;
  state = MediaPlaybackState::Stopped;
  Serial.println("MEDIA STOP: deferred cleanup complete");
  return true;
}

bool MediaPlayerPoC::togglePause()
{
  if (state == MediaPlaybackState::Playing) {
    state = MediaPlaybackState::Paused;
    seekResumePaused = true;
    return true;
  }
  if (state == MediaPlaybackState::Paused) {
    state = MediaPlaybackState::Playing;
    seekResumePaused = false;
    return true;
  }
  if (state == MediaPlaybackState::Seeking) {
    seekResumePaused = !seekResumePaused;
    return true;
  }
  return false;
}

bool MediaPlayerPoC::requestSeek(uint64_t targetFrame)
{
  MediaPlaybackState current = state;
  if ((current != MediaPlaybackState::Playing &&
       current != MediaPlaybackState::Paused &&
       current != MediaPlaybackState::Seeking) ||
      !decoder || !decoderTaskRunning() || !seekCommandQueue ||
      !mediaControlEvents) {
    return false;
  }

  if (currentFile.totalFrames > 0) {
    targetFrame = std::min<uint64_t>(targetFrame,
                                     currentFile.totalFrames - 1);
  }

  bool resumePaused = current == MediaPlaybackState::Paused ||
                      (current == MediaPlaybackState::Seeking &&
                       seekResumePaused);
  uint32_t generation =
      __atomic_add_fetch(&seekGeneration, 1U, __ATOMIC_ACQ_REL);
  SeekCommand command;
  command.targetFrame = targetFrame;
  command.generation = generation;
  command.resumePaused = resumePaused;
  if (xQueueOverwrite(seekCommandQueue, &command) != pdPASS) return false;

  seekResumePaused = resumePaused;
  state = MediaPlaybackState::Seeking;
  __atomic_add_fetch(&stats.seekRequests, 1U, __ATOMIC_RELAXED);
  // The decoder polls this queue every 2 ms while buffered and every 20 ms at
  // end-of-file. Avoid notifying through a task handle that can self-delete.
  Serial.printf("MEDIA SEEK REQUEST generation=%lu target=%llu paused=%s\n",
                (unsigned long)generation,
                (unsigned long long)targetFrame,
                resumePaused ? "yes" : "no");
  return true;
}

bool MediaPlayerPoC::seeking() const
{
  return state == MediaPlaybackState::Seeking;
}

bool MediaPlayerPoC::takeSeekEvent(MediaSeekEvent &event)
{
  return seekEventQueue &&
         xQueueReceive(seekEventQueue, &event, 0) == pdTRUE;
}

bool MediaPlayerPoC::beginExternalHold(uint32_t timeoutMs)
{
  if (!active() || !outputTaskRunning() || !mediaControlEvents) return true;
  if (seeking()) return false;

  // Include the quiesce handshake itself in the expected-control window. The
  // output task may already be inside a bounded DMA write when the bit is set.
  suppressSpdifTimeoutsUntil = millis() + timeoutMs + 250U;
  xEventGroupClearBits(mediaControlEvents, kOutputQuiescentBit);
  xEventGroupSetBits(mediaControlEvents, kExternalHoldBit);
  EventBits_t bits = xEventGroupWaitBits(
      mediaControlEvents, kOutputQuiescentBit, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(timeoutMs));
  if ((bits & kOutputQuiescentBit) == 0) {
    xEventGroupClearBits(mediaControlEvents, kExternalHoldBit);
    suppressSpdifTimeoutsUntil = millis() + 100U;
    Serial.printf("MEDIA HOLD: quiesce timeout after %lu ms\n",
                  (unsigned long)timeoutMs);
    return false;
  }
  expectedHoldTimeouts = 0;
  Serial.println("MEDIA HOLD: output quiescent for DSP transaction");
  return true;
}

void MediaPlayerPoC::endExternalHold()
{
  if (!mediaControlEvents) return;
  // A DSPi control transaction can release immediately before the current
  // 25-ms DMA write returns. Keep a short grace window so that final expected
  // timeout is reported as transaction telemetry, not as an audio fault.
  suppressSpdifTimeoutsUntil = millis() + 300U;
  xEventGroupClearBits(mediaControlEvents, kExternalHoldBit);
  EventBits_t bits = xEventGroupGetBits(mediaControlEvents);
  if ((bits & kSeekActiveBit) == 0) {
    xEventGroupClearBits(mediaControlEvents, kOutputQuiescentBit);
  }
  uint32_t expected =
      __atomic_exchange_n(&expectedHoldTimeouts, 0U, __ATOMIC_ACQ_REL);
  Serial.printf("MEDIA HOLD: released expected_spdif_timeouts=%lu\n",
                (unsigned long)expected);
}

bool MediaPlayerPoC::externalHoldActive() const
{
  return mediaControlEvents &&
         (xEventGroupGetBits(mediaControlEvents) & kExternalHoldBit) != 0;
}

MediaPlaybackState MediaPlayerPoC::playbackState() const
{
  return state;
}

bool MediaPlayerPoC::active() const
{
  return state == MediaPlaybackState::Starting ||
         state == MediaPlaybackState::Playing ||
         state == MediaPlaybackState::Paused ||
         state == MediaPlaybackState::Seeking ||
         state == MediaPlaybackState::Draining;
}

bool MediaPlayerPoC::takeTerminalEvent(MediaPlaybackState &terminalState)
{
  if (!terminalEventPending) return false;
  terminalState = state;
  terminalEventPending = false;
  return terminalState == MediaPlaybackState::Finished ||
         terminalState == MediaPlaybackState::Error;
}

void MediaPlayerPoC::printPlaybackStatus(Stream &out) const
{
  MediaPlaybackStats snapshot = playbackStats();
  out.printf(
      "MEDIA STATUS: state=%s format=%s rate=%lu depth=%u channels=%u "
      "decoded=%llu output=%llu ring=%u/%u underrun=%lu events=%lu "
      "longest_underrun=%lu last_underrun_ms=%lu lowwater=%lu "
      "spdif_timeout=%lu spdif_partial=%lu spdif_error=%lu "
      "spdif_restart=%lu spdif_retained=%lu highwater=%lu "
      "dec_stack_free=%lu out_stack_free=%lu seek=%lu/%lu/%lu "
      "seek_ms=%lu sd_cb=%lu sd_slices=%lu sd_slow=%lu sd_err=%lu "
      "sd_yield=%lu cb_max_ms=%lu slice_max_ms=%lu "
      "spi_wait_max_ms=%lu sd_xfer_max_ms=%lu "
      "decode_max_ms=%lu decode_slow=%lu decode_last_ms=%lu "
      "last_decode_slow_ms=%lu ring_ms=%lu/%lu sd_clock=%lu path=%s",
      stateName(state), formatName(currentFile.format),
      (unsigned long)currentFile.sampleRate, currentFile.bitsPerSample,
      currentFile.channels, (unsigned long long)snapshot.decodedFrames,
      (unsigned long long)snapshot.outputFrames, (unsigned)ringAvailable(),
      (unsigned)ringFrameCapacity, (unsigned long)snapshot.underrunFrames,
      (unsigned long)snapshot.underrunEvents,
      (unsigned long)snapshot.longestUnderrunFrames,
      (unsigned long)snapshot.lastUnderrunAtMs,
      (unsigned long)snapshot.ringLowWaterFrames,
      (unsigned long)snapshot.spdifTimeouts,
      (unsigned long)snapshot.spdifPartialWrites,
      (unsigned long)snapshot.spdifErrors,
      (unsigned long)snapshot.spdifCarrierRestarts,
      (unsigned long)snapshot.spdifRetainedTransitions,
      (unsigned long)snapshot.ringHighWaterFrames,
      (unsigned long)snapshot.decoderStackMinFree,
      (unsigned long)snapshot.outputStackMinFree,
      (unsigned long)snapshot.seekRequests,
      (unsigned long)snapshot.seekCompleted,
      (unsigned long)snapshot.seekFailed,
      (unsigned long)snapshot.lastSeekElapsedMs,
      (unsigned long)snapshot.sdReadCalls,
      (unsigned long)snapshot.sdReadSlices,
      (unsigned long)snapshot.sdSlowReads,
      (unsigned long)snapshot.sdReadErrors,
      (unsigned long)snapshot.sdReadYields,
      (unsigned long)snapshot.sdReadMaxMs,
      (unsigned long)snapshot.sdReadSliceMaxMs,
      (unsigned long)snapshot.sdSpiWaitMaxMs,
      (unsigned long)snapshot.sdTransferMaxMs,
      (unsigned long)snapshot.decoderCallMaxMs,
      (unsigned long)snapshot.decoderSlowCalls,
      (unsigned long)snapshot.decoderLastCallMs,
      (unsigned long)snapshot.decoderLastSlowAtMs,
      currentFile.sampleRate
          ? (unsigned long)(((uint64_t)ringAvailable() * 1000ULL) /
                            currentFile.sampleRate)
          : 0UL,
      currentFile.sampleRate
          ? (unsigned long)(((uint64_t)ringFrameCapacity * 1000ULL) /
                            currentFile.sampleRate)
          : 0UL,
      (unsigned long)mountedFrequencyHz,
      currentPath[0] ? currentPath : "(none)");
  if (state == MediaPlaybackState::Error && errorText[0]) {
    out.printf(" error=%s", errorText);
  }
  out.println();
}

const char *MediaPlayerPoC::playbackPath() const
{
  return currentPath;
}

const char *MediaPlayerPoC::playbackError() const
{
  return errorText;
}

MediaFileInfo MediaPlayerPoC::playbackFile() const
{
  return currentFile;
}

MediaPlaybackStats MediaPlayerPoC::playbackStats() const
{
  MediaPlaybackStats snapshot;
  snapshot.decodedFrames =
      __atomic_load_n(&stats.decodedFrames, __ATOMIC_ACQUIRE);
  snapshot.outputFrames =
      __atomic_load_n(&stats.outputFrames, __ATOMIC_ACQUIRE);
  snapshot.underrunFrames =
      __atomic_load_n(&stats.underrunFrames, __ATOMIC_ACQUIRE);
  snapshot.underrunEvents =
      __atomic_load_n(&stats.underrunEvents, __ATOMIC_ACQUIRE);
  snapshot.longestUnderrunFrames =
      __atomic_load_n(&stats.longestUnderrunFrames, __ATOMIC_ACQUIRE);
  snapshot.lastUnderrunAtMs =
      __atomic_load_n(&stats.lastUnderrunAtMs, __ATOMIC_ACQUIRE);
  snapshot.spdifTimeouts =
      __atomic_load_n(&spdifTimeoutsLifetime, __ATOMIC_ACQUIRE);
  snapshot.spdifPartialWrites =
      __atomic_load_n(&spdifPartialWritesLifetime, __ATOMIC_ACQUIRE);
  snapshot.spdifErrors =
      __atomic_load_n(&spdifErrorsLifetime, __ATOMIC_ACQUIRE);
  snapshot.spdifCarrierRestarts =
      __atomic_load_n(&spdifCarrierRestartsLifetime, __ATOMIC_ACQUIRE);
  snapshot.spdifRetainedTransitions =
      __atomic_load_n(&spdifRetainedTransitionsLifetime, __ATOMIC_ACQUIRE);
  snapshot.ringHighWaterFrames =
      __atomic_load_n(&stats.ringHighWaterFrames, __ATOMIC_ACQUIRE);
  snapshot.ringLowWaterFrames =
      __atomic_load_n(&stats.ringLowWaterFrames, __ATOMIC_ACQUIRE);
  snapshot.decoderStackMinFree =
      __atomic_load_n(&stats.decoderStackMinFree, __ATOMIC_ACQUIRE);
  snapshot.outputStackMinFree =
      __atomic_load_n(&stats.outputStackMinFree, __ATOMIC_ACQUIRE);
  snapshot.seekRequests =
      __atomic_load_n(&stats.seekRequests, __ATOMIC_ACQUIRE);
  snapshot.seekCompleted =
      __atomic_load_n(&stats.seekCompleted, __ATOMIC_ACQUIRE);
  snapshot.seekFailed =
      __atomic_load_n(&stats.seekFailed, __ATOMIC_ACQUIRE);
  snapshot.lastSeekElapsedMs =
      __atomic_load_n(&stats.lastSeekElapsedMs, __ATOMIC_ACQUIRE);
  snapshot.sdReadCalls =
      __atomic_load_n(&stats.sdReadCalls, __ATOMIC_ACQUIRE);
  snapshot.sdReadSlices =
      __atomic_load_n(&stats.sdReadSlices, __ATOMIC_ACQUIRE);
  snapshot.sdSlowReads =
      __atomic_load_n(&stats.sdSlowReads, __ATOMIC_ACQUIRE);
  snapshot.sdReadErrors =
      __atomic_load_n(&stats.sdReadErrors, __ATOMIC_ACQUIRE);
  snapshot.sdReadYields =
      __atomic_load_n(&stats.sdReadYields, __ATOMIC_ACQUIRE);
  snapshot.sdReadMaxMs =
      __atomic_load_n(&stats.sdReadMaxMs, __ATOMIC_ACQUIRE);
  snapshot.sdReadSliceMaxMs =
      __atomic_load_n(&stats.sdReadSliceMaxMs, __ATOMIC_ACQUIRE);
  snapshot.sdSpiWaitMaxMs =
      __atomic_load_n(&stats.sdSpiWaitMaxMs, __ATOMIC_ACQUIRE);
  snapshot.sdTransferMaxMs =
      __atomic_load_n(&stats.sdTransferMaxMs, __ATOMIC_ACQUIRE);
  snapshot.decoderCallMaxMs =
      __atomic_load_n(&stats.decoderCallMaxMs, __ATOMIC_ACQUIRE);
  snapshot.decoderSlowCalls =
      __atomic_load_n(&stats.decoderSlowCalls, __ATOMIC_ACQUIRE);
  snapshot.decoderLastCallMs =
      __atomic_load_n(&stats.decoderLastCallMs, __ATOMIC_ACQUIRE);
  snapshot.decoderLastSlowAtMs =
      __atomic_load_n(&stats.decoderLastSlowAtMs, __ATOMIC_ACQUIRE);
  return snapshot;
}


size_t MediaPlayerPoC::bufferedFrames() const
{
  return ringAvailable();
}

size_t MediaPlayerPoC::ringCapacityFrames() const
{
  return ringFrameCapacity;
}


void MediaPlayerPoC::decoderTaskEntry(void *context)
{
  static_cast<MediaPlayerPoC *>(context)->decoderTask();
}

void MediaPlayerPoC::outputTaskEntry(void *context)
{
  static_cast<MediaPlayerPoC *>(context)->outputTask();
}

void MediaPlayerPoC::decoderTask()
{
  updateTaskStackWatermark(true);
  while (!stopIsRequested()) {
    SeekCommand command;
    if (seekCommandQueue &&
        xQueueReceive(seekCommandQueue, &command, 0) == pdTRUE) {
      performSeek(command);
      updateTaskStackWatermark(true);
      continue;
    }

    if (decoderComplete) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }

    size_t writable = ringWritable();
    if (writable < kDecodeChunkFrames) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
      continue;
    }

    size_t frames = decodeFrames(decoder->stereo, kDecodeChunkFrames);
    updateTaskStackWatermark(true);
    if (frames == 0) {
      if (storageIoFault && !stopIsRequested()) {
        reportStorageReadFault("decoder");
        cardMounted = false;
        setError("SD read failed or card removed");
        setStopRequested(true);
        Serial.printf("MEDIA SD: read fault path=%s; mount invalidated\n",
                      currentPath[0] ? currentPath : "(none)");
        break;
      }
      decoderComplete = true;
      continue;
    }
    __atomic_add_fetch(&stats.decodedFrames, frames, __ATOMIC_RELAXED);
    if (writeRing(decoder->stereo, frames) != frames) {
      setError("PCM ring write failed");
      setStopRequested(true);
      break;
    }
  }

  closeDecoder();
  decoderComplete = true;
  __atomic_store_n(&decoderTaskHandle, nullptr, __ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}

void MediaPlayerPoC::outputTask()
{
  int32_t output[kOutputChunkFrames * 2] = {};
  i2s_chan_handle_t tx = static_cast<i2s_chan_handle_t>(spdifTxChannel);
  constexpr size_t encodedWordCapacity =
      kOutputChunkFrames * SpdifBlockEncoder::kWordsPerStereoFrame;
  uint32_t *encoded = static_cast<uint32_t *>(heap_caps_malloc(
      encodedWordCapacity * sizeof(uint32_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  SpdifBlockEncoder encoder;
  if (!encoded) {
    setError("S/PDIF DMA staging allocation failed");
    setStopRequested(true);
    __atomic_store_n(&outputTaskHandle, nullptr, __ATOMIC_RELEASE);
    vTaskDelete(nullptr);
    return;
  }
  if (!encoder.setSampleRate(currentFile.sampleRate)) {
    heap_caps_free(encoded);
    setError("Unsupported S/PDIF channel-status rate");
    setStopRequested(true);
    __atomic_store_n(&outputTaskHandle, nullptr, __ATOMIC_RELEASE);
    vTaskDelete(nullptr);
    return;
  }
  encoder.reset();

  auto sendFrames = [&](const int32_t *samples, size_t frames) -> bool {
    if (!encoder.encode(samples, frames, encoded, encodedWordCapacity)) {
      __atomic_add_fetch(&spdifErrorsLifetime, 1U, __ATOMIC_RELAXED);
      setError("S/PDIF block encode failed");
      setStopRequested(true);
      return false;
    }
    const uint8_t *next = reinterpret_cast<const uint8_t *>(encoded);
    size_t remaining = frames * SpdifBlockEncoder::kWordsPerStereoFrame *
                       sizeof(uint32_t);
    while (remaining) {
      // Once encoded, a retaining transition must submit the complete buffer
      // so encoder phase and queued bytes cannot diverge. An ordinary STOP
      // clears the retention token and may abort because TX will be deleted.
      if (stopIsRequested() &&
          spdifCarrierRetention() != SpdifCarrierRetention::Requested) {
        return false;
      }

      const size_t requested = remaining;
      size_t written = 0;
      esp_err_t result = i2s_channel_write(tx, next, remaining, &written, 25);
      if (written > requested) {
        __atomic_add_fetch(&spdifErrorsLifetime, 1U, __ATOMIC_RELAXED);
        setError("S/PDIF DMA write count invalid");
        setStopRequested(true);
        return false;
      }
      if (written) {
        if (written < requested) {
          __atomic_add_fetch(&spdifPartialWritesLifetime, 1U,
                             __ATOMIC_RELAXED);
        }
        next += written;
        remaining -= written;
      }
      if (result == ESP_ERR_TIMEOUT) {
        EventBits_t bits = mediaControlEvents
            ? xEventGroupGetBits(mediaControlEvents) : 0;
        const bool expectedControlTimeout =
            (bits & kExternalHoldBit) ||
            (int32_t)(millis() - suppressSpdifTimeoutsUntil) < 0;
        if (expectedControlTimeout) {
          __atomic_add_fetch(&expectedHoldTimeouts, 1U, __ATOMIC_RELAXED);
        } else {
          __atomic_add_fetch(&spdifTimeoutsLifetime, 1U, __ATOMIC_RELAXED);
        }
        continue;
      }
      if (result != ESP_OK) {
        __atomic_add_fetch(&spdifErrorsLifetime, 1U, __ATOMIC_RELAXED);
        setError("S/PDIF DMA write failed");
        setStopRequested(true);
        return false;
      }
      if (written == 0 && remaining) {
        __atomic_add_fetch(&spdifErrorsLifetime, 1U, __ATOMIC_RELAXED);
        setError("S/PDIF DMA write made no progress");
        setStopRequested(true);
        return false;
      }
    }
    return true;
  };

  bool naturalEnd = false;
  bool underrunActive = false;
  uint32_t underrunRunFrames = 0;
  updateTaskStackWatermark(false);

  // DSPi is switched to S/PDIF before this task starts. Give its receiver a
  // continuous quarter-second consumer-PCM silence stream to acquire lock
  // before consuming the first buffered music frame.
  size_t leadInFrames = currentFile.sampleRate / 4u;
  while (leadInFrames && !stopIsRequested()) {
    const size_t frames = std::min(leadInFrames, kOutputChunkFrames);
    if (!sendFrames(nullptr, frames)) break;
    leadInFrames -= frames;
  }

  while (!stopIsRequested()) {
    EventBits_t control = mediaControlEvents
        ? xEventGroupGetBits(mediaControlEvents) : 0;
    if (control & (kSeekActiveBit | kExternalHoldBit)) {
      xEventGroupSetBits(mediaControlEvents, kOutputQuiescentBit);
      if (!sendFrames(nullptr, kOutputChunkFrames)) break;
      updateTaskStackWatermark(false);
      continue;
    }
    if (mediaControlEvents) {
      EventBits_t remaining = xEventGroupGetBits(mediaControlEvents);
      if ((remaining & (kSeekActiveBit | kExternalHoldBit)) == 0) {
        xEventGroupClearBits(mediaControlEvents, kOutputQuiescentBit);
      }
    }

    if (state == MediaPlaybackState::Paused ||
        state == MediaPlaybackState::Seeking) {
      if (!sendFrames(nullptr, kOutputChunkFrames)) break;
      updateTaskStackWatermark(false);
      continue;
    }

    size_t frames = readRing(output, kOutputChunkFrames);
    if (frames == 0) {
      if (decoderComplete) {
        naturalEnd = true;
        break;
      }
      if (!underrunActive) {
        underrunActive = true;
        underrunRunFrames = 0;
        __atomic_add_fetch(&stats.underrunEvents, 1U, __ATOMIC_RELAXED);
      }
      underrunRunFrames += (uint32_t)kOutputChunkFrames;
      __atomic_add_fetch(&stats.underrunFrames,
                         (uint32_t)kOutputChunkFrames,
                         __ATOMIC_RELAXED);
      __atomic_store_n(&stats.lastUnderrunAtMs, millis(), __ATOMIC_RELAXED);
      updateAtomicMaximum(&stats.longestUnderrunFrames,
                          underrunRunFrames);
      if (!sendFrames(nullptr, kOutputChunkFrames)) break;
      updateTaskStackWatermark(false);
      continue;
    }

    underrunActive = false;
    underrunRunFrames = 0;
    if (!decoderComplete) {
      updateAtomicMinimum(&stats.ringLowWaterFrames,
                          (uint32_t)ringAvailable());
    }

    if (!sendFrames(output, frames)) break;
    __atomic_add_fetch(&stats.outputFrames, frames, __ATOMIC_RELAXED);
    updateTaskStackWatermark(false);
  }

  if (naturalEnd && !stopIsRequested() &&
      state != MediaPlaybackState::Error) {
    state = MediaPlaybackState::Draining;
    size_t remaining = kTailSilenceFrames;
    while (remaining && !stopIsRequested()) {
      size_t frames = std::min(remaining, kOutputChunkFrames);
      if (!sendFrames(nullptr, frames)) break;
      remaining -= frames;
    }
    if (!stopIsRequested() && state != MediaPlaybackState::Error) {
      state = MediaPlaybackState::Finished;
      terminalEventPending = true;
      // Do not let the DMA ring run unattended while the main loop decides
      // whether to advance. Continue producing valid silence so the DSPi
      // receiver never has to reacquire a same-rate carrier.
      while (!stopIsRequested() && state != MediaPlaybackState::Error) {
        if (!sendFrames(nullptr, kOutputChunkFrames)) break;
      }
    }
  }

  if (stopIsRequested() &&
      spdifCarrierRetention() == SpdifCarrierRetention::Requested &&
      spdifTxChannel &&
      state != MediaPlaybackState::Error) {
    // Finish the current 192-frame channel-status block, then replace a full
    // DMA-ring span with encoded silence. The inactive transition interval can
    // consequently repeat only valid complete blocks, and the next output task
    // can safely restart its encoder at block frame zero.
    const size_t blockRemainder =
        (192u - (size_t)encoder.frameNumber()) % 192u;
    bool carrierReady = !blockRemainder ||
                        sendFrames(nullptr, blockRemainder);
    size_t silenceFrames =
        kSpdifDmaDescriptorCount * (kSpdifDmaFramesPerDescriptor / 2u);
    while (carrierReady && silenceFrames) {
      const size_t frames = std::min(silenceFrames, kOutputChunkFrames);
      if (!sendFrames(nullptr, frames)) {
        carrierReady = false;
        break;
      }
      silenceFrames -= frames;
    }
    if (carrierReady && silenceFrames == 0 &&
        publishRetainedSpdifCarrier()) {
      __atomic_add_fetch(&spdifRetainedTransitionsLifetime, 1U,
                         __ATOMIC_RELAXED);
      Serial.printf(
          "MEDIA SPDIF TX: carrier retained rate=%lu Hz silence=%u frames\n",
          (unsigned long)spdifSampleRate,
          (unsigned)(kSpdifDmaDescriptorCount *
                     (kSpdifDmaFramesPerDescriptor / 2u)));
    } else {
      setSpdifCarrierRetention(SpdifCarrierRetention::Off);
      Serial.println("MEDIA SPDIF TX: carrier retention failed; teardown required");
    }
  }

  if (mediaControlEvents) {
    xEventGroupClearBits(mediaControlEvents, kOutputQuiescentBit);
  }
  free(encoded);
  __atomic_store_n(&outputTaskHandle, nullptr, __ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}
