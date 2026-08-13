#pragma once

#include <Arduino.h>
#include "MediaFs.h"
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

void mediaSharedSpiLock();
bool mediaSharedSpiTryLock(uint32_t timeoutMs);
void mediaSharedSpiUnlock();

enum class MediaFileFormat : uint8_t {
  Unknown = 0,
  Wav,
  Flac,
  Mp3
};

struct MediaFileInfo {
  MediaFileFormat format = MediaFileFormat::Unknown;
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
  uint8_t bitsPerSample = 0;
  uint64_t totalFrames = 0;
  bool valid = false;
  bool nativeRateSupported = false;
};

enum class MediaPlaybackState : uint8_t {
  Stopped = 0,
  Starting,
  Playing,
  Paused,
  Seeking,
  Draining,
  Finished,
  Error
};

struct MediaPlaybackStats {
  uint64_t decodedFrames = 0;
  uint64_t outputFrames = 0;
  uint32_t underrunFrames = 0;
  uint32_t underrunEvents = 0;
  uint32_t longestUnderrunFrames = 0;
  uint32_t lastUnderrunAtMs = 0;
  uint32_t spdifTimeouts = 0;
  uint32_t spdifErrors = 0;
  uint32_t ringHighWaterFrames = 0;
  uint32_t ringLowWaterFrames = 0;
  uint32_t decoderStackMinFree = 0;
  uint32_t outputStackMinFree = 0;
  uint32_t seekRequests = 0;
  uint32_t seekCompleted = 0;
  uint32_t seekFailed = 0;
  uint32_t lastSeekElapsedMs = 0;
  uint32_t sdReadCalls = 0;
  uint32_t sdReadSlices = 0;
  uint32_t sdSlowReads = 0;
  uint32_t sdReadErrors = 0;
  uint32_t sdReadYields = 0;
  uint32_t sdReadMaxMs = 0;
  uint32_t sdReadSliceMaxMs = 0;
  uint32_t sdSpiWaitMaxMs = 0;
  uint32_t sdTransferMaxMs = 0;
  uint32_t decoderCallMaxMs = 0;
  uint32_t decoderSlowCalls = 0;
  uint32_t decoderLastCallMs = 0;
  uint32_t decoderLastSlowAtMs = 0;
};

enum class MediaSeekResult : uint8_t {
  Completed = 0,
  Failed,
  Cancelled
};

struct MediaSeekEvent {
  MediaSeekResult result = MediaSeekResult::Cancelled;
  uint64_t requestedFrame = 0;
  uint64_t actualFrame = 0;
  uint32_t generation = 0;
  uint32_t elapsedMs = 0;
};

enum class MediaStartStatus : uint8_t {
  Idle = 0,
  Preparing,
  Started,
  Failed
};

struct MediaPlayableEntry {
  char path[MEDIA_FS_PATH_CAPACITY] = {0};
  MediaFileInfo info;
};

struct MediaBrowserEntry {
  char path[MEDIA_FS_PATH_CAPACITY] = {0};
  char name[MEDIA_FS_NAME_CAPACITY] = {0};
  bool directory = false;
  MediaFileFormat format = MediaFileFormat::Unknown;
};

// Directory pages use a stable, case-insensitive folders-first ordering. The
// browser keeps only one small page in RAM and rescans the directory when the
// user crosses a page boundary. This removes the former fixed directory-size
// ceiling while preserving deterministic alphabetical navigation.
enum class MediaDirectoryPageMode : uint8_t {
  First = 0,
  After,
  Before,
  AtOrAfter,
  AtOrBefore
};

struct MediaDirectoryPageInfo {
  uint32_t rawEntriesScanned = 0;
  uint32_t totalEntries = 0;
  uint32_t eligibleEntries = 0;
  uint32_t firstEntryIndex = 0;
  uint32_t skippedLongPaths = 0;
  uint32_t skippedUnsupportedFiles = 0;
  uint32_t skippedHiddenEntries = 0;
  bool hasPrevious = false;
  bool hasNext = false;
};

using MediaRouteCallback = bool (*)(uint32_t sampleRate, void *context);

// Optional cooperative control for background artwork work.  The callbacks
// run from the low-priority artwork task. waitForPermit may block/yield until
// SD access is safe; cancelRequested aborts stale work after track changes.

enum class MediaArtworkSource : uint8_t {
  None = 0,
  Mp3Apic,
  FlacPicture,
  FolderJpeg
};

struct MediaArtworkControl {
  bool (*waitForPermit)(void *context) = nullptr;
  bool (*cancelRequested)(void *context) = nullptr;
  void *context = nullptr;
};

class MediaPlayerPoC {
public:
  bool begin(SPIClass &spi, int8_t chipSelectPin, int8_t clockPin,
             int8_t misoPin, int8_t mosiPin);
  bool mountCard(uint32_t lockTimeoutMs = 250U);
  bool mountCardForTransfer(uint32_t lockTimeoutMs = 250U);
  // Hand the already-mounted card between the normal read-only browser and
  // exclusive transfer writer without restarting the shared SPI peripheral.
  // All playback/tasks must already be stopped.  The operation is bounded so
  // loopTask can retry instead of freezing behind an LCD/SPI owner.
  bool switchMountedCardAccessMode(MediaFsAccessMode accessMode,
                                   uint32_t lockTimeoutMs = 250U);
  bool unmountCard(uint32_t lockTimeoutMs = 250U);
  // Close SdFat, release the shared SPI peripheral and leave the SD
  // bus electrically idle before an ESP32 software restart.
  bool prepareCardForControllerRestart(uint32_t lockTimeoutMs = 1000U);
  bool mounted() const;
  MediaFsAccessMode cardAccessMode() const;
  bool prepareAudioBuffer();
  void printCardSummary(Stream &out) const;
  void printDirectory(Stream &out, const char *path, size_t maxEntries = 48);
  size_t listDirectory(const char *path, MediaBrowserEntry *entries,
                       size_t capacity, size_t maxEntries = 64);
  size_t listDirectoryPage(const char *path, MediaBrowserEntry *entries,
                           size_t capacity, MediaDirectoryPageMode mode,
                           const MediaBrowserEntry *anchor,
                           MediaDirectoryPageInfo &pageInfo);
  size_t listRootPlayable(MediaPlayableEntry *entries, size_t capacity,
                          size_t maxEntries = 48);
  bool probeFile(const char *path, MediaFileInfo &info);
  bool loadArtworkJpeg(const char *audioPath, uint8_t **data, size_t *length,
                       const MediaArtworkControl *control = nullptr,
                       MediaArtworkSource *source = nullptr);
  bool loadFolderArtworkJpeg(const char *audioPath, uint8_t **data,
                             size_t *length,
                             const MediaArtworkControl *control = nullptr);
  static const char *artworkSourceName(MediaArtworkSource source);
  static void freeArtworkJpeg(uint8_t *data);

  bool beginPlay(const char *path, int8_t dataOutPin,
                 MediaRouteCallback routeCallback,
                 void *routeContext, Stream &out);
  MediaStartStatus servicePlayStart(Stream &out,
                                    size_t decodeChunkBudget = 2);
  bool play(const char *path, int8_t dataOutPin,
            MediaRouteCallback routeCallback,
            void *routeContext, Stream &out);
  void requestStop();
  void stop();
  bool serviceStopCleanup();
  bool togglePause();
  bool requestSeek(uint64_t targetFrame);
  bool seeking() const;
  bool takeSeekEvent(MediaSeekEvent &event);
  bool beginExternalHold(uint32_t timeoutMs = 600);
  void endExternalHold();
  bool externalHoldActive() const;
  MediaPlaybackState playbackState() const;
  bool active() const;
  bool takeTerminalEvent(MediaPlaybackState &terminalState);
  void printPlaybackStatus(Stream &out) const;
  const char *playbackPath() const;
  const char *playbackError() const;
  MediaFileInfo playbackFile() const;
  MediaPlaybackStats playbackStats() const;
  size_t bufferedFrames() const;
  size_t ringCapacityFrames() const;

  static bool isPlayablePath(const char *path);
  static const char *formatName(MediaFileFormat format);
  static const char *stateName(MediaPlaybackState state);

private:
  struct DecoderState;
  struct SeekCommand;
  enum class Mp3SeekAttemptResult : uint8_t;
  struct Mp3SeekAttempt;
  enum class StartStage : uint8_t {
    Idle = 0,
    Prefill,
    Activate
  };

  bool probeWav(MediaFsFile &file, MediaFileInfo &info);
  bool probeFlac(MediaFsFile &file, MediaFileInfo &info);
  bool probeMp3(MediaFsFile &file, MediaFileInfo &info);
  bool mountCardWithMode(MediaFsAccessMode accessMode,
                         uint32_t lockTimeoutMs);

  bool openDecoder(const char *path, MediaFileInfo &info);
  bool openDecoderState(const char *path, DecoderState *&target,
                        MediaFileInfo &info);
  bool seekDecoderToFrame(uint64_t frame, uint32_t generation = 0);
  Mp3SeekAttempt seekMp3CandidateToFrame(DecoderState &candidate,
                                         uint64_t frame,
                                         uint32_t generation,
                                         uint32_t budgetMs);
  bool prefillMp3Candidate(DecoderState &candidate, int32_t *prefill,
                           size_t capacityFrames, size_t &framesWritten,
                           bool &reachedEnd, uint32_t generation,
                           Mp3SeekAttemptResult &failure);
  static const char *mp3SeekAttemptName(Mp3SeekAttemptResult result);
  bool performSeek(SeekCommand command);
  bool prefillAfterSeek();
  void publishSeekEvent(MediaSeekResult result, uint64_t requestedFrame,
                        uint64_t actualFrame, uint32_t generation,
                        uint32_t elapsedMs);
  void resetRing();
  void updateTaskStackWatermark(bool decoderTask);
  size_t decodeFrames(int32_t *stereoOutput, size_t maximumFrames);
  size_t decodeFramesFrom(DecoderState &source, const MediaFileInfo &info,
                          int32_t *stereoOutput, size_t maximumFrames,
                          bool &ioFault);
  void closeDecoderState(DecoderState *&target);
  void closeDecoder();
  bool allocateRing();
  void freeRing();
  size_t ringAvailable() const;
  size_t ringWritable() const;
  size_t writeRing(const int32_t *stereoInput, size_t frames);
  size_t readRing(int32_t *stereoOutput, size_t frames);
  bool startSpdif(int8_t dataOutPin);
  void stopSpdif();
  void setError(const char *message);
  void reportStorageReadFault(const char *stage);

  static void decoderTaskEntry(void *context);
  static void outputTaskEntry(void *context);
  void decoderTask();
  void outputTask();

  volatile bool cardMounted = false;
  uint8_t mountedCardType = 0;
  uint64_t mountedCardSizeBytes = 0;
  uint32_t mountedFrequencyHz = 0;
  SPIClass *mediaSpi = nullptr;
  int8_t mediaChipSelectPin = -1;
  int8_t mediaClockPin = -1;
  int8_t mediaMisoPin = -1;
  int8_t mediaMosiPin = -1;
  volatile uint32_t expectedHoldTimeouts = 0;
  volatile uint32_t suppressSpdifTimeoutsUntil = 0;

  // Main-loop-only browser scan scratch. MediaFsFile carries 512-byte path and
  // name buffers, so keeping these objects in the long-lived player instance
  // prevents nested folder restores from exhausting Arduino's loopTask stack.
  MediaFsFile browserDirectoryScratch;
  MediaFsFile browserEntryScratch;
  MediaBrowserEntry browserCandidateScratch;

  DecoderState *decoder = nullptr;
  int32_t *pcmRing = nullptr;
  void *spdifTxChannel = nullptr;
  int8_t spdifDataOutPin = -1;
  TaskHandle_t decoderTaskHandle = nullptr;
  TaskHandle_t outputTaskHandle = nullptr;
  QueueHandle_t seekCommandQueue = nullptr;
  QueueHandle_t seekEventQueue = nullptr;
  EventGroupHandle_t mediaControlEvents = nullptr;
  volatile uint32_t ringReadCount = 0;
  volatile uint32_t ringWriteCount = 0;
  size_t ringFrameCapacity = 0;
  StartStage startStage = StartStage::Idle;
  int8_t startDataOutPin = -1;
  MediaRouteCallback startRouteCallback = nullptr;
  void *startRouteContext = nullptr;
  size_t startPrefillTarget = 0;
  volatile bool stopRequested = false;
  volatile bool decoderComplete = false;
  volatile bool storageIoFault = false;
  volatile bool terminalEventPending = false;
  volatile bool seekResumePaused = false;
  volatile uint32_t seekGeneration = 0;
  volatile MediaPlaybackState state = MediaPlaybackState::Stopped;
  MediaFileInfo currentFile;
  MediaPlaybackStats stats;
  char currentPath[MEDIA_FS_PATH_CAPACITY] = {0};
  char errorText[96] = {0};
};
