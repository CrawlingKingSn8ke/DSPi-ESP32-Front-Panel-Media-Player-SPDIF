#include "WifiTransferMode.h"

#include <WiFi.h>
#include <algorithm>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_app_format.h>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <new>
#include <utility>

#include "MediaPlayerPoC.h"
#include "SpdifDiagnostics.h"
#include "WifiTransferWeb.h"

namespace {

constexpr uint16_t kHttpPort = 80;
constexpr uint32_t kTransferTaskStackBytes = 16U * 1024U;
constexpr UBaseType_t kTransferTaskPriority = 2;
// Batch WebServer's ~1436-byte raw chunks into fewer SdFat writes. Prefer
// internal memory because ESP32 SPI DMA paths are most predictable there; use
// PSRAM only if the internal allocation is unavailable.
constexpr size_t kUploadBufferBytes = 32U * 1024U;
constexpr size_t kUploadPipelineBufferCount = 2U;
constexpr uint32_t kUploadWriterTaskStackBytes = 8U * 1024U;
constexpr UBaseType_t kUploadWriterTaskPriority = 3;
constexpr BaseType_t kUploadWriterTaskCore = 1;
constexpr TickType_t kUploadPipelineWaitTicks = pdMS_TO_TICKS(15000U);
constexpr uint64_t kPreallocateMinimumBytes = 1ULL * 1024ULL * 1024ULL;
// Hardware telemetry showed 36 full file syncs during a 37 MiB upload.
// Temporary files remain isolated by the .uploading suffix, so checkpoint at
// 16 MiB and always perform the existing final file/device sync before rename.
constexpr uint64_t kFileSyncIntervalBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint32_t kProgressLogIntervalMs = 1000;
constexpr uint64_t kProgressLogIntervalBytes = 4ULL * 1024ULL * 1024ULL;
// WebServer emits roughly 1436-byte raw-body chunks. Yielding a complete tick
// after every chunk imposed tens of seconds of artificial delay. Yield only
// after a useful amount of data has been accepted.
constexpr uint64_t kNetworkYieldIntervalBytes = 256ULL * 1024ULL;
constexpr char kHeaderBase[] = "X-DSPi-Base";
constexpr char kHeaderPath[] = "X-DSPi-Path";
constexpr char kHeaderDeclaredSize[] = "X-DSPi-Declared-Size";
constexpr char kHeaderDeleteConfirm[] = "X-DSPi-Confirm";
constexpr char kHeaderDeleteKind[] = "X-DSPi-Entry-Type";
constexpr size_t kFolderDeleteMaxDepth = 32U;
constexpr uint32_t kFolderDeleteMaxEntries = 20000U;
constexpr char kDirectApPassword[] = "WeeblabsESP";
constexpr char kWifiPreferencesNamespace[] = "dspi_xfer";
constexpr char kWifiSsidKey[] = "sta_ssid";
constexpr char kWifiPasswordKey[] = "sta_pass";
constexpr char kMdnsHost[] = "dspi-transfer";
constexpr uint32_t kStationConnectTimeoutMs = 15000U;
constexpr uint32_t kFirmwareResponseDrainMs = 1800U;
constexpr uint64_t kMinimumFirmwareImageBytes = 256U * 1024U;

static_assert(
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
            sizeof(esp_app_desc_t) == 288U,
    "OTA validation prefix must match ESP-IDF image layout");

enum class UploadWriterCommandType : uint8_t {
  Write = 0,
  Barrier,
  Shutdown
};

struct UploadWriterCommand {
  UploadWriterCommandType type = UploadWriterCommandType::Write;
  uint8_t bufferIndex = 0;
  bool syncAfterWrite = false;
  size_t length = 0;
};

class SharedSpiGuard {
public:
  SharedSpiGuard() { mediaSharedSpiLock(); }
  ~SharedSpiGuard() { mediaSharedSpiUnlock(); }
  SharedSpiGuard(const SharedSpiGuard &) = delete;
  SharedSpiGuard &operator=(const SharedSpiGuard &) = delete;
};

uint32_t elapsedSince(uint32_t start, uint32_t now) {
  return now - start;
}

const char *uploadPhaseText(WifiTransferPolicy::UploadPhase phase) {
  using WifiTransferPolicy::UploadPhase;
  switch (phase) {
    case UploadPhase::Idle: return "IDLE";
    case UploadPhase::Writing: return "WRITING";
    case UploadPhase::ClosingIncomplete: return "CLOSING_INCOMPLETE";
    case UploadPhase::Finalizing: return "FINALIZING";
    case UploadPhase::Complete: return "COMPLETE";
    case UploadPhase::Incomplete: return "INCOMPLETE";
    default: return "UNKNOWN";
  }
}

bool endsWithUploading(const char *text) {
  return text &&
         WifiTransferPolicy::hasUploadingSuffix(text, strlen(text));
}

void copyDisplayText(char *destination, size_t capacity,
                     const char *source) {
  if (!destination || capacity == 0) return;
  destination[0] = '\0';
  if (!source) return;
  const size_t length = strlen(source);
  if (length < capacity) {
    memcpy(destination, source, length + 1U);
    return;
  }
  if (capacity <= 4U) return;
  memcpy(destination, source, capacity - 4U);
  memcpy(destination + capacity - 4U, "...", 4U);
}

}  // namespace

struct WifiTransferModeController::Workspace {
  struct ScannedNetwork {
    char ssid[33] = {0};
    int32_t rssi = INT32_MIN;
    int32_t channel = 0;
    bool secure = true;
  };

  static constexpr size_t kMaximumScannedNetworks = 32U;

  // MediaFsFile contains two 512-byte path/name fields. Keep every such object
  // in the PSRAM-preferred workspace, not on loopTask or callback stacks.
  MediaFsFile preflightProbeFile;
  MediaFsFile uploadFile;
  MediaFsFile listingDirectory;
  MediaFsFile listingEntry;
  MediaFsFile directoryTypeScratch;
  MediaFsFile deleteEntry;
  MediaFsFile deleteDirectories[kFolderDeleteMaxDepth];

  char transferRoot[WifiTransferPolicy::kPathCapacity] = {0};
  char canonicalBase[WifiTransferPolicy::kPathCapacity] = {0};
  char childFinal[WifiTransferPolicy::kPathCapacity] = {0};
  char childTemporary[WifiTransferPolicy::kPathCapacity] = {0};
  char combinedFinal[WifiTransferPolicy::kPathCapacity] = {0};
  char combinedTemporary[WifiTransferPolicy::kPathCapacity] = {0};
  char absoluteFinal[WifiTransferPolicy::kPathCapacity] = {0};
  char absoluteTemporary[WifiTransferPolicy::kPathCapacity] = {0};
  char absoluteDirectory[WifiTransferPolicy::kPathCapacity] = {0};
  char scratchPath[WifiTransferPolicy::kPathCapacity] = {0};
  char jsonChunk[192] = {0};
  ScannedNetwork scannedNetworks[kMaximumScannedNetworks];
  size_t scannedNetworkCount = 0;
  uint8_t *uploadBuffers[kUploadPipelineBufferCount] = {nullptr, nullptr};
  uint8_t *uploadBuffer = nullptr;
  size_t uploadBufferCapacity = 0;
  size_t uploadBufferUsed = 0;
  uint8_t uploadBufferIndex = 0xFFU;
  QueueHandle_t uploadWriterQueue = nullptr;
  QueueHandle_t uploadFreeQueue = nullptr;
  SemaphoreHandle_t uploadBarrier = nullptr;
  TaskHandle_t uploadWriterTask = nullptr;
  volatile bool uploadWriterFailed = false;
  volatile bool uploadBarrierOk = false;
  bool uploadPipelineAvailable = false;
  bool uploadPreallocated = false;

  size_t canonicalBaseLength = 0;
  size_t childFinalLength = 0;
  size_t childTemporaryLength = 0;
  size_t combinedFinalLength = 0;
  size_t combinedTemporaryLength = 0;
  uint64_t writerDeclaredBytes = 0;
  uint64_t writerReceivedBytes = 0;
  uint64_t writerPersistedBytes = 0;
  uint64_t bytesAtLastSync = 0;
  uint64_t bytesAtLastProgressLog = 0;
  uint64_t bytesAtLastYield = 0;
  uint32_t writerStartedMs = 0;
  uint32_t lastProgressLogMs = 0;
  uint32_t writerSyncCount = 0;

  // v13.2.8 pipelined upload telemetry. These counters distinguish
  // time waiting for the next TCP/raw-body callback from time spent copying,
  // writing, syncing, yielding, and atomically finalising the SD file.
  uint64_t writerStartedUs = 0;
  uint64_t startPreparationUs = 0;
  uint64_t lastRawCallbackEndUs = 0;
  uint64_t firstRawWriteUs = 0;
  uint64_t lastRawWriteEndUs = 0;
  uint64_t rawGapTotalUs = 0;
  uint64_t rawGapMaxUs = 0;
  uint64_t rawCallbackTotalUs = 0;
  uint64_t rawCallbackMaxUs = 0;
  uint64_t rawCopyTotalUs = 0;
  uint64_t rawCopyMaxUs = 0;
  uint64_t spiLockWaitTotalUs = 0;
  uint64_t spiLockWaitMaxUs = 0;
  uint64_t sdWriteTotalUs = 0;
  uint64_t sdWriteMaxUs = 0;
  uint64_t sdWriteBytes = 0;
  uint64_t fileSyncTotalUs = 0;
  uint64_t fileSyncMaxUs = 0;
  uint64_t yieldTotalUs = 0;
  uint64_t yieldMaxUs = 0;
  uint64_t pipelineQueueWaitTotalUs = 0;
  uint64_t pipelineQueueWaitMaxUs = 0;
  uint64_t pipelineBarrierWaitUs = 0;
  uint64_t preallocateUs = 0;
  uint64_t finalizeTotalUs = 0;
  uint64_t finalFileSyncUs = 0;
  uint64_t deviceSyncBeforeRenameUs = 0;
  uint64_t renameUs = 0;
  uint64_t deviceSyncAfterRenameUs = 0;
  uint32_t rawCallbackCount = 0;
  uint32_t rawChunkMinBytes = 0;
  uint32_t rawChunkMaxBytes = 0;
  uint32_t sdWriteCount = 0;
  uint32_t yieldCount = 0;
  uint32_t pipelineQueuedBlocks = 0;
  uint32_t pipelineBackpressureCount = 0;
  int32_t uploadRssiDbm = 0;
  uint8_t uploadWifiChannel = 0;
  uint8_t uploadApClients = 0;

  int uploadResponseCode = 500;
  bool uploadResponseReady = false;
  char uploadResponseReason[48] = {0};
  char uploadResponseMessage[128] = {0};
};

WifiTransferModeController wifiTransferMode;

bool WifiTransferModeController::copyText(char *destination, size_t capacity,
                                          const char *source) {
  if (!destination || capacity == 0) return false;
  destination[0] = '\0';
  if (!source) return false;
  const size_t length = strlen(source);
  if (length >= capacity) return false;
  memcpy(destination, source, length + 1U);
  return true;
}

bool WifiTransferModeController::startUploadPipeline() {
  if (!workspace_ || workspace_->uploadPipelineAvailable) return false;

  constexpr uint32_t dmaCaps =
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
  for (size_t index = 0; index < kUploadPipelineBufferCount; ++index) {
    workspace_->uploadBuffers[index] = static_cast<uint8_t *>(
        heap_caps_malloc(kUploadBufferBytes, dmaCaps));
    if (!workspace_->uploadBuffers[index]) {
      stopUploadPipeline();
      return false;
    }
  }

  workspace_->uploadWriterQueue = xQueueCreate(
      kUploadPipelineBufferCount + 2U, sizeof(UploadWriterCommand));
  workspace_->uploadFreeQueue = xQueueCreate(
      kUploadPipelineBufferCount, sizeof(uint8_t));
  workspace_->uploadBarrier = xSemaphoreCreateBinary();
  if (!workspace_->uploadWriterQueue || !workspace_->uploadFreeQueue ||
      !workspace_->uploadBarrier) {
    stopUploadPipeline();
    return false;
  }

  for (uint8_t index = 0; index < kUploadPipelineBufferCount; ++index) {
    if (xQueueSend(workspace_->uploadFreeQueue, &index, 0) != pdPASS) {
      stopUploadPipeline();
      return false;
    }
  }

  const BaseType_t created = xTaskCreatePinnedToCore(
      uploadWriterTaskThunk, "wifiSdWriter", kUploadWriterTaskStackBytes,
      this, kUploadWriterTaskPriority, &workspace_->uploadWriterTask,
      kUploadWriterTaskCore);
  if (created != pdPASS) {
    workspace_->uploadWriterTask = nullptr;
    stopUploadPipeline();
    return false;
  }

  workspace_->uploadPipelineAvailable = true;
  workspace_->uploadBufferCapacity = kUploadBufferBytes;
  Serial.printf(
      "WIFI XFER: upload pipeline ready buffers=%u bytes_each=%u dma=yes core=%d\n",
      static_cast<unsigned>(kUploadPipelineBufferCount),
      static_cast<unsigned>(kUploadBufferBytes),
      static_cast<int>(kUploadWriterTaskCore));
  return true;
}

void WifiTransferModeController::stopUploadPipeline() {
  if (!workspace_) return;

  if (workspace_->uploadWriterTask && workspace_->uploadWriterQueue) {
    UploadWriterCommand shutdown;
    shutdown.type = UploadWriterCommandType::Shutdown;
    xQueueSend(workspace_->uploadWriterQueue, &shutdown,
               pdMS_TO_TICKS(250U));
    const uint32_t started = millis();
    while (workspace_->uploadWriterTask &&
           elapsedSince(started, millis()) < 2000U) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (workspace_->uploadWriterTask) {
      vTaskDelete(workspace_->uploadWriterTask);
      workspace_->uploadWriterTask = nullptr;
    }
  }

  if (workspace_->uploadWriterQueue) {
    vQueueDelete(workspace_->uploadWriterQueue);
    workspace_->uploadWriterQueue = nullptr;
  }
  if (workspace_->uploadFreeQueue) {
    vQueueDelete(workspace_->uploadFreeQueue);
    workspace_->uploadFreeQueue = nullptr;
  }
  if (workspace_->uploadBarrier) {
    vSemaphoreDelete(workspace_->uploadBarrier);
    workspace_->uploadBarrier = nullptr;
  }
  for (size_t index = 0; index < kUploadPipelineBufferCount; ++index) {
    if (workspace_->uploadBuffers[index]) {
      heap_caps_free(workspace_->uploadBuffers[index]);
      workspace_->uploadBuffers[index] = nullptr;
    }
  }
  workspace_->uploadBuffer = nullptr;
  workspace_->uploadBufferCapacity = 0;
  workspace_->uploadBufferUsed = 0;
  workspace_->uploadBufferIndex = 0xFFU;
  workspace_->uploadPipelineAvailable = false;
}

bool WifiTransferModeController::allocateWorkspace() {
  if (workspace_) return true;
  void *memory = heap_caps_malloc(
      sizeof(Workspace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) {
    memory = heap_caps_malloc(sizeof(Workspace), MALLOC_CAP_8BIT);
  }
  if (!memory) return false;
  workspace_ = new (memory) Workspace();

  if (startUploadPipeline()) return true;

  // Keep the hardware-proven synchronous path as a low-memory fallback. The
  // first optimisation build must never make transfer entry depend on a second
  // internal DMA buffer or writer task being available.
  workspace_->uploadBuffers[0] = static_cast<uint8_t *>(heap_caps_malloc(
      kUploadBufferBytes,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!workspace_->uploadBuffers[0]) {
    workspace_->uploadBuffers[0] = static_cast<uint8_t *>(heap_caps_malloc(
        kUploadBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (!workspace_->uploadBuffers[0]) {
    workspace_->uploadBuffers[0] = static_cast<uint8_t *>(heap_caps_malloc(
        kUploadBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!workspace_->uploadBuffers[0]) {
    workspace_->~Workspace();
    heap_caps_free(workspace_);
    workspace_ = nullptr;
    return false;
  }
  workspace_->uploadBuffer = workspace_->uploadBuffers[0];
  workspace_->uploadBufferIndex = 0U;
  workspace_->uploadBufferCapacity = kUploadBufferBytes;
  Serial.println(
      "WIFI XFER: upload pipeline unavailable; using proven synchronous buffer");
  return true;
}

void WifiTransferModeController::releaseWorkspace() {
  Workspace *workspace = workspace_;
  if (!workspace) return;
  stopUploadPipeline();
  workspace_ = nullptr;

  // stopUploadPipeline() frees both DMA buffers when the pipeline was active.
  // The synchronous fallback owns only slot zero and has no queue/task.
  for (size_t index = 0; index < kUploadPipelineBufferCount; ++index) {
    if (workspace->uploadBuffers[index]) {
      heap_caps_free(workspace->uploadBuffers[index]);
      workspace->uploadBuffers[index] = nullptr;
    }
  }
  workspace->uploadBuffer = nullptr;
  workspace->uploadBufferCapacity = 0;
  workspace->uploadBufferUsed = 0;
  workspace->uploadBufferIndex = 0xFFU;
  workspace->~Workspace();
  heap_caps_free(workspace);
}

bool WifiTransferModeController::preflightStorage(
    MediaFsStorage &storage, const char *transferRoot,
    char *errorText, size_t errorCapacity) {
  if (errorText && errorCapacity) errorText[0] = '\0';

  Serial.println("WIFI XFER: preflight begin");
  Serial.printf(
      "WIFI XFER: memory heap_free=%lu psram_free=%lu\n",
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  if (active()) {
    copyText(errorText, errorCapacity, "transfer service already active");
    Serial.println("WIFI XFER: preflight failed service-active");
    return false;
  }
  if (!transferRoot || !transferRoot[0] ||
      strlen(transferRoot) > WifiTransferPolicy::kMaxPathBytes) {
    copyText(errorText, errorCapacity, "invalid transfer root");
    Serial.println("WIFI XFER: preflight failed invalid-root");
    return false;
  }
  if (!storage.mounted() ||
      storage.accessMode() != MediaFsAccessMode::TransferReadWrite) {
    copyText(errorText, errorCapacity, "SD is not mounted transfer read/write");
    Serial.println("WIFI XFER: preflight failed SD-not-transfer-rw");
    return false;
  }
  if (!allocateWorkspace()) {
    copyText(errorText, errorCapacity, "transfer workspace allocation failed");
    Serial.println("WIFI XFER: preflight failed workspace-allocation");
    return false;
  }

  static constexpr char rootProbeRelative[] = "probe.wav.uploading";
  WifiTransferPolicy::PathResult rootCheck =
      WifiTransferPolicy::joinCanonicalUnderRoot(
          transferRoot, strlen(transferRoot), rootProbeRelative,
          sizeof(rootProbeRelative) - 1U, workspace_->scratchPath,
          sizeof(workspace_->scratchPath));
  if (rootCheck.status != WifiTransferPolicy::PathStatus::Ok) {
    copyText(errorText, errorCapacity, "transfer root rejected");
    Serial.printf("WIFI XFER: preflight failed root-policy=%s\n",
                  WifiTransferPolicy::pathStatusText(rootCheck.status));
    releaseWorkspace();
    return false;
  }

  if (!copyText(workspace_->transferRoot,
                sizeof(workspace_->transferRoot), transferRoot)) {
    copyText(errorText, errorCapacity, "transfer root too long");
    Serial.println("WIFI XFER: preflight failed root-copy");
    releaseWorkspace();
    return false;
  }

  uint64_t freeBytes = 0;
  bool result = false;
  bool created = false;
  bool cleaned = false;
  char probeRelative[64] = {0};
  workspace_->preflightProbeFile.close();

  for (uint8_t attempt = 0; attempt < 4 && !created; ++attempt) {
    snprintf(probeRelative, sizeof(probeRelative),
             ".dspi-transfer-probe-%08" PRIX32 ".wav.uploading",
             static_cast<uint32_t>(esp_random()));
    WifiTransferPolicy::PathResult joined =
        WifiTransferPolicy::joinCanonicalUnderRoot(
            transferRoot, strlen(transferRoot), probeRelative,
            strlen(probeRelative), workspace_->scratchPath,
            sizeof(workspace_->scratchPath));
    if (joined.status != WifiTransferPolicy::PathStatus::Ok) break;

    SharedSpiGuard guard;
    if (storage.exists(workspace_->scratchPath)) continue;
    workspace_->preflightProbeFile =
        storage.createTransferFileExclusive(workspace_->scratchPath);
    created = static_cast<bool>(workspace_->preflightProbeFile);
  }

  if (!created) {
    copyText(errorText, errorCapacity,
             "could not exclusive-create SD write probe");
    Serial.println("WIFI XFER: preflight failed exclusive-create");
    storagePreflightValid_ = false;
    portENTER_CRITICAL(&stateMux_);
    storage_ = nullptr;
    portEXIT_CRITICAL(&stateMux_);
    releaseWorkspace();
    return false;
  }

  static const uint8_t probePayload[] = {
      'D', 'S', 'P', 'i', '-', 'v', '1', '3', '-', 'R', 'W', '-', 'P', 'R',
      'O', 'B', 'E', '\r', '\n'};
  bool writeOk = false;
  bool firstDeviceSyncOk = false;
  {
    SharedSpiGuard guard;
    writeOk =
        workspace_->preflightProbeFile.write(
            probePayload, sizeof(probePayload)) ==
            sizeof(probePayload) &&
        workspace_->preflightProbeFile.sync() &&
        !workspace_->preflightProbeFile.hadIoError();
    workspace_->preflightProbeFile.close();
    firstDeviceSyncOk = storage.syncTransferDevice();
    cleaned =
        storage.removeIncompleteTransferFile(workspace_->scratchPath);
    const bool finalDeviceSyncOk = storage.syncTransferDevice();
    result = writeOk && firstDeviceSyncOk && cleaned && finalDeviceSyncOk &&
             storage.freeSpaceBytes(freeBytes);
  }

  if (!result) {
    storagePreflightValid_ = false;
    copyText(errorText, errorCapacity,
             cleaned ? "SD write probe sync failed"
                     : "SD write probe cleanup failed; incomplete file remains");
    Serial.printf(
        "WIFI XFER: preflight failed write=%u sync=%u cleanup=%u\n",
        writeOk ? 1U : 0U, firstDeviceSyncOk ? 1U : 0U,
        cleaned ? 1U : 0U);
    portENTER_CRITICAL(&stateMux_);
    storage_ = nullptr;
    portEXIT_CRITICAL(&stateMux_);
    releaseWorkspace();
    return false;
  }

  uint64_t totalBytes = 0;
  {
    SharedSpiGuard guard;
    totalBytes = storage.cardSize();
  }
  portENTER_CRITICAL(&stateMux_);
  storage_ = &storage;
  freeBytes_ = freeBytes;
  totalBytes_ = totalBytes;
  storagePreflightValid_ = true;
  portEXIT_CRITICAL(&stateMux_);
  Serial.printf(
      "WIFI XFER: preflight complete fs=%s free=%" PRIu64
      " capacity=%" PRIu64 "\n",
      storage.fileSystemName(), freeBytes, totalBytes);
  return true;
}

bool WifiTransferModeController::start(MediaFsStorage &storage,
                                       const char *transferRoot) {
  if (active()) return false;

  // A previous failed/abandoned web update must not leak state into a new
  // transfer-mode session. This does not touch NVS or either valid app slot.
  resetFirmwareUpdateState();

  bool prepared = false;
  portENTER_CRITICAL(&stateMux_);
  prepared = storagePreflightValid_ && storage_ == &storage;
  portEXIT_CRITICAL(&stateMux_);
  prepared = prepared && workspace_ && transferRoot &&
             strcmp(workspace_->transferRoot, transferRoot) == 0 &&
             storage.mounted() &&
             storage.accessMode() == MediaFsAccessMode::TransferReadWrite;

  char error[96] = {0};
  if (!prepared &&
      !preflightStorage(storage, transferRoot, error, sizeof(error))) {
    setError(error[0] ? error : "storage preflight failed");
    return false;
  }

  portENTER_CRITICAL(&stateMux_);
  storagePreflightValid_ = false;
  state_ = WifiTransferServiceState::Starting;
  uploadPhase_ = WifiTransferPolicy::UploadPhase::Idle;
  accepting_ = false;
  handlingRequest_ = false;
  finishRequested_ = false;
  autoExitRequested_ = false;
  abortWriterRequested_ = false;
  filesystemSyncRequested_ = false;
  filesystemSyncComplete_ = false;
  filesystemSyncOk_ = false;
  stopNetworkRequested_ = false;
  closeHttpRequested_ = false;
  httpListenerClosed_ = true;
  networkStopped_ = false;
  networkStopStage_ = NetworkStopStage::Idle;
  writtenBytes_ = 0;
  declaredBytes_ = 0;
  uploadStartedMs_ = 0;
  uploadElapsedMs_ = 0;
  syncCount_ = 0;
  lastActivityMs_ = millis();
  ssid_[0] = '\0';
  password_[0] = '\0';
  numericIp_[0] = '\0';
  currentFile_[0] = '\0';
  copyText(message_, sizeof(message_), "Starting Wi-Fi transfer");
  portEXIT_CRITICAL(&stateMux_);

  BaseType_t created = xTaskCreate(
      transferTaskThunk, "wifiTransferTask", kTransferTaskStackBytes, this,
      kTransferTaskPriority, &taskHandle_);
  if (created != pdPASS) {
    releaseWorkspace();
    portENTER_CRITICAL(&stateMux_);
    taskHandle_ = nullptr;
    storage_ = nullptr;
    networkStopped_ = true;
    closeHttpRequested_ = false;
    httpListenerClosed_ = true;
    state_ = WifiTransferServiceState::Error;
    copyText(message_, sizeof(message_), "transfer task allocation failed");
    portEXIT_CRITICAL(&stateMux_);
    Serial.println("WIFI XFER: start failed task-allocation");
    return false;
  }
  return true;
}

void WifiTransferModeController::transferTaskThunk(void *context) {
  WifiTransferModeController *controller =
      static_cast<WifiTransferModeController *>(context);
  controller->transferTask();
}

void WifiTransferModeController::uploadWriterTaskThunk(void *context) {
  WifiTransferModeController *controller =
      static_cast<WifiTransferModeController *>(context);
  controller->uploadWriterTask();
}

void WifiTransferModeController::uploadWriterTask() {
  for (;;) {
    Workspace *workspace = workspace_;
    if (!workspace || !workspace->uploadWriterQueue) break;

    UploadWriterCommand command;
    if (xQueueReceive(workspace->uploadWriterQueue, &command,
                      portMAX_DELAY) != pdPASS) {
      continue;
    }

    if (command.type == UploadWriterCommandType::Shutdown) break;

    if (command.type == UploadWriterCommandType::Write) {
      bool ok = !workspace->uploadWriterFailed;
      if (ok && command.bufferIndex < kUploadPipelineBufferCount &&
          workspace->uploadBuffers[command.bufferIndex] &&
          command.length > 0U) {
        ok = writeUploadBlock(
            workspace->uploadBuffers[command.bufferIndex],
            command.length, false);
      } else if (command.bufferIndex >= kUploadPipelineBufferCount ||
                 !workspace->uploadBuffers[command.bufferIndex] ||
                 command.length == 0U) {
        ok = false;
      }
      if (!ok) workspace->uploadWriterFailed = true;
      xQueueSend(workspace->uploadFreeQueue, &command.bufferIndex,
                 portMAX_DELAY);
      continue;
    }

    if (command.type == UploadWriterCommandType::Barrier) {
      bool ok = !workspace->uploadWriterFailed;
      if (ok && command.syncAfterWrite) {
        ok = writeUploadBlock(nullptr, 0U, true);
      }
      workspace->uploadBarrierOk = ok;
      if (!ok) workspace->uploadWriterFailed = true;
      xSemaphoreGive(workspace->uploadBarrier);
    }
  }

  if (workspace_) workspace_->uploadWriterTask = nullptr;
  vTaskDelete(nullptr);
}

void WifiTransferModeController::transferTask() {
  if (!startSoftApAndServer()) {
    // startSoftApAndServer() may fail after AP mode has already been enabled.
    // Drive the same staged shutdown used by Finish Safely rather than
    // deleting a live server/network object from this error path.
    for (uint8_t step = 0;
         step < 16U && !stopHttpAndWifi(); ++step) {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    releaseWorkspace();
    portENTER_CRITICAL(&stateMux_);
    storage_ = nullptr;
    ssid_[0] = '\0';
    password_[0] = '\0';
    numericIp_[0] = '\0';
    networkStopped_ = true;
    closeHttpRequested_ = false;
    httpListenerClosed_ = true;
    taskHandle_ = nullptr;
    state_ = WifiTransferServiceState::Error;
    portEXIT_CRITICAL(&stateMux_);
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    uint32_t firmwareRebootAt = 0;
    portENTER_CRITICAL(&stateMux_);
    firmwareRebootAt = firmwareRebootAt_;
    portEXIT_CRITICAL(&stateMux_);
    if (firmwareRebootAt &&
        static_cast<int32_t>(millis() - firmwareRebootAt) >= 0) {
      Serial.println("WIFI OTA: response drained; restarting into update");
      delay(20);
      ESP.restart();
    }

    serviceStationConnection();
    serviceControlRequests();

    bool stopRequested = false;
    bool networkStopped = false;
    portENTER_CRITICAL(&stateMux_);
    stopRequested = stopNetworkRequested_;
    networkStopped = networkStopped_;
    portEXIT_CRITICAL(&stateMux_);
    if (stopRequested) {
      if (stopHttpAndWifi()) {
        // The task owns Workspace from xTaskCreate until this point. Release
        // it before publishing taskHandle_ == nullptr/networkStopped_ so the
        // main-loop rollback path cannot race discardPreparedState() and
        // release the same allocation concurrently.
        releaseWorkspace();
        portENTER_CRITICAL(&stateMux_);
        networkStopped_ = true;
        closeHttpRequested_ = false;
        httpListenerClosed_ = true;
        taskHandle_ = nullptr;
        state_ = WifiTransferServiceState::Stopped;
        storage_ = nullptr;
        ssid_[0] = '\0';
        password_[0] = '\0';
        numericIp_[0] = '\0';
        copyText(message_, sizeof(message_), "Wi-Fi transfer stopped");
        portEXIT_CRITICAL(&stateMux_);
        Serial.printf(
            "WIFI XFER: network stopped heap=%lu psram=%lu stack_min=%lu\n",
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned long>(
                uxTaskGetStackHighWaterMark(nullptr)));
        vTaskDelete(nullptr);
        return;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (networkStopped) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    bool listenerClosed = true;
    portENTER_CRITICAL(&stateMux_);
    listenerClosed = httpListenerClosed_;
    portEXIT_CRITICAL(&stateMux_);
    if (!listenerClosed && workspace_ && server_) {
      server_->handleClient();
    }

    // WebServer presents a large raw body as roughly 1436-byte callbacks.
    // Sleeping a full RTOS tick after every callback accounted for tens of
    // seconds of avoidable delay.  While a writer is active the raw callback
    // itself gives lower-priority loopTask one tick every 256 KiB; between
    // uploads retain the normal one-tick idle delay.
    bool writerActive = false;
    portENTER_CRITICAL(&stateMux_);
    writerActive = WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
                   firmwareUpdateActive_;
    portEXIT_CRITICAL(&stateMux_);
    if (writerActive) {
      taskYIELD();
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

bool WifiTransferModeController::requestStartupAbort() {
  bool allowed = false;
  portENTER_CRITICAL(&stateMux_);
  const bool writerActive =
      WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
      firmwareUpdateActive_;
  allowed = taskHandle_ != nullptr && !writerActive && !handlingRequest_ &&
            (state_ == WifiTransferServiceState::Starting ||
             state_ == WifiTransferServiceState::Serving ||
             state_ == WifiTransferServiceState::Quiescing);
  if (allowed) {
    // Startup cancellation must follow the same storage-first shutdown as a
    // browser Finish request.  Never arm radio teardown while the transfer
    // task may still own a read/write mounted filesystem.
    accepting_ = false;
    finishRequested_ = true;
    autoExitRequested_ = false;
    closeHttpRequested_ = true;
    stationConnectPending_ = false;
    state_ = WifiTransferServiceState::Quiescing;
    copyText(message_, sizeof(message_),
             "Cancelling Wi-Fi transfer safely");
  }
  portEXIT_CRITICAL(&stateMux_);
  if (allowed) {
    Serial.println("WIFI XFER: startup abort routed through safe exit");
  }
  return allowed;
}

bool WifiTransferModeController::loadSavedStationCredentials() {
  savedStationSsid_[0] = '\0';
  savedStationPassword_[0] = '\0';

  Preferences preferences;
  if (!preferences.begin(kWifiPreferencesNamespace, true)) return false;
  const String ssid = preferences.getString(kWifiSsidKey, "");
  const String password = preferences.getString(kWifiPasswordKey, "");
  preferences.end();

  if (ssid.length() == 0 || ssid.length() > 32U ||
      password.length() > 63U) {
    return false;
  }
  copyText(savedStationSsid_, sizeof(savedStationSsid_), ssid.c_str());
  copyText(savedStationPassword_, sizeof(savedStationPassword_),
           password.c_str());
  return true;
}

bool WifiTransferModeController::saveStationCredentials(
    const char *ssid, const char *password) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32U ||
      !password || strlen(password) > 63U) {
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kWifiPreferencesNamespace, false)) return false;
  const size_t ssidWritten = preferences.putString(kWifiSsidKey, ssid);
  // putString() returns zero for both an intentionally empty string and an
  // error, so verify both values by reading them back before reporting success.
  preferences.putString(kWifiPasswordKey, password);
  const String verifiedSsid = preferences.getString(kWifiSsidKey, "");
  const String verifiedPassword = preferences.getString(kWifiPasswordKey, "");
  preferences.end();
  const bool ok = ssidWritten == strlen(ssid) &&
                  verifiedSsid == ssid && verifiedPassword == password;
  if (!ok) return false;

  copyText(savedStationSsid_, sizeof(savedStationSsid_), ssid);
  copyText(savedStationPassword_, sizeof(savedStationPassword_), password);
  return true;
}

bool WifiTransferModeController::beginStationConnection() {
  if (!savedStationSsid_[0]) return false;

  stopMdns();
  WiFi.disconnect(false, false);
  stationConnected_ = false;
  stationIp_[0] = '\0';
  stationConnectStartedMs_ = millis();
  stationConnectPending_ = true;
  WiFi.begin(savedStationSsid_, savedStationPassword_);
  Serial.printf("WIFI XFER: joining saved network ssid=%s\n",
                savedStationSsid_);
  return true;
}

void WifiTransferModeController::stopMdns() {
  if (!mdnsStarted_) return;
  MDNS.end();
  mdnsStarted_ = false;
}

void WifiTransferModeController::serviceStationConnection() {
  bool closing = false;
  portENTER_CRITICAL(&stateMux_);
  closing = closeHttpRequested_ || stopNetworkRequested_ ||
            state_ == WifiTransferServiceState::Quiescing ||
            state_ == WifiTransferServiceState::StoppingNetwork;
  if (closing) stationConnectPending_ = false;
  portEXIT_CRITICAL(&stateMux_);
  if (closing) return;

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    const String localIp = WiFi.localIP().toString();
    if (!stationConnected_ || strcmp(stationIp_, localIp.c_str()) != 0) {
      stationConnected_ = true;
      stationConnectPending_ = false;
      copyText(stationIp_, sizeof(stationIp_), localIp.c_str());
      char readyMessage[sizeof(message_)] = {0};
      snprintf(readyMessage, sizeof(readyMessage),
               "LAN %s; direct AP remains ready", stationIp_);
      portENTER_CRITICAL(&stateMux_);
      // Keep the screen's primary address at 192.168.4.1. It always belongs
      // to the direct AP; the LAN address is shown in status and on the page.
      if (!WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
          !firmwareUpdateActive_) {
        copyText(message_, sizeof(message_), readyMessage);
      }
      portEXIT_CRITICAL(&stateMux_);
      if (!mdnsStarted_ && MDNS.begin(kMdnsHost)) {
        MDNS.addService("http", "tcp", kHttpPort);
        mdnsStarted_ = true;
      }
      Serial.printf("WIFI XFER: home network connected ssid=%s ip=%s mdns=%s\n",
                    savedStationSsid_, stationIp_,
                    mdnsStarted_ ? "dspi-transfer.local" : "unavailable");
    }
    return;
  }

  if (stationConnected_) {
    stationConnected_ = false;
    stationIp_[0] = '\0';
    stopMdns();
    portENTER_CRITICAL(&stateMux_);
    if (!WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
        !firmwareUpdateActive_) {
      copyText(message_, sizeof(message_),
               "Home Wi-Fi lost; direct access point remains ready");
    }
    portEXIT_CRITICAL(&stateMux_);
  }

  if (stationConnectPending_ &&
      elapsedSince(stationConnectStartedMs_, millis()) >=
          kStationConnectTimeoutMs) {
    stationConnectPending_ = false;
    WiFi.disconnect(false, false);
    portENTER_CRITICAL(&stateMux_);
    if (!WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
        !firmwareUpdateActive_) {
      copyText(message_, sizeof(message_),
               "Direct access point ready; saved Wi-Fi unavailable");
    }
    portEXIT_CRITICAL(&stateMux_);
    Serial.printf("WIFI XFER: saved network timeout ssid=%s\n",
                  savedStationSsid_);
  }
}

bool WifiTransferModeController::startSoftApAndServer() {
  if (!workspace_ || !storage_ ||
      storage_->accessMode() != MediaFsAccessMode::TransferReadWrite) {
    setError("SD transfer mount was lost");
    Serial.println("WIFI XFER: AP start failed SD-not-transfer-rw");
    return false;
  }

  const uint64_t deviceId = ESP.getEfuseMac();
  char localSsid[sizeof(ssid_)] = {0};
  snprintf(localSsid, sizeof(localSsid), "DSPi-Transfer-%04" PRIX32,
           static_cast<uint32_t>(deviceId & 0xFFFFU));

  char localPassword[sizeof(password_)] = {0};
  copyText(localPassword, sizeof(localPassword), kDirectApPassword);

  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP_STA)) {
    setError("could not initialize SoftAP mode");
    Serial.println("WIFI XFER: AP start failed mode");
    return false;
  }
  if (!WiFi.setSleep(false)) {
    // This is a throughput optimization, not a reason to strand an otherwise
    // usable transfer session after BLE and normal SD users have stopped.
    Serial.println("WIFI XFER: warning power-save disable failed");
  }
  if (!WiFi.softAP(localSsid, localPassword, 1, false, 2, false,
                   WIFI_AUTH_WPA2_PSK)) {
    setError("could not start SoftAP");
    Serial.println("WIFI XFER: AP start failed softAP");
    return false;
  }

  const String ip = WiFi.softAPIP().toString();
  if (!server_) {
    server_ = new (std::nothrow) WebServer(kHttpPort);
    if (!server_) {
      setError("HTTP server allocation failed");
      Serial.println("WIFI XFER: web start failed allocation");
      return false;
    }
  }
  if (!routesConfigured_) {
    configureRoutes();
    routesConfigured_ = true;
  }
  portENTER_CRITICAL(&stateMux_);
  networkStopStage_ = NetworkStopStage::Idle;
  portEXIT_CRITICAL(&stateMux_);
  server_->begin();

  stationConnectPending_ = false;
  stationConnected_ = false;
  stationIp_[0] = '\0';
  stopMdns();
  const bool haveSavedStation = loadSavedStationCredentials();

  portENTER_CRITICAL(&stateMux_);
  copyText(ssid_, sizeof(ssid_), localSsid);
  copyText(password_, sizeof(password_), localPassword);
  copyText(numericIp_, sizeof(numericIp_), ip.c_str());
  state_ = WifiTransferServiceState::Serving;
  accepting_ = true;
  closeHttpRequested_ = false;
  httpListenerClosed_ = false;
  networkStopped_ = false;
  lastActivityMs_ = millis();
  copyText(message_, sizeof(message_),
           haveSavedStation ? "Joining saved Wi-Fi; direct AP is ready"
                            : "Ready for browser connection");
  portEXIT_CRITICAL(&stateMux_);

  if (haveSavedStation) beginStationConnection();

  // Deliberately never print localPassword/password_.
  Serial.printf("WIFI XFER: AP started ssid=%s ip=%s\n",
                localSsid, ip.c_str());
  Serial.printf("WIFI XFER: memory heap_free=%lu psram_free=%lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(
                    heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  Serial.println("WIFI XFER: server started port=80");
  return true;
}

void WifiTransferModeController::configureRoutes() {
  WebServer *server = server_;
  if (!server) return;

  const char *headers[] = {kHeaderBase, kHeaderPath, kHeaderDeclaredSize,
                           kHeaderDeleteConfirm, kHeaderDeleteKind};
  server->collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));

  auto wrapped = [this](void (WifiTransferModeController::*handler)()) {
    setHandlingRequest(true);
    noteWebActivity();
    (this->*handler)();
    setHandlingRequest(false);
  };

  server->on(WifiTransferWeb::kRouteIndex, HTTP_GET,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handleIndex); });
  server->on(WifiTransferWeb::kRouteStatus, HTTP_GET,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handleStatus); });
  server->on(WifiTransferWeb::kRouteSpdifDiagnostics, HTTP_GET,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleSpdifDiagnostics);
             });
  server->on(WifiTransferWeb::kRouteNetwork, HTTP_GET,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleNetworkStatus);
             });
  server->on(WifiTransferWeb::kRouteNetwork, HTTP_POST,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleNetworkSave);
             });
  server->on(WifiTransferWeb::kRouteNetworkScan, HTTP_GET,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleNetworkScan);
             });
  server->on(WifiTransferWeb::kRouteList, HTTP_GET,
             [this]() {
               setHandlingRequest(true);
               noteWebActivity();
               handleList(false);
               setHandlingRequest(false);
             });
  server->on(WifiTransferWeb::kRouteIncomplete, HTTP_GET,
             [this]() {
               setHandlingRequest(true);
               noteWebActivity();
               handleList(true);
               setHandlingRequest(false);
             });
  server->on(WifiTransferWeb::kRouteMkdir, HTTP_POST,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handleMkdir); });
  server->on(WifiTransferWeb::kRoutePreflight, HTTP_POST,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handlePreflight); });
  server->on(
      WifiTransferWeb::kRouteUpload, HTTP_PUT,
      [this, wrapped]() {
        wrapped(&WifiTransferModeController::handleUploadFinished);
      },
      [this, wrapped]() {
        wrapped(&WifiTransferModeController::handleUploadRaw);
      });
  server->on(
      WifiTransferWeb::kRouteFirmware, HTTP_PUT,
      [this, wrapped]() {
        wrapped(&WifiTransferModeController::handleFirmwareFinished);
      },
      [this, wrapped]() {
        wrapped(&WifiTransferModeController::handleFirmwareRaw);
      });
  server->on(WifiTransferWeb::kRouteCancel, HTTP_POST,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handleCancel); });
  server->on(WifiTransferWeb::kRouteIncomplete, HTTP_DELETE,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleDeleteIncomplete);
             });
  server->on(WifiTransferWeb::kRouteFolder, HTTP_DELETE,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleDeleteFolder);
             });
  server->on(WifiTransferWeb::kRouteDelete, HTTP_POST,
             [this, wrapped]() {
               wrapped(&WifiTransferModeController::handleDeleteEntry);
             });
  server->on(WifiTransferWeb::kRouteFinish, HTTP_POST,
             [this, wrapped]() { wrapped(&WifiTransferModeController::handleFinish); });
  server->onNotFound(
      [this, wrapped]() { wrapped(&WifiTransferModeController::handleNotFound); });
}

void WifiTransferModeController::serviceControlRequests() {
  bool syncRequested = false;
  bool abortRequested = false;
  bool writerActive = false;
  bool accepting = false;
  bool handling = false;
  bool closeHttpRequested = false;
  bool httpListenerClosed = true;
  uint32_t lastActivity = 0;

  portENTER_CRITICAL(&stateMux_);
  syncRequested =
      filesystemSyncRequested_ && !filesystemSyncComplete_;
  abortRequested = abortWriterRequested_;
  writerActive =
      WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
      firmwareUpdateActive_;
  accepting = accepting_;
  handling = handlingRequest_;
  closeHttpRequested = closeHttpRequested_;
  httpListenerClosed = httpListenerClosed_;
  lastActivity = lastActivityMs_;
  portEXIT_CRITICAL(&stateMux_);

  if (abortRequested && writerActive) {
    // The raw callback observes the same flag between bounded core chunks.
    return;
  }
  if (abortRequested && !writerActive) {
    portENTER_CRITICAL(&stateMux_);
    abortWriterRequested_ = false;
    portEXIT_CRITICAL(&stateMux_);
  }

  if (closeHttpRequested && !httpListenerClosed && !writerActive && !handling) {
    closeHttpListenerIfRequested();
    portENTER_CRITICAL(&stateMux_);
    httpListenerClosed = httpListenerClosed_;
    portEXIT_CRITICAL(&stateMux_);
  }

  if (syncRequested && httpListenerClosed && !writerActive && !handling &&
      !accepting && storage_ &&
      storage_->accessMode() == MediaFsAccessMode::TransferReadWrite) {
    bool ok = false;
    {
      SharedSpiGuard guard;
      ok = storage_->syncTransferDevice();
    }
    portENTER_CRITICAL(&stateMux_);
    filesystemSyncOk_ = ok;
    filesystemSyncComplete_ = true;
    copyText(message_, sizeof(message_),
             ok ? "Transfer filesystem synced"
                : "Transfer filesystem sync failed");
    portEXIT_CRITICAL(&stateMux_);
    Serial.printf("WIFI XFER: filesystem sync result=%s\n",
                  ok ? "OK" : "FAIL");
  }

  const uint32_t now = millis();
  if (accepting && !writerActive && !handling &&
      elapsedSince(lastActivity, now) >= WIFI_TRANSFER_AUTO_EXIT_IDLE_MS) {
    portENTER_CRITICAL(&stateMux_);
    if (accepting_ &&
        !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
        !firmwareUpdateActive_ &&
        !handlingRequest_) {
      accepting_ = false;
      finishRequested_ = true;
      autoExitRequested_ = true;
      closeHttpRequested_ = true;
      state_ = WifiTransferServiceState::Quiescing;
      copyText(message_, sizeof(message_),
               "Idle timeout; finishing transfer safely");
    }
    portEXIT_CRITICAL(&stateMux_);
    Serial.printf("WIFI XFER: idle finish requested idle_ms=%lu\n",
                  static_cast<unsigned long>(
                      WIFI_TRANSFER_AUTO_EXIT_IDLE_MS));
  }
}

void WifiTransferModeController::closeHttpListenerIfRequested() {
  bool shouldClose = false;
  portENTER_CRITICAL(&stateMux_);
  shouldClose = closeHttpRequested_ && !httpListenerClosed_ &&
                !handlingRequest_ &&
                !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
                !firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);
  if (!shouldClose || !workspace_ || !server_) return;

  // Run only on the transfer task, never inside a request callback. This lets
  // the Finish response leave first, then closes the sole WebServer listener
  // so browser polling cannot keep the safe-exit state machine alive.
  server_->client().stop();
  server_->close();
  portENTER_CRITICAL(&stateMux_);
  httpListenerClosed_ = true;
  copyText(message_, sizeof(message_),
           "Browser closed; finishing transfer safely");
  portEXIT_CRITICAL(&stateMux_);
  Serial.println("WIFI XFER: HTTP listener closed for safe exit");
}

bool WifiTransferModeController::stopHttpAndWifi() {
  NetworkStopStage stage = NetworkStopStage::Idle;
  portENTER_CRITICAL(&stateMux_);
  stage = networkStopStage_;
  portEXIT_CRITICAL(&stateMux_);

  const auto advanceStage = [this](NetworkStopStage expected,
                                   NetworkStopStage next) {
    portENTER_CRITICAL(&stateMux_);
    if (networkStopStage_ == expected) networkStopStage_ = next;
    portEXIT_CRITICAL(&stateMux_);
  };

  switch (stage) {
    case NetworkStopStage::Idle:
    case NetworkStopStage::CloseHttp: {
      bool listenerAlreadyClosed = true;
      portENTER_CRITICAL(&stateMux_);
      listenerAlreadyClosed = httpListenerClosed_;
      portEXIT_CRITICAL(&stateMux_);
      Serial.printf("WIFI XFER: network stop stage=http begin already_closed=%s\n",
                    listenerAlreadyClosed ? "yes" : "no");
      // Normal Finish Safely closed the listener after its response drained.
      // Do not repeat client().stop()/close(): the hardware trace ended inside
      // that duplicate teardown. Startup rollback may reach this stage before
      // quiesce, in which case one listener close is still required.
      if (!listenerAlreadyClosed && server_) {
        server_->close();
      }
      portENTER_CRITICAL(&stateMux_);
      closeHttpRequested_ = false;
      httpListenerClosed_ = true;
      portEXIT_CRITICAL(&stateMux_);
      // Keep WebServer and its captured route handlers allocated for the
      // firmware lifetime. Reopen the same listener on the next session.
      Serial.println("WIFI XFER: HTTP stopped");
      advanceStage(stage, NetworkStopStage::StopMdns);
      return false;
    }

    case NetworkStopStage::StopMdns:
      Serial.println("WIFI XFER: network stop stage=mdns begin");
      stopMdns();
      Serial.println("WIFI XFER: mDNS stopped");
      advanceStage(stage, NetworkStopStage::DisableWifi);
      return false;

    case NetworkStopStage::DisableWifi: {
      Serial.println("WIFI XFER: network stop stage=radio begin");
      stationConnectPending_ = false;
      stationConnected_ = false;
      stationIp_[0] = '\0';
      WiFi.scanDelete();

      // One Arduino-core mode transition owns the complete AP+STA shutdown.
      // The v13.1 trace stopped after the transfer filesystem had been safely
      // released but before any network-stop completion telemetry.  Avoid the
      // former disconnect(STA) + softAPdisconnect() + mode(NULL) teardown
      // chain, which asks the same core to dismantle the interfaces repeatedly.
      if (!WiFi.mode(WIFI_MODE_NULL)) {
        setMessage("Waiting for Wi-Fi shutdown");
        return false;
      }
      Serial.println("WIFI XFER: WiFi stopped");
      memset(savedStationSsid_, 0, sizeof(savedStationSsid_));
      memset(savedStationPassword_, 0, sizeof(savedStationPassword_));
      advanceStage(stage, NetworkStopStage::Complete);
      return false;
    }

    case NetworkStopStage::Complete:
      advanceStage(stage, NetworkStopStage::Idle);
      return true;
  }
  return false;
}

void WifiTransferModeController::setHandlingRequest(bool activeRequest) {
  portENTER_CRITICAL(&stateMux_);
  handlingRequest_ = activeRequest;
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::noteWebActivity() {
  portENTER_CRITICAL(&stateMux_);
  lastActivityMs_ = millis();
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::setMessage(const char *message) {
  portENTER_CRITICAL(&stateMux_);
  copyText(message_, sizeof(message_), message ? message : "");
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::setError(const char *message) {
  portENTER_CRITICAL(&stateMux_);
  state_ = WifiTransferServiceState::Error;
  accepting_ = false;
  copyText(message_, sizeof(message_), message ? message : "transfer error");
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::updateUploadSnapshot() {
  if (!workspace_) return;
  portENTER_CRITICAL(&stateMux_);
  writtenBytes_ = workspace_->writerReceivedBytes;
  declaredBytes_ = workspace_->writerDeclaredBytes;
  uploadStartedMs_ = workspace_->writerStartedMs;
  uploadElapsedMs_ =
      workspace_->writerStartedMs
          ? elapsedSince(workspace_->writerStartedMs, millis())
          : 0;
  syncCount_ = workspace_->writerSyncCount;
  const char *leaf = strrchr(workspace_->combinedFinal, '/');
  copyDisplayText(currentFile_, sizeof(currentFile_),
                  leaf ? leaf + 1 : workspace_->combinedFinal);
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::updateStorageSnapshot() {
  if (!storage_ ||
      storage_->accessMode() != MediaFsAccessMode::TransferReadWrite) {
    return;
  }
  uint64_t freeBytes = 0;
  uint64_t totalBytes = 0;
  {
    SharedSpiGuard guard;
    if (!storage_->freeSpaceBytes(freeBytes)) return;
    totalBytes = storage_->cardSize();
  }
  portENTER_CRITICAL(&stateMux_);
  freeBytes_ = freeBytes;
  totalBytes_ = totalBytes;
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::handleIndex() {
  if (!workspace_ || !server_) return;
  server_->sendHeader("Cache-Control", "no-store");
  server_->send_P(
      200, PSTR("text/html; charset=utf-8"),
      WifiTransferWeb::kIndexHtml);
}

void WifiTransferModeController::beginChunkedJson(int code) {
  if (!workspace_ || !server_) return;
  server_->sendHeader("Cache-Control", "no-store");
  server_->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server_->send(code, "application/json; charset=utf-8", "");
}

void WifiTransferModeController::sendJsonString(const char *text) {
  if (!workspace_ || !server_) return;
  WebServer &server = *server_;
  size_t used = 0;
  auto flush = [&]() {
    if (used) {
      server.sendContent(workspace_->jsonChunk, used);
      used = 0;
    }
  };
  auto append = [&](char value) {
    if (used == sizeof(workspace_->jsonChunk)) flush();
    workspace_->jsonChunk[used++] = value;
  };
  auto appendText = [&](const char *value) {
    while (value && *value) append(*value++);
  };

  append('"');
  if (text) {
    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(text);
    static constexpr char hex[] = "0123456789ABCDEF";
    while (*cursor) {
      const uint8_t value = *cursor++;
      switch (value) {
        case '"': appendText("\\\""); break;
        case '\\': appendText("\\\\"); break;
        case '\b': appendText("\\b"); break;
        case '\f': appendText("\\f"); break;
        case '\n': appendText("\\n"); break;
        case '\r': appendText("\\r"); break;
        case '\t': appendText("\\t"); break;
        default:
          if (value < 0x20U) {
            appendText("\\u00");
            append(hex[(value >> 4U) & 0x0FU]);
            append(hex[value & 0x0FU]);
          } else {
            append(static_cast<char>(value));
          }
          break;
      }
    }
  }
  append('"');
  flush();
}

void WifiTransferModeController::endChunkedJson() {
  if (workspace_ && server_) {
    server_->sendContent("");
  }
}

void WifiTransferModeController::sendJsonError(
    int code, const char *reason, const char *message) {
  beginChunkedJson(code);
  server_->sendContent("{\"ok\":false,\"error\":");
  sendJsonString(reason ? reason : "transfer_error");
  server_->sendContent(",\"message\":");
  sendJsonString(message ? message : "Transfer request failed.");
  server_->sendContent("}");
  endChunkedJson();
}

void WifiTransferModeController::sendJsonOk(const char *extraFields) {
  beginChunkedJson();
  server_->sendContent("{\"ok\":true");
  if (extraFields && extraFields[0]) {
    server_->sendContent(",");
    server_->sendContent(extraFields);
  }
  server_->sendContent("}");
  endChunkedJson();
}

const char *WifiTransferModeController::stateText(
    WifiTransferServiceState state) {
  switch (state) {
    case WifiTransferServiceState::Stopped: return "STOPPED";
    case WifiTransferServiceState::Starting: return "STARTING";
    case WifiTransferServiceState::Serving: return "SERVING";
    case WifiTransferServiceState::Quiescing: return "QUIESCING";
    case WifiTransferServiceState::StoppingNetwork: return "STOPPING_NETWORK";
    case WifiTransferServiceState::Error: return "ERROR";
    default: return "UNKNOWN";
  }
}

void WifiTransferModeController::handleStatus() {
  WifiTransferSnapshot current;
  snapshot(current);
  beginChunkedJson();
  WebServer &server = *server_;
  char number[48] = {0};
  server.sendContent("{\"ok\":true,\"mode\":");
  sendJsonString(stateText(current.state));
  server.sendContent(",\"accepting\":");
  server.sendContent(current.accepting ? "true" : "false");
  server.sendContent(",\"writerActive\":");
  server.sendContent(current.writerActive ? "true" : "false");
  server.sendContent(",\"finishRequested\":");
  server.sendContent(current.finishRequested ? "true" : "false");
  server.sendContent(",\"currentFile\":");
  sendJsonString(current.currentFile);
  snprintf(number, sizeof(number), ",\"writtenBytes\":\"%" PRIu64 "\"",
           current.writtenBytes);
  server.sendContent(number);
  snprintf(number, sizeof(number), ",\"declaredBytes\":\"%" PRIu64 "\"",
           current.declaredBytes);
  server.sendContent(number);
  snprintf(number, sizeof(number), ",\"freeBytes\":\"%" PRIu64 "\"",
           current.freeBytes);
  server.sendContent(number);
  snprintf(number, sizeof(number), ",\"totalBytes\":\"%" PRIu64 "\"",
           current.totalBytes);
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"maxFileBytes\":\"%" PRIu64 "\",\"elapsedMs\":%lu",
           WIFI_TRANSFER_MAX_FILE_BYTES,
           static_cast<unsigned long>(current.elapsedMs));
  server.sendContent(number);
  server.sendContent(",\"message\":");
  sendJsonString(current.message);
  server.sendContent("}");
  endChunkedJson();
}

void WifiTransferModeController::handleSpdifDiagnostics() {
  if (!server_) return;
  WebServer &server = *server_;
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");

  const size_t count = spdifDiagnosticCount();
  if (count == 0) {
    server.sendContent("No S/PDIF diagnostic events recorded since boot.\n");
  } else {
    char line[176] = {0};
    for (size_t index = 0; index < count; ++index) {
      SpdifDiagnosticEntry entry;
      if (!spdifDiagnosticRead(index, entry)) continue;
      snprintf(line, sizeof(line), "#%lu  %lu.%03lu s  %s\n",
               (unsigned long)entry.sequence,
               (unsigned long)(entry.uptimeMs / 1000u),
               (unsigned long)(entry.uptimeMs % 1000u), entry.message);
      server.sendContent(line);
    }
  }
  server.sendContent("");
}

bool WifiTransferModeController::parseUnsignedDecimal(
    const String &text, uint64_t maximum, uint64_t &value) {
  value = 0;
  if (text.length() == 0) return false;
  for (size_t index = 0; index < text.length(); ++index) {
    const char digit = text[index];
    if (digit < '0' || digit > '9') return false;
    const uint8_t numeric = static_cast<uint8_t>(digit - '0');
    if (value > (maximum - numeric) / 10U) return false;
    value = value * 10U + numeric;
  }
  return true;
}

bool WifiTransferModeController::makeAbsoluteDirectoryFromQuery(
    const String &path, size_t &canonicalLength) {
  canonicalLength = 0;
  if (!workspace_) return false;
  if (path.length() == 0) {
    workspace_->canonicalBase[0] = '\0';
    return copyText(workspace_->absoluteDirectory,
                    sizeof(workspace_->absoluteDirectory),
                    workspace_->transferRoot);
  }
  WifiTransferPolicy::PathResult checked =
      WifiTransferPolicy::validateExistingRelativePath(
          path.c_str(), path.length());
  if (checked.status != WifiTransferPolicy::PathStatus::Ok ||
      !copyText(workspace_->canonicalBase,
                sizeof(workspace_->canonicalBase), path.c_str())) {
    return false;
  }
  canonicalLength = checked.length;
  return makeAbsoluteExisting(workspace_->canonicalBase, canonicalLength,
                              workspace_->absoluteDirectory,
                              sizeof(workspace_->absoluteDirectory));
}

bool WifiTransferModeController::makeAbsolute(
    const char *relative, size_t relativeLength,
    char *output, size_t outputCapacity) {
  if (!workspace_ || !relative || relativeLength == 0) return false;
  WifiTransferPolicy::PathResult joined =
      WifiTransferPolicy::joinCanonicalUnderRoot(
          workspace_->transferRoot, strlen(workspace_->transferRoot),
          relative, relativeLength, output, outputCapacity);
  return joined.status == WifiTransferPolicy::PathStatus::Ok;
}

bool WifiTransferModeController::makeAbsoluteExisting(
    const char *relative, size_t relativeLength,
    char *output, size_t outputCapacity) {
  if (!workspace_ || !relative || relativeLength == 0) return false;
  WifiTransferPolicy::PathResult joined =
      WifiTransferPolicy::joinExistingUnderRoot(
          workspace_->transferRoot, strlen(workspace_->transferRoot),
          relative, relativeLength, output, outputCapacity);
  return joined.status == WifiTransferPolicy::PathStatus::Ok;
}

bool WifiTransferModeController::combineRelative(
    const char *base, size_t baseLength, const char *child,
    size_t childLength, char *output, size_t outputCapacity,
    size_t &outputLength) {
  outputLength = 0;
  if (!child || childLength == 0 || !output || outputCapacity == 0) {
    return false;
  }
  const size_t separator = baseLength ? 1U : 0U;
  if (baseLength > WifiTransferPolicy::kMaxPathBytes - separator ||
      childLength >
          WifiTransferPolicy::kMaxPathBytes - baseLength - separator) {
    return false;
  }
  outputLength = baseLength + separator + childLength;
  if (outputLength + 1U > outputCapacity) return false;
  size_t offset = 0;
  if (baseLength) {
    memcpy(output, base, baseLength);
    offset = baseLength;
    output[offset++] = '/';
  }
  memcpy(output + offset, child, childLength);
  output[outputLength] = '\0';
  WifiTransferPolicy::PathResult checked =
      WifiTransferPolicy::validateCanonicalRelativePath(
          output, outputLength);
  if (checked.status != WifiTransferPolicy::PathStatus::Ok) {
    output[0] = '\0';
    outputLength = 0;
    return false;
  }
  return true;
}

bool WifiTransferModeController::combineExistingRelative(
    const char *base, size_t baseLength, const char *child,
    size_t childLength, char *output, size_t outputCapacity,
    size_t &outputLength) {
  outputLength = 0;
  if (!child || childLength == 0 || !output || outputCapacity == 0) {
    return false;
  }
  const size_t separator = baseLength ? 1U : 0U;
  if (baseLength > WifiTransferPolicy::kMaxPathBytes - separator ||
      childLength >
          WifiTransferPolicy::kMaxPathBytes - baseLength - separator) {
    return false;
  }
  outputLength = baseLength + separator + childLength;
  if (outputLength + 1U > outputCapacity) return false;
  size_t offset = 0;
  if (baseLength) {
    memcpy(output, base, baseLength);
    offset = baseLength;
    output[offset++] = '/';
  }
  memcpy(output + offset, child, childLength);
  output[outputLength] = '\0';
  WifiTransferPolicy::PathResult checked =
      WifiTransferPolicy::validateExistingRelativePath(output, outputLength);
  if (checked.status != WifiTransferPolicy::PathStatus::Ok) {
    output[0] = '\0';
    outputLength = 0;
    return false;
  }
  return true;
}

bool WifiTransferModeController::decodeOptionalBaseHeader() {
  if (!workspace_ || !server_) return false;
  const String encoded = server_->header(kHeaderBase);
  if (encoded.length() == 0) {
    workspace_->canonicalBase[0] = '\0';
    workspace_->canonicalBaseLength = 0;
    return true;
  }
  if (encoded.length() >
      WifiTransferPolicy::kMaxPathBytes * 3U) {
    return false;
  }
  WifiTransferPolicy::PathResult decoded =
      WifiTransferPolicy::normalizeRelativePath(
          encoded.c_str(), encoded.length(), workspace_->canonicalBase,
          sizeof(workspace_->canonicalBase));
  workspace_->canonicalBaseLength = decoded.length;
  return decoded.status == WifiTransferPolicy::PathStatus::Ok;
}

bool WifiTransferModeController::planUploadFromHeaders(
    uint64_t &declaredBytes, uint64_t &freeBytes, int &httpCode,
    const char *&reason) {
  reason = "invalid_request";
  httpCode = 400;
  declaredBytes = 0;
  freeBytes = 0;
  if (!workspace_ || !server_ || !storage_) return false;

  bool accepting = false;
  bool busy = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  busy = WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
         firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting) {
    reason = "not_accepting";
    httpCode = 503;
    return false;
  }
  if (busy) {
    reason = "writer_busy";
    httpCode = 409;
    return false;
  }
  if (storage_->accessMode() != MediaFsAccessMode::TransferReadWrite) {
    reason = "sd_not_transfer_rw";
    httpCode = 503;
    return false;
  }

  const String encodedPath = server_->header(kHeaderPath);
  const String sizeHeader = server_->header(kHeaderDeclaredSize);
  if (encodedPath.length() == 0 ||
      encodedPath.length() >
          WifiTransferPolicy::kMaxPathBytes * 3U ||
      sizeHeader.length() > 10U ||
      !parseUnsignedDecimal(sizeHeader, WIFI_TRANSFER_MAX_FILE_BYTES,
                            declaredBytes) ||
      declaredBytes == 0) {
    reason = "invalid_declared_size_or_path";
    return false;
  }
  if (!decodeOptionalBaseHeader()) {
    reason = "base_path_rejected";
    Serial.println("WIFI XFER: path rejected reason=base-policy");
    return false;
  }

  WifiTransferPolicy::UploadPlanResult plan =
      WifiTransferPolicy::prepareUploadPaths(
          encodedPath.c_str(), encodedPath.length(), workspace_->childFinal,
          sizeof(workspace_->childFinal), workspace_->childTemporary,
          sizeof(workspace_->childTemporary));
  if (plan.status != WifiTransferPolicy::UploadPlanStatus::Ok) {
    reason = "upload_path_rejected";
    Serial.printf("WIFI XFER: path rejected reason=%s policy=%s\n",
                  WifiTransferPolicy::uploadPlanStatusText(plan.status),
                  WifiTransferPolicy::pathStatusText(plan.pathStatus));
    return false;
  }
  workspace_->childFinalLength = plan.finalLength;
  workspace_->childTemporaryLength = plan.temporaryLength;

  if (!combineRelative(
          workspace_->canonicalBase, workspace_->canonicalBaseLength,
          workspace_->childFinal, workspace_->childFinalLength,
          workspace_->combinedFinal, sizeof(workspace_->combinedFinal),
          workspace_->combinedFinalLength) ||
      !combineRelative(
          workspace_->canonicalBase, workspace_->canonicalBaseLength,
          workspace_->childTemporary, workspace_->childTemporaryLength,
          workspace_->combinedTemporary,
          sizeof(workspace_->combinedTemporary),
          workspace_->combinedTemporaryLength) ||
      !makeAbsolute(workspace_->combinedFinal,
                    workspace_->combinedFinalLength,
                    workspace_->absoluteFinal,
                    sizeof(workspace_->absoluteFinal)) ||
      !makeAbsolute(workspace_->combinedTemporary,
                    workspace_->combinedTemporaryLength,
                    workspace_->absoluteTemporary,
                    sizeof(workspace_->absoluteTemporary))) {
    reason = "combined_path_too_long";
    Serial.println("WIFI XFER: path rejected reason=combined-path");
    return false;
  }

  bool finalExists = false;
  bool temporaryExists = false;
  {
    SharedSpiGuard guard;
    if (!storage_->freeSpaceBytes(freeBytes)) {
      reason = "free_space_unavailable";
      httpCode = 503;
      return false;
    }
    finalExists = storage_->exists(workspace_->absoluteFinal);
    temporaryExists = storage_->exists(workspace_->absoluteTemporary);
  }

  WifiTransferPolicy::DestinationStatus destination =
      WifiTransferPolicy::checkDestinationAvailability(
          finalExists, temporaryExists);
  if (destination != WifiTransferPolicy::DestinationStatus::Available) {
    reason = finalExists ? "final_exists" : "incomplete_exists";
    httpCode = 409;
    return false;
  }

  WifiTransferPolicy::DeclaredSizeResult sizeCheck =
      WifiTransferPolicy::checkDeclaredSize(
          true, declaredBytes, freeBytes,
          WifiTransferPolicy::kDefaultFreeSpaceMarginBytes);
  if (sizeCheck.status != WifiTransferPolicy::DeclaredSizeStatus::Ok) {
    reason = "insufficient_free_space";
    httpCode = 507;
    return false;
  }
  return true;
}

void WifiTransferModeController::handlePreflight() {
  uint64_t declared = 0;
  uint64_t free = 0;
  int code = 400;
  const char *reason = nullptr;
  if (!planUploadFromHeaders(declared, free, code, reason)) {
    sendJsonError(code, reason, "Upload preflight was rejected.");
    return;
  }
  char fields[128] = {0};
  snprintf(fields, sizeof(fields),
           "\"declaredBytes\":\"%" PRIu64 "\",\"freeBytes\":\"%" PRIu64
           "\",\"maxFileBytes\":\"%" PRIu64 "\"",
           declared, free, WIFI_TRANSFER_MAX_FILE_BYTES);
  sendJsonOk(fields);
}

bool WifiTransferModeController::pathIsDirectory(
    const char *absolutePath) {
  if (!workspace_ || !storage_ || !absolutePath) return false;
  workspace_->directoryTypeScratch.close();
  workspace_->directoryTypeScratch = storage_->open(absolutePath);
  const bool directory =
      workspace_->directoryTypeScratch &&
      workspace_->directoryTypeScratch.isDirectory();
  workspace_->directoryTypeScratch.close();
  return directory;
}

void WifiTransferModeController::closeDeleteWorkspace() {
  if (!workspace_) return;
  workspace_->deleteEntry.close();
  for (size_t index = 0; index < kFolderDeleteMaxDepth; ++index) {
    workspace_->deleteDirectories[index].close();
  }
}

bool WifiTransferModeController::scanFolderTreeForDeletion(
    const char *absolutePath, uint32_t &entryCount) {
  entryCount = 0;
  if (!workspace_ || !storage_ || !absolutePath || !absolutePath[0]) {
    return false;
  }
  closeDeleteWorkspace();
  workspace_->deleteDirectories[0] = storage_->open(absolutePath);
  if (!workspace_->deleteDirectories[0] ||
      !workspace_->deleteDirectories[0].isDirectory()) {
    closeDeleteWorkspace();
    return false;
  }

  size_t depth = 0;
  for (;;) {
    workspace_->deleteEntry.close();
    if (workspace_->deleteDirectories[depth].openNextFile(
            workspace_->deleteEntry)) {
      if (++entryCount > kFolderDeleteMaxEntries) {
        closeDeleteWorkspace();
        return false;
      }
      if (workspace_->deleteEntry.isDirectory()) {
        if (depth + 1U >= kFolderDeleteMaxDepth) {
          closeDeleteWorkspace();
          return false;
        }
        workspace_->deleteDirectories[++depth] =
            std::move(workspace_->deleteEntry);
      } else {
        workspace_->deleteEntry.close();
      }
      if ((entryCount & 0x3FU) == 0U) taskYIELD();
      continue;
    }

    workspace_->deleteDirectories[depth].close();
    if (depth == 0U) break;
    --depth;
  }
  closeDeleteWorkspace();
  return true;
}

bool WifiTransferModeController::deleteFolderTree(
    const char *absolutePath, uint32_t &removedEntries) {
  removedEntries = 0;
  if (!workspace_ || !storage_ || !absolutePath || !absolutePath[0]) {
    return false;
  }

  const size_t initialLength = strlen(absolutePath);
  if (initialLength == 0U ||
      initialLength >= sizeof(workspace_->scratchPath)) {
    return false;
  }
  memcpy(workspace_->scratchPath, absolutePath, initialLength + 1U);

  // Deleting the entry currently being used by a directory iterator can move
  // or retire exFAT directory records.  The v13.2.4 implementation continued
  // that same iterator and could therefore stop with entries still present.
  // Reopen the exact directory after every mutation and always delete its
  // first remaining child.  This is bounded by the completed preflight and it
  // preserves legacy trailing-space names through MediaFs exact-path fallback.
  size_t pathLengths[kFolderDeleteMaxDepth] = {0};
  pathLengths[0] = initialLength;
  size_t depth = 0;
  closeDeleteWorkspace();

  for (;;) {
    workspace_->deleteEntry.close();
    workspace_->deleteDirectories[0].close();
    workspace_->deleteDirectories[0] =
        storage_->open(workspace_->scratchPath);
    if (!workspace_->deleteDirectories[0] ||
        !workspace_->deleteDirectories[0].isDirectory()) {
      closeDeleteWorkspace();
      return false;
    }

    if (workspace_->deleteDirectories[0].openNextFileForDelete(
            workspace_->deleteEntry)) {
      if (workspace_->deleteEntry.isDirectory()) {
        if (depth + 1U >= kFolderDeleteMaxDepth) {
          closeDeleteWorkspace();
          return false;
        }
        const char *childName = workspace_->deleteEntry.name();
        const size_t childLength = strlen(childName);
        const size_t parentLength = pathLengths[depth];
        const bool needsSlash = parentLength > 0U &&
                                workspace_->scratchPath[parentLength - 1U] != '/';
        const size_t joinedLength = parentLength +
                                    (needsSlash ? 1U : 0U) + childLength;
        if (childLength == 0U ||
            joinedLength >= sizeof(workspace_->scratchPath)) {
          closeDeleteWorkspace();
          return false;
        }
        size_t offset = parentLength;
        if (needsSlash) workspace_->scratchPath[offset++] = '/';
        memcpy(workspace_->scratchPath + offset, childName, childLength);
        workspace_->scratchPath[joinedLength] = '\0';
        pathLengths[++depth] = joinedLength;
        workspace_->deleteEntry.close();
        workspace_->deleteDirectories[0].close();
      } else {
        if (!storage_->removeTransferFileHandle(
                workspace_->deleteEntry)) {
          closeDeleteWorkspace();
          return false;
        }
        workspace_->deleteDirectories[0].close();
        ++removedEntries;
      }
      if ((removedEntries & 0x3FU) == 0U) taskYIELD();
      continue;
    }

    workspace_->deleteDirectories[0].close();
    workspace_->deleteDirectories[0] =
        storage_->open(workspace_->scratchPath);
    if (!workspace_->deleteDirectories[0] ||
        !workspace_->deleteDirectories[0].isDirectory() ||
        !storage_->removeTransferDirectoryHandle(
            workspace_->deleteDirectories[0])) {
      closeDeleteWorkspace();
      return false;
    }
    ++removedEntries;
    if (depth == 0U) break;
    --depth;
    workspace_->scratchPath[pathLengths[depth]] = '\0';
    if ((removedEntries & 0x3FU) == 0U) taskYIELD();
  }

  closeDeleteWorkspace();
  return true;
}

bool WifiTransferModeController::ensureUploadParentDirectories() {
  if (!workspace_ || !storage_) return false;
  const char *relative = workspace_->combinedFinal;
  const size_t length = workspace_->combinedFinalLength;
  for (size_t index = 0; index < length; ++index) {
    if (relative[index] != '/') continue;
    if (index == 0 || index >= sizeof(workspace_->scratchPath)) return false;
    memcpy(workspace_->scratchPath, relative, index);
    workspace_->scratchPath[index] = '\0';
    if (!makeAbsolute(workspace_->scratchPath, index,
                      workspace_->absoluteDirectory,
                      sizeof(workspace_->absoluteDirectory))) {
      return false;
    }
    {
      SharedSpiGuard guard;
      if (storage_->exists(workspace_->absoluteDirectory)) {
        if (!pathIsDirectory(workspace_->absoluteDirectory)) return false;
      } else if (!storage_->makeTransferDirectory(
                     workspace_->absoluteDirectory)) {
        return false;
      }
    }
    vTaskDelay(1);
  }
  return true;
}

void WifiTransferModeController::resetFirmwareUpdateState() {
  if (Update.isRunning()) Update.abort();
  firmwareUpdateActive_ = false;
  firmwareUpdateResponseReady_ = false;
  firmwareUpdateSucceeded_ = false;
  firmwareHeaderValidated_ = false;
  firmwareUpdateResponseCode_ = 500;
  firmwareHeaderUsed_ = 0;
  firmwareDeclaredBytes_ = 0;
  firmwareReceivedBytes_ = 0;
  firmwareStartedMs_ = 0;
  firmwareRebootAt_ = 0;
  memset(firmwareHeader_, 0, sizeof(firmwareHeader_));
  firmwareResponseReason_[0] = '\0';
  firmwareResponseMessage_[0] = '\0';
  firmwareVersion_[0] = '\0';
}

void WifiTransferModeController::failFirmwareUpdate(
    int code, const char *reason, const char *message, bool stopClient) {
  if (Update.isRunning()) Update.abort();
  portENTER_CRITICAL(&stateMux_);
  firmwareUpdateActive_ = false;
  firmwareUpdateSucceeded_ = false;
  firmwareUpdateResponseReady_ = true;
  firmwareUpdateResponseCode_ = code;
  accepting_ = state_ == WifiTransferServiceState::Serving &&
               !finishRequested_ && !closeHttpRequested_;
  copyText(firmwareResponseReason_, sizeof(firmwareResponseReason_),
           reason ? reason : "firmware_update_failed");
  copyText(firmwareResponseMessage_, sizeof(firmwareResponseMessage_),
           message ? message : "Firmware update failed.");
  copyText(message_, sizeof(message_), firmwareResponseMessage_);
  portEXIT_CRITICAL(&stateMux_);
  Serial.printf("WIFI OTA: failed reason=%s received=%" PRIu64
                " declared=%" PRIu64 " update_error=%u\n",
                firmwareResponseReason_, firmwareReceivedBytes_,
                firmwareDeclaredBytes_, Update.getError());
  if (stopClient && server_) server_->client().stop();
}

bool WifiTransferModeController::validateFirmwareHeader() {
  if (firmwareHeaderUsed_ < sizeof(firmwareHeader_)) return false;
  esp_image_header_t imageHeader = {};
  memcpy(&imageHeader, firmwareHeader_, sizeof(imageHeader));
  if (imageHeader.magic != ESP_IMAGE_HEADER_MAGIC ||
      imageHeader.segment_count == 0 ||
      imageHeader.chip_id != ESP_CHIP_ID_ESP32S3) {
    return false;
  }

  esp_app_desc_t app = {};
  constexpr size_t appOffset =
      sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  memcpy(&app, firmwareHeader_ + appOffset, sizeof(app));
  if (app.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
  copyText(firmwareVersion_, sizeof(firmwareVersion_), app.version);
  firmwareHeaderValidated_ = true;
  return true;
}

void WifiTransferModeController::handleFirmwareRaw() {
  if (!server_) return;
  HTTPRaw &raw = server_->raw();

  if (raw.status == RAW_START) {
    resetFirmwareUpdateState();
    const int contentLength = server_->clientContentLength();
    uint64_t declared = 0;
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    bool accepting = false;
    bool sdWriterActive = false;
    portENTER_CRITICAL(&stateMux_);
    accepting = accepting_;
    sdWriterActive =
        WifiTransferPolicy::uploadWriterIsActive(uploadPhase_);
    portEXIT_CRITICAL(&stateMux_);

    const String sizeHeader = server_->header(kHeaderDeclaredSize);
    if (!accepting || sdWriterActive || contentLength <= 0 || !next ||
        sizeHeader.length() > 10U ||
        !parseUnsignedDecimal(sizeHeader, 0xFFFFFFFFULL, declared) ||
        declared != static_cast<uint64_t>(contentLength) ||
        declared < kMinimumFirmwareImageBytes || declared > next->size) {
      failFirmwareUpdate(
          !accepting ? 503 : (sdWriterActive ? 409 : 400),
          "firmware_preflight_rejected",
          "Select a valid application-only firmware .bin that fits the OTA slot.",
          true);
      return;
    }

    // Music/BLE are already stopped by entry into transfer mode. Flush the SD
    // metadata once before internal-flash writing; no SD operation is admitted
    // while firmwareUpdateActive_ is true.
    bool sdSynced = false;
    if (storage_ &&
        storage_->accessMode() == MediaFsAccessMode::TransferReadWrite) {
      SharedSpiGuard guard;
      sdSynced = storage_->syncTransferDevice();
    }
    if (!sdSynced || !Update.begin(static_cast<size_t>(declared), U_FLASH)) {
      failFirmwareUpdate(500, "firmware_begin_failed",
                         sdSynced ? Update.errorString()
                                  : "SD metadata could not be synchronized.",
                         true);
      return;
    }

    portENTER_CRITICAL(&stateMux_);
    firmwareUpdateActive_ = true;
    firmwareDeclaredBytes_ = declared;
    firmwareStartedMs_ = millis();
    accepting_ = false;
    writtenBytes_ = 0;
    declaredBytes_ = declared;
    uploadStartedMs_ = firmwareStartedMs_;
    copyText(currentFile_, sizeof(currentFile_), "Firmware application");
    copyText(message_, sizeof(message_), "Receiving firmware update");
    portEXIT_CRITICAL(&stateMux_);
    Serial.printf("WIFI OTA: begin bytes=%" PRIu64
                  " target=%s offset=0x%08lX slot_bytes=%lu\n",
                  declared, next->label,
                  static_cast<unsigned long>(next->address),
                  static_cast<unsigned long>(next->size));
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!firmwareUpdateActive_) return;
    if (firmwareReceivedBytes_ + raw.currentSize > firmwareDeclaredBytes_) {
      failFirmwareUpdate(400, "firmware_size_overrun",
                         "Firmware body exceeded its declared size.", true);
      return;
    }

    size_t sourceOffset = 0;
    if (!firmwareHeaderValidated_) {
      const size_t needed = sizeof(firmwareHeader_) - firmwareHeaderUsed_;
      const size_t copied = std::min(needed, raw.currentSize);
      memcpy(firmwareHeader_ + firmwareHeaderUsed_, raw.buf, copied);
      firmwareHeaderUsed_ += copied;
      sourceOffset += copied;
      if (firmwareHeaderUsed_ == sizeof(firmwareHeader_)) {
        if (!validateFirmwareHeader() ||
            Update.write(firmwareHeader_, sizeof(firmwareHeader_)) !=
                sizeof(firmwareHeader_)) {
          failFirmwareUpdate(400, "invalid_application_image",
                             "The selected file is not an ESP32-S3 application image.",
                             true);
          return;
        }
      }
    }

    if (sourceOffset < raw.currentSize) {
      const size_t remaining = raw.currentSize - sourceOffset;
      if (!firmwareHeaderValidated_ ||
          Update.write(raw.buf + sourceOffset, remaining) != remaining) {
        failFirmwareUpdate(500, "firmware_write_failed",
                           Update.errorString(), true);
        return;
      }
    }

    firmwareReceivedBytes_ += raw.currentSize;
    portENTER_CRITICAL(&stateMux_);
    writtenBytes_ = firmwareReceivedBytes_;
    uploadElapsedMs_ = elapsedSince(firmwareStartedMs_, millis());
    portEXIT_CRITICAL(&stateMux_);
    noteWebActivity();
    return;
  }

  if (raw.status == RAW_END) {
    if (!firmwareUpdateActive_) return;
    if (!firmwareHeaderValidated_ ||
        firmwareReceivedBytes_ != firmwareDeclaredBytes_ ||
        !Update.end(false) || !Update.isFinished()) {
      failFirmwareUpdate(400, "firmware_verification_failed",
                         Update.errorString(), false);
      return;
    }
    portENTER_CRITICAL(&stateMux_);
    firmwareUpdateActive_ = false;
    firmwareUpdateSucceeded_ = true;
    firmwareUpdateResponseReady_ = true;
    firmwareUpdateResponseCode_ = 200;
    uploadElapsedMs_ = elapsedSince(firmwareStartedMs_, millis());
    copyText(firmwareResponseReason_, sizeof(firmwareResponseReason_), "ok");
    copyText(firmwareResponseMessage_, sizeof(firmwareResponseMessage_),
             "Firmware verified. DSPi is restarting.");
    copyText(message_, sizeof(message_), firmwareResponseMessage_);
    portEXIT_CRITICAL(&stateMux_);
    Serial.printf("WIFI OTA: verified bytes=%" PRIu64 " version=%s\n",
                  firmwareReceivedBytes_,
                  firmwareVersion_[0] ? firmwareVersion_ : "unknown");
    return;
  }

  if (raw.status == RAW_ABORTED && firmwareUpdateActive_) {
    failFirmwareUpdate(499, "firmware_upload_aborted",
                       "Firmware upload was cancelled or disconnected.",
                       false);
  }
}

void WifiTransferModeController::handleFirmwareFinished() {
  if (!firmwareUpdateResponseReady_) {
    sendJsonError(500, "missing_firmware_result",
                  "Firmware upload did not produce a terminal result.");
    return;
  }
  if (!firmwareUpdateSucceeded_) {
    sendJsonError(firmwareUpdateResponseCode_, firmwareResponseReason_,
                  firmwareResponseMessage_);
    return;
  }

  beginChunkedJson();
  WebServer &server = *server_;
  char fields[192] = {0};
  server.sendContent("{\"ok\":true,\"message\":");
  sendJsonString(firmwareResponseMessage_);
  snprintf(fields, sizeof(fields),
           ",\"bytes\":\"%" PRIu64 "\",\"version\":",
           firmwareReceivedBytes_);
  server.sendContent(fields);
  sendJsonString(firmwareVersion_[0] ? firmwareVersion_ : "unknown");
  server.sendContent("}");
  endChunkedJson();

  portENTER_CRITICAL(&stateMux_);
  firmwareRebootAt_ = millis() + kFirmwareResponseDrainMs;
  portEXIT_CRITICAL(&stateMux_);
}

void WifiTransferModeController::handleUploadRaw() {
  if (!workspace_ || !server_) return;
  HTTPRaw &raw = server_->raw();

  if (raw.status == RAW_START) {
    workspace_->uploadResponseReady = false;
    workspace_->uploadResponseCode = 500;
    workspace_->writerReceivedBytes = 0;
    workspace_->writerPersistedBytes = 0;
    workspace_->uploadBufferUsed = 0;
    workspace_->writerDeclaredBytes = 0;
    workspace_->writerSyncCount = 0;
    workspace_->bytesAtLastSync = 0;
    workspace_->bytesAtLastProgressLog = 0;
    workspace_->bytesAtLastYield = 0;
    workspace_->writerStartedMs = millis();
    workspace_->lastProgressLogMs = workspace_->writerStartedMs;
    workspace_->writerStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    workspace_->startPreparationUs = 0;
    workspace_->lastRawCallbackEndUs = 0;
    workspace_->firstRawWriteUs = 0;
    workspace_->lastRawWriteEndUs = 0;
    workspace_->rawGapTotalUs = 0;
    workspace_->rawGapMaxUs = 0;
    workspace_->rawCallbackTotalUs = 0;
    workspace_->rawCallbackMaxUs = 0;
    workspace_->rawCopyTotalUs = 0;
    workspace_->rawCopyMaxUs = 0;
    workspace_->spiLockWaitTotalUs = 0;
    workspace_->spiLockWaitMaxUs = 0;
    workspace_->sdWriteTotalUs = 0;
    workspace_->sdWriteMaxUs = 0;
    workspace_->sdWriteBytes = 0;
    workspace_->fileSyncTotalUs = 0;
    workspace_->fileSyncMaxUs = 0;
    workspace_->yieldTotalUs = 0;
    workspace_->yieldMaxUs = 0;
    workspace_->pipelineQueueWaitTotalUs = 0;
    workspace_->pipelineQueueWaitMaxUs = 0;
    workspace_->pipelineBarrierWaitUs = 0;
    workspace_->preallocateUs = 0;
    workspace_->finalizeTotalUs = 0;
    workspace_->finalFileSyncUs = 0;
    workspace_->deviceSyncBeforeRenameUs = 0;
    workspace_->renameUs = 0;
    workspace_->deviceSyncAfterRenameUs = 0;
    workspace_->rawCallbackCount = 0;
    workspace_->rawChunkMinBytes = 0;
    workspace_->rawChunkMaxBytes = 0;
    workspace_->sdWriteCount = 0;
    workspace_->yieldCount = 0;
    workspace_->pipelineQueuedBlocks = 0;
    workspace_->pipelineBackpressureCount = 0;
    workspace_->uploadPreallocated = false;
    workspace_->uploadRssiDbm =
        WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    workspace_->uploadWifiChannel =
        static_cast<uint8_t>(WiFi.channel());
    workspace_->uploadApClients =
        static_cast<uint8_t>(WiFi.softAPgetStationNum());
    workspace_->uploadFile.close();

    uint64_t declared = 0;
    uint64_t free = 0;
    int code = 400;
    const char *reason = nullptr;
    const int contentLength = server_->clientContentLength();
    if (!planUploadFromHeaders(declared, free, code, reason) ||
        contentLength <= 0 ||
        static_cast<uint64_t>(contentLength) != declared) {
      workspace_->uploadResponseCode =
          contentLength <= 0 ? 411 : code;
      copyText(workspace_->uploadResponseReason,
               sizeof(workspace_->uploadResponseReason),
               contentLength <= 0 ? "content_length_required"
                                  : (reason ? reason : "preflight_failed"));
      copyText(workspace_->uploadResponseMessage,
               sizeof(workspace_->uploadResponseMessage),
               "Upload start was rejected.");
      workspace_->uploadResponseReady = true;
      Serial.printf(
          "WIFI XFER: upload rejected reason=%s content_length=%ld\n",
          workspace_->uploadResponseReason,
          static_cast<long>(contentLength));
      server_->client().stop();
      return;
    }

    bool opened = false;
    const bool parentsReady = ensureUploadParentDirectories();
    {
      SharedSpiGuard guard;
      if (parentsReady && !storage_->exists(workspace_->absoluteFinal) &&
          !storage_->exists(workspace_->absoluteTemporary)) {
        workspace_->uploadFile =
            storage_->createTransferFileExclusive(
                workspace_->absoluteTemporary);
        opened = static_cast<bool>(workspace_->uploadFile);
      }
    }
    if (!opened) {
      workspace_->uploadResponseCode = 409;
      copyText(workspace_->uploadResponseReason,
               sizeof(workspace_->uploadResponseReason),
               "exclusive_create_failed");
      copyText(workspace_->uploadResponseMessage,
               sizeof(workspace_->uploadResponseMessage),
               "Destination or parent directory was unavailable.");
      workspace_->uploadResponseReady = true;
      Serial.println(
          "WIFI XFER: upload failed reason=exclusive-create-or-parent");
      server_->client().stop();
      return;
    }

    bool storageLayoutReady = true;
    if (declared >= kPreallocateMinimumBytes) {
      const uint64_t preallocateStartedUs =
          static_cast<uint64_t>(esp_timer_get_time());
      {
        SharedSpiGuard guard;
        workspace_->uploadPreallocated =
            workspace_->uploadFile.preAllocate(declared);
        if (workspace_->uploadPreallocated) {
          workspace_->uploadPreallocated = workspace_->uploadFile.seek(0U);
        }
        if (!workspace_->uploadPreallocated) {
          // preAllocate() may have reserved only part of the requested extent
          // before reporting failure. Restore the exclusive temporary file to
          // a known empty state before accepting any network bytes.
          workspace_->uploadFile.clearIoError();
          storageLayoutReady = workspace_->uploadFile.truncate(0U);
          workspace_->uploadFile.clearIoError();
          if (storageLayoutReady) {
            storageLayoutReady = workspace_->uploadFile.seek(0U);
          }
          if (storageLayoutReady) workspace_->uploadFile.clearIoError();
        }
      }
      workspace_->preallocateUs =
          static_cast<uint64_t>(esp_timer_get_time()) -
          preallocateStartedUs;
    }

    if (!storageLayoutReady || !resetUploadPipelineForNewFile()) {
      {
        SharedSpiGuard guard;
        workspace_->uploadFile.close();
        if (storage_) {
          storage_->removeIncompleteTransferFile(
              workspace_->absoluteTemporary);
          storage_->syncTransferDevice();
        }
      }
      workspace_->uploadResponseCode = storageLayoutReady ? 503 : 500;
      copyText(workspace_->uploadResponseReason,
               sizeof(workspace_->uploadResponseReason),
               storageLayoutReady ? "upload_pipeline_unavailable"
                                  : "preallocation_recovery_failed");
      copyText(workspace_->uploadResponseMessage,
               sizeof(workspace_->uploadResponseMessage),
               storageLayoutReady
                   ? "Upload buffers were unavailable."
                   : "The temporary file could not be reset after preallocation.");
      workspace_->uploadResponseReady = true;
      server_->client().stop();
      return;
    }

    Serial.printf(
        "WIFI XFER: upload storage pipeline=%s buffers=%u bytes_each=%u preallocated=%s preallocate_ms=%" PRIu64 "\n",
        workspace_->uploadPipelineAvailable ? "dual" : "synchronous-fallback",
        workspace_->uploadPipelineAvailable
            ? static_cast<unsigned>(kUploadPipelineBufferCount) : 1U,
        static_cast<unsigned>(workspace_->uploadBufferCapacity),
        workspace_->uploadPreallocated ? "yes" : "no",
        workspace_->preallocateUs / 1000ULL);

    WifiTransferPolicy::UploadTransition transition =
        WifiTransferPolicy::transitionUpload(
            uploadPhase_, WifiTransferPolicy::UploadSignal::Begin);
    if (transition.disposition !=
        WifiTransferPolicy::TransitionDisposition::Applied) {
      closeIncompleteWriter("upload state rejected begin");
      server_->client().stop();
      return;
    }
    portENTER_CRITICAL(&stateMux_);
    uploadPhase_ = transition.next;
    abortWriterRequested_ = false;
    portEXIT_CRITICAL(&stateMux_);
    workspace_->writerDeclaredBytes = declared;
    updateUploadSnapshot();
    portENTER_CRITICAL(&stateMux_);
    freeBytes_ = free;
    copyText(message_, sizeof(message_), "Receiving upload");
    portEXIT_CRITICAL(&stateMux_);
    const uint64_t preparedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    workspace_->startPreparationUs =
        preparedUs - workspace_->writerStartedUs;
    workspace_->lastRawCallbackEndUs = preparedUs;
    Serial.printf(
        "WIFI XFER: upload begin path=%s declared=%" PRIu64
        " free=%" PRIu64 " prepare_ms=%" PRIu64
        " rssi_dbm=%ld channel=%u ap_clients=%u\n",
        workspace_->combinedFinal, declared, free,
        workspace_->startPreparationUs / 1000ULL,
        static_cast<long>(workspace_->uploadRssiDbm),
        static_cast<unsigned>(workspace_->uploadWifiChannel),
        static_cast<unsigned>(workspace_->uploadApClients));
    return;
  }

  if (raw.status == RAW_WRITE) {
    const uint64_t callbackStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    if (workspace_->lastRawCallbackEndUs &&
        callbackStartedUs >= workspace_->lastRawCallbackEndUs) {
      const uint64_t gapUs =
          callbackStartedUs - workspace_->lastRawCallbackEndUs;
      workspace_->rawGapTotalUs += gapUs;
      workspace_->rawGapMaxUs =
          std::max(workspace_->rawGapMaxUs, gapUs);
    }
    if (!workspace_->firstRawWriteUs) {
      workspace_->firstRawWriteUs = callbackStartedUs;
    }
    workspace_->rawCallbackCount++;
    const uint32_t rawChunkBytes =
        raw.currentSize > 0xFFFFFFFFULL
            ? 0xFFFFFFFFUL
            : static_cast<uint32_t>(raw.currentSize);
    if (!workspace_->rawChunkMinBytes ||
        rawChunkBytes < workspace_->rawChunkMinBytes) {
      workspace_->rawChunkMinBytes = rawChunkBytes;
    }
    workspace_->rawChunkMaxBytes =
        std::max(workspace_->rawChunkMaxBytes, rawChunkBytes);

    bool abortRequested = false;
    WifiTransferPolicy::UploadPhase phase;
    portENTER_CRITICAL(&stateMux_);
    abortRequested = abortWriterRequested_;
    phase = uploadPhase_;
    portEXIT_CRITICAL(&stateMux_);
    if (abortRequested ||
        phase != WifiTransferPolicy::UploadPhase::Writing) {
      closeIncompleteWriter(abortRequested ? "cancelled by user"
                                           : "writer not active");
      server_->client().stop();
      return;
    }

    uint64_t nextBytes = workspace_->writerReceivedBytes;
    WifiTransferPolicy::ByteCountStatus counted =
        WifiTransferPolicy::addReceivedBytes(
            workspace_->writerReceivedBytes, raw.currentSize,
            workspace_->writerDeclaredBytes, &nextBytes);
    if (counted != WifiTransferPolicy::ByteCountStatus::Ok) {
      closeIncompleteWriter("received bytes exceed declared length");
      server_->client().stop();
      return;
    }

    const uint32_t now = millis();
    size_t sourceOffset = 0;
    while (sourceOffset < raw.currentSize) {
      if (!workspace_->uploadBuffer ||
          workspace_->uploadBufferCapacity == 0) {
        closeIncompleteWriter("upload buffer unavailable");
        server_->client().stop();
        return;
      }
      if (workspace_->uploadBufferUsed ==
          workspace_->uploadBufferCapacity) {
        if (!flushUploadBuffer(false)) {
          closeIncompleteWriter("SD buffered write failed");
          server_->client().stop();
          return;
        }
      }
      const size_t room = workspace_->uploadBufferCapacity -
                          workspace_->uploadBufferUsed;
      const size_t remaining = raw.currentSize - sourceOffset;
      const size_t copyLength = std::min(room, remaining);
      const uint64_t copyStartedUs =
          static_cast<uint64_t>(esp_timer_get_time());
      memcpy(workspace_->uploadBuffer + workspace_->uploadBufferUsed,
             raw.buf + sourceOffset, copyLength);
      const uint64_t copyElapsedUs =
          static_cast<uint64_t>(esp_timer_get_time()) - copyStartedUs;
      workspace_->rawCopyTotalUs += copyElapsedUs;
      workspace_->rawCopyMaxUs =
          std::max(workspace_->rawCopyMaxUs, copyElapsedUs);
      workspace_->uploadBufferUsed += copyLength;
      sourceOffset += copyLength;
    }

    if (workspace_->uploadBufferUsed == workspace_->uploadBufferCapacity &&
        !flushUploadBuffer(false)) {
      closeIncompleteWriter("SD buffered write failed");
      server_->client().stop();
      return;
    }

    workspace_->writerReceivedBytes = nextBytes;
    updateUploadSnapshot();
    noteWebActivity();

    if (elapsedSince(workspace_->lastProgressLogMs, now) >=
            kProgressLogIntervalMs &&
        nextBytes - workspace_->bytesAtLastProgressLog >=
            kProgressLogIntervalBytes) {
      Serial.printf(
          "WIFI XFER: upload progress path=%s bytes=%" PRIu64
          " declared=%" PRIu64 " elapsed_ms=%lu speed_Bps=%" PRIu64
          " syncs=%lu\n",
          workspace_->combinedFinal, nextBytes,
          workspace_->writerDeclaredBytes,
          static_cast<unsigned long>(
              elapsedSince(workspace_->writerStartedMs, now)),
          elapsedSince(workspace_->writerStartedMs, now)
              ? (nextBytes * 1000ULL) /
                    elapsedSince(workspace_->writerStartedMs, now)
              : 0,
          static_cast<unsigned long>(workspace_->writerSyncCount));
      Serial.printf(
          "WIFI XFER: memory heap_free=%lu psram_free=%lu\n",
          static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(
              heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      Serial.printf(
          "WIFI XFER: stack transfer_free=%lu\n",
          static_cast<unsigned long>(
              uxTaskGetStackHighWaterMark(nullptr)));
      workspace_->lastProgressLogMs = now;
      workspace_->bytesAtLastProgressLog = nextBytes;
    }
    if (nextBytes - workspace_->bytesAtLastYield >=
        kNetworkYieldIntervalBytes) {
      workspace_->bytesAtLastYield = nextBytes;
      // The transfer task runs above loopTask priority. taskYIELD() alone can
      // only yield to an equal-priority task, so give the panel/lifecycle a
      // real one-tick window at a coarse 256 KiB cadence instead of after
      // every WebServer raw chunk.
      const uint64_t yieldStartedUs =
          static_cast<uint64_t>(esp_timer_get_time());
      vTaskDelay(pdMS_TO_TICKS(1));
      const uint64_t yieldElapsedUs =
          static_cast<uint64_t>(esp_timer_get_time()) - yieldStartedUs;
      workspace_->yieldTotalUs += yieldElapsedUs;
      workspace_->yieldMaxUs =
          std::max(workspace_->yieldMaxUs, yieldElapsedUs);
      workspace_->yieldCount++;
    }
    const uint64_t callbackEndedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t callbackElapsedUs =
        callbackEndedUs - callbackStartedUs;
    workspace_->rawCallbackTotalUs += callbackElapsedUs;
    workspace_->rawCallbackMaxUs =
        std::max(workspace_->rawCallbackMaxUs, callbackElapsedUs);
    workspace_->lastRawCallbackEndUs = callbackEndedUs;
    workspace_->lastRawWriteEndUs = callbackEndedUs;
    return;
  }

  if (raw.status == RAW_END) {
    bool activeWriter = false;
    portENTER_CRITICAL(&stateMux_);
    activeWriter =
        WifiTransferPolicy::uploadWriterIsActive(uploadPhase_);
    portEXIT_CRITICAL(&stateMux_);
    if (!activeWriter) return;
    if (!WifiTransferPolicy::finalByteCountMatches(
            workspace_->writerReceivedBytes,
            workspace_->writerDeclaredBytes)) {
      closeIncompleteWriter("final byte count mismatch");
      return;
    }
    finalizeUpload();
    return;
  }

  if (raw.status == RAW_ABORTED) {
    bool activeWriter = false;
    portENTER_CRITICAL(&stateMux_);
    activeWriter =
        WifiTransferPolicy::uploadWriterIsActive(uploadPhase_);
    portEXIT_CRITICAL(&stateMux_);
    if (activeWriter || workspace_->uploadFile) {
      closeIncompleteWriter("client disconnected or upload stalled");
    }
  }
}

bool WifiTransferModeController::resetUploadPipelineForNewFile() {
  if (!workspace_) return false;

  workspace_->uploadBufferUsed = 0;
  workspace_->uploadWriterFailed = false;
  workspace_->uploadBarrierOk = false;
  workspace_->uploadBuffer = nullptr;
  workspace_->uploadBufferIndex = 0xFFU;
  if (workspace_->uploadBarrier) {
    while (xSemaphoreTake(workspace_->uploadBarrier, 0) == pdPASS) {
    }
  }

  if (!workspace_->uploadPipelineAvailable) {
    if (!workspace_->uploadBuffers[0]) return false;
    workspace_->uploadBuffer = workspace_->uploadBuffers[0];
    workspace_->uploadBufferIndex = 0U;
    return true;
  }

  uint8_t index = 0xFFU;
  if (xQueueReceive(workspace_->uploadFreeQueue, &index,
                    kUploadPipelineWaitTicks) != pdPASS ||
      index >= kUploadPipelineBufferCount ||
      !workspace_->uploadBuffers[index]) {
    return false;
  }
  workspace_->uploadBufferIndex = index;
  workspace_->uploadBuffer = workspace_->uploadBuffers[index];
  return true;
}

bool WifiTransferModeController::writeUploadBlock(
    const uint8_t *buffer, size_t length, bool syncAfterWrite) {
  if (!workspace_ || !workspace_->uploadFile) return false;

  bool writeOk = true;
  bool syncOk = true;
  const uint64_t lockStartedUs =
      static_cast<uint64_t>(esp_timer_get_time());
  SharedSpiGuard guard;
  const uint64_t lockWaitUs =
      static_cast<uint64_t>(esp_timer_get_time()) - lockStartedUs;
  workspace_->spiLockWaitTotalUs += lockWaitUs;
  workspace_->spiLockWaitMaxUs =
      std::max(workspace_->spiLockWaitMaxUs, lockWaitUs);

  if (length > 0U) {
    if (!buffer) return false;
    const uint64_t writeStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    const size_t written = workspace_->uploadFile.write(buffer, length);
    const uint64_t writeElapsedUs =
        static_cast<uint64_t>(esp_timer_get_time()) - writeStartedUs;
    workspace_->sdWriteTotalUs += writeElapsedUs;
    workspace_->sdWriteMaxUs =
        std::max(workspace_->sdWriteMaxUs, writeElapsedUs);
    workspace_->sdWriteBytes += written;
    workspace_->sdWriteCount++;
    workspace_->writerPersistedBytes += written;
    writeOk = written == length && !workspace_->uploadFile.hadIoError();
  }

  const bool intervalReached =
      workspace_->writerPersistedBytes - workspace_->bytesAtLastSync >=
      kFileSyncIntervalBytes;
  if (writeOk && (syncAfterWrite || intervalReached)) {
    const uint64_t syncStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    syncOk = workspace_->uploadFile.sync();
    const uint64_t syncElapsedUs =
        static_cast<uint64_t>(esp_timer_get_time()) - syncStartedUs;
    workspace_->fileSyncTotalUs += syncElapsedUs;
    workspace_->fileSyncMaxUs =
        std::max(workspace_->fileSyncMaxUs, syncElapsedUs);
    if (syncOk) {
      workspace_->bytesAtLastSync = workspace_->writerPersistedBytes;
      workspace_->writerSyncCount++;
    }
  }
  return writeOk && syncOk;
}

bool WifiTransferModeController::queueCurrentUploadBuffer(
    bool acquireReplacement) {
  if (!workspace_ || !workspace_->uploadFile) return false;

  if (!workspace_->uploadPipelineAvailable) {
    const size_t requested = workspace_->uploadBufferUsed;
    const bool ok = requested == 0U ||
                    writeUploadBlock(workspace_->uploadBuffer, requested,
                                     false);
    if (ok) workspace_->uploadBufferUsed = 0U;
    return ok;
  }

  bool queuedOk = true;
  if (workspace_->uploadBufferIndex >= kUploadPipelineBufferCount ||
      !workspace_->uploadBuffer) {
    return false;
  }

  if (workspace_->uploadBufferUsed > 0U) {
    UploadWriterCommand command;
    command.type = UploadWriterCommandType::Write;
    command.bufferIndex = workspace_->uploadBufferIndex;
    command.length = workspace_->uploadBufferUsed;
    if (xQueueSend(workspace_->uploadWriterQueue, &command,
                   kUploadPipelineWaitTicks) != pdPASS) {
      return false;
    }
    workspace_->pipelineQueuedBlocks++;
    workspace_->uploadBuffer = nullptr;
    workspace_->uploadBufferIndex = 0xFFU;
    workspace_->uploadBufferUsed = 0U;
  } else if (acquireReplacement) {
    // An empty producer buffer is already writable. Keeping it avoids taking
    // a second buffer from the free queue and cannot deadlock the two-buffer
    // pipeline if a caller requests an otherwise unnecessary flush.
    return !workspace_->uploadWriterFailed;
  } else {
    const uint8_t returnedIndex = workspace_->uploadBufferIndex;
    if (xQueueSend(workspace_->uploadFreeQueue, &returnedIndex,
                   kUploadPipelineWaitTicks) != pdPASS) {
      return false;
    }
    workspace_->uploadBuffer = nullptr;
    workspace_->uploadBufferIndex = 0xFFU;
  }

  if (acquireReplacement) {
    const uint64_t waitStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    uint8_t index = 0xFFU;
    const BaseType_t received = xQueueReceive(
        workspace_->uploadFreeQueue, &index, kUploadPipelineWaitTicks);
    const uint64_t waitUs =
        static_cast<uint64_t>(esp_timer_get_time()) - waitStartedUs;
    workspace_->pipelineQueueWaitTotalUs += waitUs;
    workspace_->pipelineQueueWaitMaxUs =
        std::max(workspace_->pipelineQueueWaitMaxUs, waitUs);
    if (waitUs >= 1000ULL) workspace_->pipelineBackpressureCount++;
    if (received != pdPASS || index >= kUploadPipelineBufferCount ||
        !workspace_->uploadBuffers[index]) {
      queuedOk = false;
    } else {
      workspace_->uploadBufferIndex = index;
      workspace_->uploadBuffer = workspace_->uploadBuffers[index];
    }
  }

  return queuedOk && !workspace_->uploadWriterFailed;
}

bool WifiTransferModeController::drainUploadPipeline(bool syncAfterWrite) {
  if (!workspace_ || !workspace_->uploadFile) return false;

  if (!workspace_->uploadPipelineAvailable) {
    const size_t requested = workspace_->uploadBufferUsed;
    bool ok = true;
    if (requested > 0U) {
      ok = writeUploadBlock(workspace_->uploadBuffer, requested, false);
      if (ok) workspace_->uploadBufferUsed = 0U;
    }
    if (ok && syncAfterWrite) {
      ok = writeUploadBlock(nullptr, 0U, true);
    }
    return ok;
  }

  while (xSemaphoreTake(workspace_->uploadBarrier, 0) == pdPASS) {
  }
  const bool queuedOk = queueCurrentUploadBuffer(false);

  UploadWriterCommand barrier;
  barrier.type = UploadWriterCommandType::Barrier;
  barrier.syncAfterWrite = syncAfterWrite;
  if (xQueueSend(workspace_->uploadWriterQueue, &barrier,
                 kUploadPipelineWaitTicks) != pdPASS) {
    return false;
  }

  const uint64_t waitStartedUs =
      static_cast<uint64_t>(esp_timer_get_time());
  const bool completed =
      xSemaphoreTake(workspace_->uploadBarrier,
                     kUploadPipelineWaitTicks) == pdPASS;
  workspace_->pipelineBarrierWaitUs +=
      static_cast<uint64_t>(esp_timer_get_time()) - waitStartedUs;
  return completed && queuedOk && workspace_->uploadBarrierOk &&
         !workspace_->uploadWriterFailed;
}

bool WifiTransferModeController::flushUploadBuffer(bool syncAfterWrite) {
  if (!workspace_ || !workspace_->uploadFile) return false;
  if (syncAfterWrite) return drainUploadPipeline(true);
  return queueCurrentUploadBuffer(true);
}

void WifiTransferModeController::closeIncompleteWriter(
    const char *reason) {
  if (!workspace_) return;

  portENTER_CRITICAL(&stateMux_);
  if (uploadPhase_ == WifiTransferPolicy::UploadPhase::Writing ||
      uploadPhase_ == WifiTransferPolicy::UploadPhase::Finalizing) {
    WifiTransferPolicy::UploadTransition transition =
        WifiTransferPolicy::transitionUpload(
            uploadPhase_, WifiTransferPolicy::UploadSignal::Cancel);
    if (transition.disposition ==
            WifiTransferPolicy::TransitionDisposition::Applied ||
        transition.disposition ==
            WifiTransferPolicy::TransitionDisposition::NoChange) {
      uploadPhase_ = transition.next;
    }
  }
  portEXIT_CRITICAL(&stateMux_);

  bool syncOk = true;
  if (workspace_->uploadFile) {
    syncOk = flushUploadBuffer(true);
    {
      SharedSpiGuard guard;
      if (workspace_->uploadPreallocated &&
          workspace_->writerPersistedBytes <
              workspace_->writerDeclaredBytes) {
        const bool truncated = workspace_->uploadFile.truncate(
            workspace_->writerPersistedBytes);
        syncOk = truncated && workspace_->uploadFile.sync() && syncOk;
      }
      workspace_->uploadFile.close();
      syncOk = storage_ && storage_->syncTransferDevice() && syncOk;
    }
  }
  workspace_->uploadBufferUsed = 0;
  workspace_->uploadBuffer =
      workspace_->uploadPipelineAvailable ? nullptr
                                          : workspace_->uploadBuffers[0];
  workspace_->uploadBufferIndex =
      workspace_->uploadPipelineAvailable ? 0xFFU : 0U;

  portENTER_CRITICAL(&stateMux_);
  WifiTransferPolicy::UploadTransition closed =
      WifiTransferPolicy::transitionUpload(
          uploadPhase_, WifiTransferPolicy::UploadSignal::WriterClosed);
  if (closed.disposition ==
          WifiTransferPolicy::TransitionDisposition::Applied ||
      closed.disposition ==
          WifiTransferPolicy::TransitionDisposition::NoChange) {
    uploadPhase_ = WifiTransferPolicy::UploadPhase::Incomplete;
  }
  abortWriterRequested_ = false;
  copyText(message_, sizeof(message_),
           "Upload incomplete; temporary file retained");
  portEXIT_CRITICAL(&stateMux_);

  workspace_->uploadResponseCode = 500;
  workspace_->uploadResponseReady = true;
  copyText(workspace_->uploadResponseReason,
           sizeof(workspace_->uploadResponseReason),
           "upload_incomplete");
  copyText(workspace_->uploadResponseMessage,
           sizeof(workspace_->uploadResponseMessage),
           "Upload is incomplete; the .uploading file was retained.");
  updateUploadSnapshot();
  Serial.printf(
      "WIFI XFER: upload %s path=%s reason=%s bytes=%" PRIu64
      " declared=%" PRIu64 " incomplete=yes close_sync=%s\n",
      reason && strcmp(reason, "cancelled by user") == 0
          ? "cancelled" : "failed",
      workspace_->combinedFinal,
      reason ? reason : "unknown", workspace_->writerReceivedBytes,
      workspace_->writerDeclaredBytes, syncOk ? "OK" : "FAIL");
}

bool WifiTransferModeController::finalizeUpload() {
  if (!workspace_ || !storage_) return false;

  const uint64_t finalizeStartedUs =
      static_cast<uint64_t>(esp_timer_get_time());
  if (!drainUploadPipeline(false) ||
      workspace_->writerPersistedBytes !=
          workspace_->writerDeclaredBytes) {
    closeIncompleteWriter("final buffered write failed");
    return false;
  }

  portENTER_CRITICAL(&stateMux_);
  WifiTransferPolicy::UploadTransition bodyComplete =
      WifiTransferPolicy::transitionUpload(
          uploadPhase_, WifiTransferPolicy::UploadSignal::BodyComplete);
  if (bodyComplete.disposition !=
      WifiTransferPolicy::TransitionDisposition::Applied) {
    portEXIT_CRITICAL(&stateMux_);
    closeIncompleteWriter("finalize state rejected");
    return false;
  }
  uploadPhase_ = bodyComplete.next;
  portEXIT_CRITICAL(&stateMux_);

  bool fileSyncOk = false;
  bool deviceSyncBeforeRenameOk = false;
  bool renameOk = false;
  bool deviceSyncAfterRenameOk = false;
  {
    const uint64_t lockStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    SharedSpiGuard guard;
    const uint64_t lockWaitUs =
        static_cast<uint64_t>(esp_timer_get_time()) - lockStartedUs;
    workspace_->spiLockWaitTotalUs += lockWaitUs;
    workspace_->spiLockWaitMaxUs =
        std::max(workspace_->spiLockWaitMaxUs, lockWaitUs);

    const uint64_t finalFileSyncStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    fileSyncOk =
        workspace_->uploadFile.sync() &&
        !workspace_->uploadFile.hadIoError();
    workspace_->finalFileSyncUs =
        static_cast<uint64_t>(esp_timer_get_time()) -
        finalFileSyncStartedUs;
    workspace_->fileSyncTotalUs += workspace_->finalFileSyncUs;
    workspace_->fileSyncMaxUs =
        std::max(workspace_->fileSyncMaxUs,
                 workspace_->finalFileSyncUs);
    if (fileSyncOk) workspace_->writerSyncCount++;
    workspace_->uploadFile.close();

    const uint64_t beforeRenameSyncStartedUs =
        static_cast<uint64_t>(esp_timer_get_time());
    deviceSyncBeforeRenameOk =
        fileSyncOk && storage_->syncTransferDevice();
    workspace_->deviceSyncBeforeRenameUs =
        static_cast<uint64_t>(esp_timer_get_time()) -
        beforeRenameSyncStartedUs;

    if (deviceSyncBeforeRenameOk &&
        !storage_->exists(workspace_->absoluteFinal)) {
      const uint64_t renameStartedUs =
          static_cast<uint64_t>(esp_timer_get_time());
      renameOk = storage_->renameTransferFile(
          workspace_->absoluteTemporary, workspace_->absoluteFinal);
      workspace_->renameUs =
          static_cast<uint64_t>(esp_timer_get_time()) - renameStartedUs;
    }
    if (renameOk) {
      const uint64_t afterRenameSyncStartedUs =
          static_cast<uint64_t>(esp_timer_get_time());
      deviceSyncAfterRenameOk = storage_->syncTransferDevice();
      workspace_->deviceSyncAfterRenameUs =
          static_cast<uint64_t>(esp_timer_get_time()) -
          afterRenameSyncStartedUs;
    }
  }

  if (!fileSyncOk || !deviceSyncBeforeRenameOk || !renameOk) {
    closeIncompleteWriter(
        !fileSyncOk
            ? "final file sync failed"
            : (!deviceSyncBeforeRenameOk
                   ? "device sync before rename failed"
                   : "atomic rename rejected or final exists"));
    return false;
  }

  workspace_->finalizeTotalUs =
      static_cast<uint64_t>(esp_timer_get_time()) - finalizeStartedUs;

  portENTER_CRITICAL(&stateMux_);
  WifiTransferPolicy::UploadTransition committed =
      WifiTransferPolicy::transitionUpload(
          uploadPhase_, WifiTransferPolicy::UploadSignal::CommitSucceeded);
  uploadPhase_ = committed.next;
  abortWriterRequested_ = false;
  syncCount_ = workspace_->writerSyncCount;
  copyText(message_, sizeof(message_),
           deviceSyncAfterRenameOk
               ? "Upload completed"
               : "Upload renamed; final metadata sync failed");
  portEXIT_CRITICAL(&stateMux_);

  workspace_->uploadResponseCode = deviceSyncAfterRenameOk ? 200 : 500;
  workspace_->uploadResponseReady = true;
  copyText(workspace_->uploadResponseReason,
           sizeof(workspace_->uploadResponseReason),
           deviceSyncAfterRenameOk ? "ok" : "post_rename_sync_failed");
  copyText(
      workspace_->uploadResponseMessage,
      sizeof(workspace_->uploadResponseMessage),
      deviceSyncAfterRenameOk
          ? "Upload completed and was renamed safely."
          : "The file was renamed, but final filesystem sync failed.");

  updateUploadSnapshot();
  updateStorageSnapshot();
  const uint32_t elapsed =
      elapsedSince(workspace_->writerStartedMs, millis());
  const uint64_t rate =
      elapsed ? (workspace_->writerReceivedBytes * 1000ULL) / elapsed : 0;
  const uint64_t totalUs = static_cast<uint64_t>(elapsed) * 1000ULL;
  const uint64_t accountedUs =
      workspace_->startPreparationUs + workspace_->rawGapTotalUs +
      workspace_->rawCallbackTotalUs + workspace_->finalizeTotalUs;
  const uint64_t otherUs =
      totalUs > accountedUs ? totalUs - accountedUs : 0;
  const uint64_t rawAverageBytes = workspace_->rawCallbackCount
      ? workspace_->writerReceivedBytes / workspace_->rawCallbackCount
      : 0;
  const uint64_t sdWriteRate = workspace_->sdWriteTotalUs
      ? (workspace_->sdWriteBytes * 1000000ULL) /
            workspace_->sdWriteTotalUs
      : 0;
  Serial.printf(
      "WIFI XFER: upload complete path=%s bytes=%" PRIu64
      " elapsed_ms=%lu speed_Bps=%" PRIu64
      " syncs=%lu final_sync=%s\n",
      workspace_->combinedFinal, workspace_->writerReceivedBytes,
      static_cast<unsigned long>(elapsed), rate,
      static_cast<unsigned long>(workspace_->writerSyncCount),
      deviceSyncAfterRenameOk ? "OK" : "FAIL");
  Serial.printf(
      "WIFI XFER PERF: total_ms=%lu rate_Bps=%" PRIu64
      " prepare_ms=%" PRIu64 " network_wait_ms=%" PRIu64
      " callback_work_ms=%" PRIu64 " finalize_ms=%" PRIu64
      " other_ms=%" PRIu64 "\n",
      static_cast<unsigned long>(elapsed), rate,
      workspace_->startPreparationUs / 1000ULL,
      workspace_->rawGapTotalUs / 1000ULL,
      workspace_->rawCallbackTotalUs / 1000ULL,
      workspace_->finalizeTotalUs / 1000ULL,
      otherUs / 1000ULL);
  Serial.printf(
      "WIFI XFER PERF: raw_callbacks=%lu chunk_min=%lu chunk_avg=%" PRIu64
      " chunk_max=%lu gap_max_us=%" PRIu64
      " callback_max_us=%" PRIu64 " copy_ms=%" PRIu64 "\n",
      static_cast<unsigned long>(workspace_->rawCallbackCount),
      static_cast<unsigned long>(workspace_->rawChunkMinBytes),
      rawAverageBytes,
      static_cast<unsigned long>(workspace_->rawChunkMaxBytes),
      workspace_->rawGapMaxUs, workspace_->rawCallbackMaxUs,
      workspace_->rawCopyTotalUs / 1000ULL);
  Serial.printf(
      "WIFI XFER PERF: spi_lock_ms=%" PRIu64
      " sd_write_ms=%" PRIu64 " sd_write_count=%lu"
      " sd_write_rate_Bps=%" PRIu64 " file_sync_ms=%" PRIu64
      " file_sync_count=%lu yield_ms=%" PRIu64 " yield_count=%lu\n",
      workspace_->spiLockWaitTotalUs / 1000ULL,
      workspace_->sdWriteTotalUs / 1000ULL,
      static_cast<unsigned long>(workspace_->sdWriteCount), sdWriteRate,
      workspace_->fileSyncTotalUs / 1000ULL,
      static_cast<unsigned long>(workspace_->writerSyncCount),
      workspace_->yieldTotalUs / 1000ULL,
      static_cast<unsigned long>(workspace_->yieldCount));
  Serial.printf(
      "WIFI XFER PERF: final_file_sync_ms=%" PRIu64
      " device_sync_before_ms=%" PRIu64 " rename_ms=%" PRIu64
      " device_sync_after_ms=%" PRIu64 " rssi_dbm=%ld channel=%u"
      " ap_clients=%u\n",
      workspace_->finalFileSyncUs / 1000ULL,
      workspace_->deviceSyncBeforeRenameUs / 1000ULL,
      workspace_->renameUs / 1000ULL,
      workspace_->deviceSyncAfterRenameUs / 1000ULL,
      static_cast<long>(workspace_->uploadRssiDbm),
      static_cast<unsigned>(workspace_->uploadWifiChannel),
      static_cast<unsigned>(workspace_->uploadApClients));
  Serial.printf(
      "WIFI XFER PERF: pipeline=%s buffers=%u queued_blocks=%lu"
      " queue_wait_ms=%" PRIu64 " queue_wait_max_us=%" PRIu64
      " backpressure=%lu barrier_wait_ms=%" PRIu64
      " preallocated=%s preallocate_ms=%" PRIu64 "\n",
      workspace_->uploadPipelineAvailable ? "dual" : "synchronous",
      workspace_->uploadPipelineAvailable
          ? static_cast<unsigned>(kUploadPipelineBufferCount) : 1U,
      static_cast<unsigned long>(workspace_->pipelineQueuedBlocks),
      workspace_->pipelineQueueWaitTotalUs / 1000ULL,
      workspace_->pipelineQueueWaitMaxUs,
      static_cast<unsigned long>(workspace_->pipelineBackpressureCount),
      workspace_->pipelineBarrierWaitUs / 1000ULL,
      workspace_->uploadPreallocated ? "yes" : "no",
      workspace_->preallocateUs / 1000ULL);
  return deviceSyncAfterRenameOk;
}

void WifiTransferModeController::handleUploadFinished() {
  if (!workspace_) return;
  if (!workspace_->uploadResponseReady) {
    sendJsonError(500, "missing_upload_result",
                  "Upload did not produce a terminal result.");
    return;
  }
  if (workspace_->uploadResponseCode != 200) {
    sendJsonError(workspace_->uploadResponseCode,
                  workspace_->uploadResponseReason,
                  workspace_->uploadResponseMessage);
    return;
  }

  const uint32_t elapsed =
      workspace_->writerStartedMs
          ? elapsedSince(workspace_->writerStartedMs, millis())
          : 0;
  const uint64_t rate = elapsed
      ? (workspace_->writerReceivedBytes * 1000ULL) / elapsed
      : 0;
  const uint64_t totalUs = static_cast<uint64_t>(elapsed) * 1000ULL;
  const uint64_t accountedUs =
      workspace_->startPreparationUs + workspace_->rawGapTotalUs +
      workspace_->rawCallbackTotalUs + workspace_->finalizeTotalUs;
  const uint64_t otherUs =
      totalUs > accountedUs ? totalUs - accountedUs : 0;
  const uint64_t rawAverageBytes = workspace_->rawCallbackCount
      ? workspace_->writerReceivedBytes / workspace_->rawCallbackCount
      : 0;
  const uint64_t sdWriteRate = workspace_->sdWriteTotalUs
      ? (workspace_->sdWriteBytes * 1000000ULL) /
            workspace_->sdWriteTotalUs
      : 0;

  beginChunkedJson();
  WebServer &server = *server_;
  char number[512] = {0};
  server.sendContent("{\"ok\":true");
  snprintf(number, sizeof(number),
           ",\"bytes\":\"%" PRIu64 "\",\"syncCount\":%lu",
           workspace_->writerReceivedBytes,
           static_cast<unsigned long>(workspace_->writerSyncCount));
  server.sendContent(number);
  server.sendContent(",\"performance\":{");
  snprintf(number, sizeof(number),
           "\"serverElapsedMs\":%lu,\"serverRateBps\":\"%" PRIu64
           "\",\"prepareMs\":%" PRIu64
           ",\"networkWaitMs\":%" PRIu64
           ",\"callbackWorkMs\":%" PRIu64
           ",\"finalizeMs\":%" PRIu64 ",\"otherMs\":%" PRIu64,
           static_cast<unsigned long>(elapsed), rate,
           workspace_->startPreparationUs / 1000ULL,
           workspace_->rawGapTotalUs / 1000ULL,
           workspace_->rawCallbackTotalUs / 1000ULL,
           workspace_->finalizeTotalUs / 1000ULL,
           otherUs / 1000ULL);
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"rawCallbacks\":%lu,\"rawChunkMinBytes\":%lu"
           ",\"rawChunkAverageBytes\":\"%" PRIu64
           "\",\"rawChunkMaxBytes\":%lu,\"rawGapMaxUs\":\"%" PRIu64
           "\",\"rawCallbackMaxUs\":\"%" PRIu64 "\"",
           static_cast<unsigned long>(workspace_->rawCallbackCount),
           static_cast<unsigned long>(workspace_->rawChunkMinBytes),
           rawAverageBytes,
           static_cast<unsigned long>(workspace_->rawChunkMaxBytes),
           workspace_->rawGapMaxUs, workspace_->rawCallbackMaxUs);
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"copyMs\":%" PRIu64 ",\"spiLockWaitMs\":%" PRIu64
           ",\"sdWriteMs\":%" PRIu64 ",\"sdWriteCount\":%lu"
           ",\"sdWriteBytes\":\"%" PRIu64
           "\",\"sdWriteRateBps\":\"%" PRIu64 "\"",
           workspace_->rawCopyTotalUs / 1000ULL,
           workspace_->spiLockWaitTotalUs / 1000ULL,
           workspace_->sdWriteTotalUs / 1000ULL,
           static_cast<unsigned long>(workspace_->sdWriteCount),
           workspace_->sdWriteBytes, sdWriteRate);
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"fileSyncMs\":%" PRIu64 ",\"fileSyncCount\":%lu"
           ",\"yieldMs\":%" PRIu64 ",\"yieldCount\":%lu"
           ",\"finalFileSyncMs\":%" PRIu64,
           workspace_->fileSyncTotalUs / 1000ULL,
           static_cast<unsigned long>(workspace_->writerSyncCount),
           workspace_->yieldTotalUs / 1000ULL,
           static_cast<unsigned long>(workspace_->yieldCount),
           workspace_->finalFileSyncUs / 1000ULL);
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"deviceSyncBeforeRenameMs\":%" PRIu64
           ",\"renameMs\":%" PRIu64
           ",\"deviceSyncAfterRenameMs\":%" PRIu64
           ",\"wifiRssiDbm\":%ld,\"wifiChannel\":%u"
           ",\"apClients\":%u",
           workspace_->deviceSyncBeforeRenameUs / 1000ULL,
           workspace_->renameUs / 1000ULL,
           workspace_->deviceSyncAfterRenameUs / 1000ULL,
           static_cast<long>(workspace_->uploadRssiDbm),
           static_cast<unsigned>(workspace_->uploadWifiChannel),
           static_cast<unsigned>(workspace_->uploadApClients));
  server.sendContent(number);
  snprintf(number, sizeof(number),
           ",\"pipelineEnabled\":%s,\"pipelineBuffers\":%u"
           ",\"pipelineQueuedBlocks\":%lu"
           ",\"pipelineQueueWaitMs\":%" PRIu64
           ",\"pipelineQueueWaitMaxUs\":%" PRIu64
           ",\"pipelineBackpressureCount\":%lu"
           ",\"pipelineBarrierWaitMs\":%" PRIu64
           ",\"preallocated\":%s,\"preallocateMs\":%" PRIu64 "}",
           workspace_->uploadPipelineAvailable ? "true" : "false",
           workspace_->uploadPipelineAvailable
               ? static_cast<unsigned>(kUploadPipelineBufferCount) : 1U,
           static_cast<unsigned long>(workspace_->pipelineQueuedBlocks),
           workspace_->pipelineQueueWaitTotalUs / 1000ULL,
           workspace_->pipelineQueueWaitMaxUs,
           static_cast<unsigned long>(workspace_->pipelineBackpressureCount),
           workspace_->pipelineBarrierWaitUs / 1000ULL,
           workspace_->uploadPreallocated ? "true" : "false",
           workspace_->preallocateUs / 1000ULL);
  server.sendContent(number);
  server.sendContent("}");
  endChunkedJson();
}

void WifiTransferModeController::handleCancel() {
  bool writerActive = false;
  portENTER_CRITICAL(&stateMux_);
  writerActive =
      WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
      firmwareUpdateActive_;
  if (writerActive) abortWriterRequested_ = true;
  portEXIT_CRITICAL(&stateMux_);
  sendJsonOk(writerActive ? "\"cancelling\":true"
                          : "\"cancelling\":false");
}

void WifiTransferModeController::handleFinish() {
  bool writerActive = false;
  portENTER_CRITICAL(&stateMux_);
  writerActive =
      WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
      firmwareUpdateActive_;
  if (!writerActive) {
    accepting_ = false;
    finishRequested_ = true;
    stationConnectPending_ = false;
    closeHttpRequested_ = true;
    state_ = WifiTransferServiceState::Quiescing;
    copyText(message_, sizeof(message_),
             "Finish requested; closing transfer mode");
  }
  portEXIT_CRITICAL(&stateMux_);
  if (writerActive) {
    sendJsonError(409, "writer_active",
                  "Finish or cancel the current upload first.");
    return;
  }
  Serial.println("WIFI XFER: safe finish requested by browser");
  sendJsonOk("\"finishRequested\":true");
}

void WifiTransferModeController::handleMkdir() {
  bool accepting = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting || !workspace_ || !storage_) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }
  if (!decodeOptionalBaseHeader()) {
    sendJsonError(400, "base_path_rejected",
                  "Selected folder path was rejected.");
    return;
  }

  const String encodedPath = server_->header(kHeaderPath);
  if (encodedPath.length() == 0 ||
      encodedPath.length() >
          WifiTransferPolicy::kMaxPathBytes * 3U) {
    sendJsonError(400, "folder_path_rejected",
                  "Folder path was rejected.");
    return;
  }
  WifiTransferPolicy::PathResult decoded =
      WifiTransferPolicy::normalizeRelativePath(
          encodedPath.c_str(), encodedPath.length(),
          workspace_->childFinal, sizeof(workspace_->childFinal));
  if (decoded.status != WifiTransferPolicy::PathStatus::Ok ||
      !combineRelative(
          workspace_->canonicalBase, workspace_->canonicalBaseLength,
          workspace_->childFinal, decoded.length,
          workspace_->combinedFinal, sizeof(workspace_->combinedFinal),
          workspace_->combinedFinalLength) ||
      !makeAbsolute(workspace_->combinedFinal,
                    workspace_->combinedFinalLength,
                    workspace_->absoluteFinal,
                    sizeof(workspace_->absoluteFinal))) {
    Serial.printf("WIFI XFER: path rejected reason=mkdir-policy=%s\n",
                  WifiTransferPolicy::pathStatusText(decoded.status));
    sendJsonError(400, "folder_path_rejected",
                  "Folder path was rejected.");
    return;
  }

  bool ok = false;
  bool exists = false;
  const bool parentsReady = ensureUploadParentDirectories();
  {
    SharedSpiGuard guard;
    exists = storage_->exists(workspace_->absoluteFinal);
    if (!exists && parentsReady) {
      ok = storage_->makeTransferDirectory(workspace_->absoluteFinal) &&
           storage_->syncTransferDevice();
    }
  }
  if (exists) {
    sendJsonError(409, "folder_exists",
                  "That folder already exists.");
  } else if (!ok) {
    sendJsonError(500, "mkdir_failed",
                  "Folder could not be created.");
  } else {
    noteWebActivity();
    sendJsonOk();
  }
}

void WifiTransferModeController::handleDeleteIncomplete() {
  bool accepting = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting || !workspace_ || !storage_) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }

  const String encodedPath = server_->header(kHeaderPath);
  if (encodedPath.length() == 0 ||
      encodedPath.length() >
          WifiTransferPolicy::kMaxPathBytes * 3U) {
    sendJsonError(400, "not_incomplete_upload",
                  "Only a valid .uploading file can be deleted.");
    return;
  }
  WifiTransferPolicy::PathResult decoded =
      WifiTransferPolicy::normalizeRelativePath(
          encodedPath.c_str(), encodedPath.length(),
          workspace_->combinedTemporary,
          sizeof(workspace_->combinedTemporary));
  if (decoded.status != WifiTransferPolicy::PathStatus::Ok ||
      !WifiTransferPolicy::isDeletableIncompletePath(
          workspace_->combinedTemporary, decoded.length) ||
      !makeAbsolute(workspace_->combinedTemporary, decoded.length,
                    workspace_->absoluteTemporary,
                    sizeof(workspace_->absoluteTemporary))) {
    Serial.println(
        "WIFI XFER: path rejected reason=incomplete-delete-policy");
    sendJsonError(400, "not_incomplete_upload",
                  "Only a valid .uploading file can be deleted.");
    return;
  }

  bool removed = false;
  {
    SharedSpiGuard guard;
    removed = storage_->removeIncompleteTransferFile(
                  workspace_->absoluteTemporary) &&
              storage_->syncTransferDevice();
  }
  if (!removed) {
    sendJsonError(404, "incomplete_not_removed",
                  "Incomplete file was not found or could not be removed.");
    return;
  }
  updateStorageSnapshot();
  sendJsonOk();
}

void WifiTransferModeController::handleDeleteFolder() {
  // Compatibility route for the v13.2.4 browser/API.  The v13.2.5+ page uses
  // POST /api/delete because some captive-portal and proxy paths discard
  // DELETE requests without surfacing an error to the user.
  handleDeletePath(true);
}

void WifiTransferModeController::handleDeleteEntry() {
  const String kind = server_->header(kHeaderDeleteKind);
  if (kind == "folder") {
    handleDeletePath(true);
    return;
  }
  if (kind == "file") {
    handleDeletePath(false);
    return;
  }
  sendJsonError(400, "delete_type_rejected",
                "Delete target type must be file or folder.");
}

void WifiTransferModeController::handleDeletePath(bool expectDirectory) {
  bool accepting = false;
  bool writerActive = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  writerActive = WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
                 firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting || !workspace_ || !storage_) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }
  if (writerActive) {
    sendJsonError(409, "writer_active",
                  "Finish or cancel the current upload first.");
    return;
  }
  if (server_->header(kHeaderDeleteConfirm) != "DELETE") {
    sendJsonError(412, "delete_confirmation_required",
                  "Deletion requires explicit confirmation.");
    return;
  }

  const String encodedPath = server_->header(kHeaderPath);
  if (encodedPath.length() == 0 ||
      encodedPath.length() > WifiTransferPolicy::kMaxPathBytes * 3U) {
    sendJsonError(400, "entry_path_rejected",
                  "A non-root file or folder path is required.");
    return;
  }
  WifiTransferPolicy::PathResult decoded =
      WifiTransferPolicy::normalizeExistingRelativePath(
          encodedPath.c_str(), encodedPath.length(),
          workspace_->combinedFinal, sizeof(workspace_->combinedFinal));
  if (decoded.status != WifiTransferPolicy::PathStatus::Ok ||
      decoded.length == 0U ||
      !makeAbsoluteExisting(workspace_->combinedFinal, decoded.length,
                            workspace_->absoluteFinal,
                            sizeof(workspace_->absoluteFinal))) {
    Serial.printf("WIFI XFER: delete rejected path_status=%s\n",
                  WifiTransferPolicy::pathStatusText(decoded.status));
    sendJsonError(400, "entry_path_rejected",
                  "Delete path was rejected.");
    return;
  }

  workspace_->listingEntry.close();
  workspace_->listingDirectory.close();

  Serial.printf("WIFI XFER: delete request kind=%s path=%s\n",
                expectDirectory ? "folder" : "file",
                workspace_->combinedFinal);

  if (!expectDirectory) {
    if (!WifiTransferPolicy::hasAllowedExtension(
            workspace_->combinedFinal, decoded.length) ||
        endsWithUploading(workspace_->combinedFinal)) {
      sendJsonError(400, "file_type_rejected",
                    "Only listed music and JPEG files can be deleted.");
      return;
    }

    bool removed = false;
    bool synced = false;
    bool wasFile = false;
    {
      SharedSpiGuard guard;
      workspace_->deleteEntry.close();
      workspace_->deleteEntry =
          storage_->openTransferDeleteFile(workspace_->absoluteFinal);
      wasFile = workspace_->deleteEntry &&
                !workspace_->deleteEntry.isDirectory();
      if (wasFile) {
        removed = storage_->removeTransferFileHandle(
            workspace_->deleteEntry);
      }
      if (removed) synced = storage_->syncTransferDevice();
      workspace_->deleteEntry.close();
    }
    if (!wasFile) {
      sendJsonError(404, "file_not_found",
                    "File was not found.");
      return;
    }
    if (!removed || !synced) {
      Serial.printf("WIFI XFER: file delete failed path=%s removed=%s sync=%s\n",
                    workspace_->combinedFinal,
                    removed ? "yes" : "no", synced ? "OK" : "FAIL");
      sendJsonError(500, "file_delete_failed",
                    "File could not be deleted safely.");
      return;
    }

    Serial.printf("WIFI XFER: file deleted path=%s\n",
                  workspace_->combinedFinal);
    updateStorageSnapshot();
    sendJsonOk("\"removedEntries\":1");
    return;
  }

  uint32_t scannedEntries = 0;
  uint32_t removedEntries = 0;
  bool scanned = false;
  bool removed = false;
  bool synced = false;
  {
    SharedSpiGuard guard;
    scanned = scanFolderTreeForDeletion(workspace_->absoluteFinal,
                                        scannedEntries);
    if (scanned) {
      removed = deleteFolderTree(workspace_->absoluteFinal, removedEntries);
    }
    if (removed) synced = storage_->syncTransferDevice();
  }

  if (!scanned) {
    Serial.printf(
        "WIFI XFER: folder delete preflight failed path=%s max_depth=%u "
        "max_entries=%lu\n",
        workspace_->combinedFinal,
        static_cast<unsigned>(kFolderDeleteMaxDepth),
        static_cast<unsigned long>(kFolderDeleteMaxEntries));
    sendJsonError(409, "folder_delete_preflight_failed",
                  "Folder was not found or its tree is too large to delete "
                  "safely in one request.");
    return;
  }
  if (!removed || !synced) {
    Serial.printf(
        "WIFI XFER: folder delete failed path=%s scanned=%lu removed=%lu "
        "sync=%s\n",
        workspace_->combinedFinal,
        static_cast<unsigned long>(scannedEntries),
        static_cast<unsigned long>(removedEntries),
        synced ? "OK" : "FAIL");
    sendJsonError(500, "folder_delete_failed",
                  "Folder deletion stopped before it completed. Refresh the "
                  "listing before trying again.");
    return;
  }

  Serial.printf(
      "WIFI XFER: folder deleted path=%s descendants=%lu removed=%lu\n",
      workspace_->combinedFinal,
      static_cast<unsigned long>(scannedEntries),
      static_cast<unsigned long>(removedEntries));
  updateStorageSnapshot();
  char fields[96] = {0};
  snprintf(fields, sizeof(fields),
           "\"removedEntries\":%lu",
           static_cast<unsigned long>(removedEntries));
  sendJsonOk(fields);
}

void WifiTransferModeController::handleList(bool incompleteOnly) {
  bool accepting = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting || !workspace_ || !storage_) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }

  const String path = server_->arg("path");
  size_t canonicalLength = 0;
  if (!makeAbsoluteDirectoryFromQuery(path, canonicalLength)) {
    Serial.println("WIFI XFER: path rejected reason=list-policy");
    sendJsonError(400, "folder_path_rejected",
                  "Folder path was rejected.");
    return;
  }

  uint64_t page = 0;
  const String pageText = server_->hasArg("page")
                              ? server_->arg("page")
                              : String("0");
  if (!parseUnsignedDecimal(pageText, UINT32_MAX, page)) {
    sendJsonError(400, "invalid_page", "Page number was invalid.");
    return;
  }
  if (server_->hasArg("limit")) {
    uint64_t limit = 0;
    if (!parseUnsignedDecimal(server_->arg("limit"),
                              WifiTransferPolicy::kDirectoryPageEntries,
                              limit) ||
        limit != WifiTransferPolicy::kDirectoryPageEntries) {
      sendJsonError(400, "invalid_page_size",
                    "Directory page size must be 96.");
      return;
    }
  }
  if (page >
      UINT64_MAX / WifiTransferPolicy::kDirectoryPageEntries) {
    sendJsonError(400, "page_overflow", "Page number was too large.");
    return;
  }
  const uint64_t first =
      page * WifiTransferPolicy::kDirectoryPageEntries;

  workspace_->listingDirectory.close();
  workspace_->listingEntry.close();
  {
    SharedSpiGuard guard;
    workspace_->listingDirectory =
        storage_->open(workspace_->absoluteDirectory);
  }
  if (!workspace_->listingDirectory ||
      !workspace_->listingDirectory.isDirectory()) {
    workspace_->listingDirectory.close();
    sendJsonError(404, "folder_not_found",
                  "Folder was not found.");
    return;
  }

  beginChunkedJson();
  WebServer &server = *server_;
  char number[64] = {0};
  server.sendContent("{\"ok\":true,\"path\":");
  sendJsonString(workspace_->canonicalBase);
  snprintf(number, sizeof(number), ",\"page\":%" PRIu64
           ",\"pageSize\":%lu,\"entries\":[",
           page,
           static_cast<unsigned long>(
               WifiTransferPolicy::kDirectoryPageEntries));
  server.sendContent(number);

  uint64_t matching = 0;
  uint32_t emitted = 0;
  bool hasMore = false;
  bool firstJsonEntry = true;
  for (;;) {
    bool opened = false;
    {
      SharedSpiGuard guard;
      opened = workspace_->listingDirectory.openNextFile(
          workspace_->listingEntry);
    }
    if (!opened) break;

    const char *name = workspace_->listingEntry.name();
    const size_t nameLength = strlen(name);
    WifiTransferPolicy::PathResult nameCheck =
        WifiTransferPolicy::validateExistingRelativePath(
            name, nameLength);
    bool include = nameCheck.status ==
                       WifiTransferPolicy::PathStatus::Ok &&
                   nameCheck.componentCount == 1U;
    const bool directoryEntry =
        workspace_->listingEntry.isDirectory();
    const uint64_t entrySize =
        directoryEntry ? 0 : workspace_->listingEntry.size();
    if (include) {
      if (incompleteOnly) {
        include =
            !directoryEntry &&
            WifiTransferPolicy::isDeletableIncompletePath(
                name, nameLength);
      } else {
        include =
            directoryEntry ||
            (WifiTransferPolicy::hasAllowedExtension(name, nameLength) &&
             !endsWithUploading(name));
      }
    }
    if (include) {
      size_t relativeLength = 0;
      include = combineExistingRelative(
          workspace_->canonicalBase, canonicalLength, name, nameLength,
          workspace_->combinedFinal, sizeof(workspace_->combinedFinal),
          relativeLength);
      if (include && matching++ >= first) {
        if (emitted >=
            WifiTransferPolicy::kDirectoryPageEntries) {
          hasMore = true;
          workspace_->listingEntry.close();
          break;
        }
        if (!firstJsonEntry) server.sendContent(",");
        firstJsonEntry = false;
        server.sendContent("{\"name\":");
        sendJsonString(name);
        server.sendContent(",\"path\":");
        sendJsonString(workspace_->combinedFinal);
        server.sendContent(",\"type\":");
        sendJsonString(directoryEntry ? "dir" : "file");
        snprintf(number, sizeof(number), ",\"size\":\"%" PRIu64 "\"}",
                 entrySize);
        server.sendContent(number);
        emitted++;
      }
    }
    workspace_->listingEntry.close();
    vTaskDelay(1);
  }
  workspace_->listingEntry.close();
  workspace_->listingDirectory.close();
  server.sendContent("],\"hasMore\":");
  server.sendContent(hasMore ? "true" : "false");
  server.sendContent("}");
  endChunkedJson();
}

void WifiTransferModeController::handleNetworkStatus() {
  WebServer &server = *server_;
  const String apIp = WiFi.softAPIP().toString();

  beginChunkedJson();
  server.sendContent("{\"ok\":true,\"saved\":");
  server.sendContent(savedStationSsid_[0] ? "true" : "false");
  server.sendContent(",\"connected\":");
  server.sendContent(stationConnected_ ? "true" : "false");
  server.sendContent(",\"connecting\":");
  server.sendContent(stationConnectPending_ ? "true" : "false");
  server.sendContent(",\"ssid\":");
  sendJsonString(savedStationSsid_);
  server.sendContent(",\"lanIp\":");
  sendJsonString(stationIp_);
  server.sendContent(",\"apIp\":");
  sendJsonString(apIp.c_str());
  server.sendContent(",\"host\":");
  sendJsonString(mdnsStarted_ ? "dspi-transfer.local" : "");
  server.sendContent("}");
  endChunkedJson();
}

void WifiTransferModeController::handleNetworkScan() {
  bool accepting = false;
  bool writerActive = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  writerActive = WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
                 firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);

  if (!accepting || !workspace_) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }
  if (writerActive) {
    sendJsonError(409, "writer_active",
                  "Wait for the active upload to finish first.");
    return;
  }

  setMessage("Scanning nearby Wi-Fi networks");
  Serial.println("WIFI XFER: network scan begin");

  // WebServer handles one request at a time and the browser suspends its normal
  // status polling while this endpoint runs. Keep active dwell bounded so a
  // phone using the direct AP remains responsive during the scan.
  WiFi.scanDelete();
  const int16_t found = WiFi.scanNetworks(
      false, false, false, 120U, 0);
  workspace_->scannedNetworkCount = 0;

  if (found < 0) {
    WiFi.scanDelete();
    setMessage("Wi-Fi scan failed; direct AP remains ready");
    sendJsonError(503, "wifi_scan_failed",
                  "Nearby Wi-Fi networks could not be scanned.");
    Serial.printf("WIFI XFER: network scan failed code=%d\n",
                  static_cast<int>(found));
    return;
  }

  for (int16_t index = 0; index < found; ++index) {
    const String discoveredSsid = WiFi.SSID(index);
    if (discoveredSsid.length() == 0 || discoveredSsid.length() > 32U) {
      continue;
    }

    const int32_t discoveredRssi = WiFi.RSSI(index);
    size_t existing = workspace_->scannedNetworkCount;
    for (size_t candidate = 0;
         candidate < workspace_->scannedNetworkCount; ++candidate) {
      if (strcmp(workspace_->scannedNetworks[candidate].ssid,
                 discoveredSsid.c_str()) == 0) {
        existing = candidate;
        break;
      }
    }

    if (existing < workspace_->scannedNetworkCount) {
      // Multiple access points may advertise the same SSID. Return one clear
      // choice using the strongest observed signal.
      if (discoveredRssi > workspace_->scannedNetworks[existing].rssi) {
        workspace_->scannedNetworks[existing].rssi = discoveredRssi;
        workspace_->scannedNetworks[existing].channel = WiFi.channel(index);
        workspace_->scannedNetworks[existing].secure =
            WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
      }
      continue;
    }

    if (workspace_->scannedNetworkCount >=
        Workspace::kMaximumScannedNetworks) {
      continue;
    }
    Workspace::ScannedNetwork &result =
        workspace_->scannedNetworks[workspace_->scannedNetworkCount++];
    copyText(result.ssid, sizeof(result.ssid), discoveredSsid.c_str());
    result.rssi = discoveredRssi;
    result.channel = WiFi.channel(index);
    result.secure = WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();

  // Strongest networks first. Insertion sort keeps this bounded and avoids a
  // second heap-backed collection for at most 32 results.
  for (size_t index = 1; index < workspace_->scannedNetworkCount; ++index) {
    Workspace::ScannedNetwork moving = workspace_->scannedNetworks[index];
    size_t destination = index;
    while (destination > 0 &&
           workspace_->scannedNetworks[destination - 1U].rssi < moving.rssi) {
      workspace_->scannedNetworks[destination] =
          workspace_->scannedNetworks[destination - 1U];
      --destination;
    }
    workspace_->scannedNetworks[destination] = moving;
  }

  WebServer &server = *server_;
  beginChunkedJson();
  server.sendContent("{\"ok\":true,\"networks\":[");
  char fields[96] = {0};
  for (size_t index = 0; index < workspace_->scannedNetworkCount; ++index) {
    if (index) server.sendContent(",");
    const Workspace::ScannedNetwork &network =
        workspace_->scannedNetworks[index];
    server.sendContent("{\"ssid\":");
    sendJsonString(network.ssid);
    snprintf(fields, sizeof(fields),
             ",\"rssi\":%ld,\"channel\":%ld,\"secure\":%s}",
             static_cast<long>(network.rssi),
             static_cast<long>(network.channel),
             network.secure ? "true" : "false");
    server.sendContent(fields);
  }
  server.sendContent("]}");
  endChunkedJson();
  setMessage("Wi-Fi scan complete; choose a network");
  Serial.printf("WIFI XFER: network scan complete raw=%d unique=%lu\n",
                static_cast<int>(found),
                static_cast<unsigned long>(workspace_->scannedNetworkCount));
}

void WifiTransferModeController::handleNetworkSave() {
  bool accepting = false;
  bool writerActive = false;
  portENTER_CRITICAL(&stateMux_);
  accepting = accepting_;
  writerActive = WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
                 firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);
  if (!accepting) {
    sendJsonError(503, "not_accepting",
                  "Transfer service is finishing.");
    return;
  }
  if (writerActive) {
    sendJsonError(409, "writer_active",
                  "Wait for the active upload to finish first.");
    return;
  }

  WebServer &server = *server_;
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  // IEEE 802.11 SSIDs are byte strings and leading/trailing spaces are legal.
  // Preserve the exact value selected or typed by the user.
  if (ssid.length() == 0 || ssid.length() > 32U ||
      password.length() > 63U) {
    sendJsonError(400, "invalid_wifi_credentials",
                  "Wi-Fi name or password length is invalid.");
    return;
  }

  if (!saveStationCredentials(ssid.c_str(), password.c_str())) {
    sendJsonError(500, "wifi_save_failed",
                  "Wi-Fi details could not be saved.");
    return;
  }

  beginStationConnection();
  sendJsonOk("\"saved\":true,\"connecting\":true");
  Serial.printf("WIFI XFER: saved network updated ssid=%s\n",
                savedStationSsid_);
}

void WifiTransferModeController::handleNotFound() {
  sendJsonError(404, "not_found", "API route was not found.");
}

bool WifiTransferModeController::copyCredentials(
    char *ssid, size_t ssidCapacity, char *password,
    size_t passwordCapacity, char *numericIp, size_t ipCapacity) const {
  bool available = false;
  portENTER_CRITICAL(&stateMux_);
  available = ssid_[0] && password_[0] && numericIp_[0] &&
              state_ != WifiTransferServiceState::Stopped &&
              state_ != WifiTransferServiceState::Error;
  if (available) {
    available =
        copyText(ssid, ssidCapacity, ssid_) &&
        copyText(password, passwordCapacity, password_) &&
        copyText(numericIp, ipCapacity, numericIp_);
  }
  portEXIT_CRITICAL(&stateMux_);
  if (!available) {
    if (ssid && ssidCapacity) ssid[0] = '\0';
    if (password && passwordCapacity) password[0] = '\0';
    if (numericIp && ipCapacity) numericIp[0] = '\0';
  }
  return available;
}

void WifiTransferModeController::snapshot(
    WifiTransferSnapshot &result) const {
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateMux_);
  result.state = state_;
  result.uploadPhase = uploadPhase_;
  result.active =
      state_ != WifiTransferServiceState::Stopped &&
      state_ != WifiTransferServiceState::Error;
  result.accepting = accepting_;
  result.handlingRequest = handlingRequest_;
  result.writerActive =
      WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) ||
      firmwareUpdateActive_;
  result.httpListenerClosed = httpListenerClosed_;
  result.quiescent =
      httpListenerClosed_ && !handlingRequest_ &&
      !result.writerActive && !accepting_;
  result.finishRequested = finishRequested_;
  result.autoExitRequested = autoExitRequested_;
  result.filesystemSyncRequested = filesystemSyncRequested_;
  result.filesystemSyncComplete = filesystemSyncComplete_;
  result.filesystemSyncOk = filesystemSyncOk_;
  result.networkStopped = networkStopped_;
  result.writtenBytes = writtenBytes_;
  result.declaredBytes = declaredBytes_;
  result.freeBytes = freeBytes_;
  result.totalBytes = totalBytes_;
  result.elapsedMs =
      result.writerActive && uploadStartedMs_
          ? elapsedSince(uploadStartedMs_, now)
          : uploadElapsedMs_;
  result.syncCount = syncCount_;
  copyText(result.currentFile, sizeof(result.currentFile), currentFile_);
  copyText(result.message, sizeof(result.message), message_);
  portEXIT_CRITICAL(&stateMux_);
}

bool WifiTransferModeController::active() const {
  portENTER_CRITICAL(&stateMux_);
  const bool result =
      state_ != WifiTransferServiceState::Stopped &&
      state_ != WifiTransferServiceState::Error;
  portEXIT_CRITICAL(&stateMux_);
  return result;
}

bool WifiTransferModeController::discardPreparedState() {
  bool allowed = false;
  portENTER_CRITICAL(&stateMux_);
  allowed =
      taskHandle_ == nullptr && networkStopped_ &&
      !handlingRequest_ &&
      !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
      !firmwareUpdateActive_;
  if (allowed) {
    storagePreflightValid_ = false;
    storage_ = nullptr;
    state_ = WifiTransferServiceState::Stopped;
    accepting_ = false;
    finishRequested_ = false;
    closeHttpRequested_ = false;
    httpListenerClosed_ = true;
    autoExitRequested_ = false;
    filesystemSyncRequested_ = false;
    filesystemSyncComplete_ = false;
    filesystemSyncOk_ = false;
    ssid_[0] = '\0';
    password_[0] = '\0';
    numericIp_[0] = '\0';
    currentFile_[0] = '\0';
    copyText(message_, sizeof(message_), "");
  }
  portEXIT_CRITICAL(&stateMux_);
  if (allowed) releaseWorkspace();
  return allowed;
}

bool WifiTransferModeController::finishRequested() const {
  portENTER_CRITICAL(&stateMux_);
  const bool result = finishRequested_;
  portEXIT_CRITICAL(&stateMux_);
  return result;
}

void WifiTransferModeController::requestQuiesce() {
  portENTER_CRITICAL(&stateMux_);
  accepting_ = false;
  stationConnectPending_ = false;
  closeHttpRequested_ = true;
  if (state_ == WifiTransferServiceState::Serving) {
    state_ = WifiTransferServiceState::Quiescing;
  }
  copyText(message_, sizeof(message_), "Finishing transfer safely");
  portEXIT_CRITICAL(&stateMux_);
  Serial.println("WIFI XFER: quiesce requested");
}

bool WifiTransferModeController::quiescent() const {
  portENTER_CRITICAL(&stateMux_);
  const bool result =
      httpListenerClosed_ && !accepting_ && !handlingRequest_ &&
      !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
      !firmwareUpdateActive_;
  portEXIT_CRITICAL(&stateMux_);
  return result;
}

void WifiTransferModeController::requestAbortWriter() {
  portENTER_CRITICAL(&stateMux_);
  if (WifiTransferPolicy::uploadWriterIsActive(uploadPhase_)) {
    abortWriterRequested_ = true;
  }
  portEXIT_CRITICAL(&stateMux_);
}

bool WifiTransferModeController::requestFilesystemSync() {
  bool allowed = false;
  portENTER_CRITICAL(&stateMux_);
  allowed =
      httpListenerClosed_ && !accepting_ && !handlingRequest_ &&
      !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
      !firmwareUpdateActive_ &&
      storage_ &&
      storage_->accessMode() == MediaFsAccessMode::TransferReadWrite;
  if (allowed) {
    filesystemSyncRequested_ = true;
    filesystemSyncComplete_ = false;
    filesystemSyncOk_ = false;
  }
  portEXIT_CRITICAL(&stateMux_);
  return allowed;
}

bool WifiTransferModeController::stopNetworkAfterStorageRelease() {
  bool allowed = false;
  portENTER_CRITICAL(&stateMux_);
  allowed =
      httpListenerClosed_ && !accepting_ && !handlingRequest_ &&
      !WifiTransferPolicy::uploadWriterIsActive(uploadPhase_) &&
      !firmwareUpdateActive_ &&
      storage_ &&
      storage_->accessMode() != MediaFsAccessMode::TransferReadWrite &&
      taskHandle_ != nullptr;
  if (allowed) {
    stopNetworkRequested_ = true;
    networkStopStage_ = httpListenerClosed_
                            ? NetworkStopStage::StopMdns
                            : NetworkStopStage::CloseHttp;
    state_ = WifiTransferServiceState::StoppingNetwork;
    copyText(message_, sizeof(message_), "Stopping HTTP and Wi-Fi");
  }
  portEXIT_CRITICAL(&stateMux_);
  return allowed;
}

bool WifiTransferModeController::networkStopped() const {
  portENTER_CRITICAL(&stateMux_);
  const bool result = networkStopped_;
  portEXIT_CRITICAL(&stateMux_);
  return result;
}
