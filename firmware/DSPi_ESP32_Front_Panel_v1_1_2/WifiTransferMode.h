#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "MediaFs.h"
#include "WifiTransferPolicy.h"

class WebServer;

// The ESP32 Arduino WebServer stores Content-Length in a signed int.  Reject
// larger files explicitly rather than allowing that implementation detail to
// wrap or truncate a transfer.
static constexpr uint64_t WIFI_TRANSFER_MAX_FILE_BYTES = 2147483647ULL;

// Browser status polling counts as activity.  When the browser has gone away
// and no writer/request is active, ask the panel state machine to perform its
// normal safe-exit sequence after ten minutes.
static constexpr uint32_t WIFI_TRANSFER_AUTO_EXIT_IDLE_MS =
    10UL * 60UL * 1000UL;

// WebServer 3.3.11 applies its 5000 ms stream timeout while waiting for each
// raw upload chunk. A stalled/disconnected body therefore reaches RAW_ABORTED,
// is closed, and remains recoverable as .uploading.
static constexpr uint32_t WIFI_TRANSFER_STALLED_BODY_TIMEOUT_MS = 5000U;

enum class WifiTransferServiceState : uint8_t {
  Stopped = 0,
  Starting,
  Serving,
  Quiescing,
  StoppingNetwork,
  Error
};

struct WifiTransferSnapshot {
  WifiTransferServiceState state = WifiTransferServiceState::Stopped;
  WifiTransferPolicy::UploadPhase uploadPhase =
      WifiTransferPolicy::UploadPhase::Idle;
  bool active = false;
  bool accepting = false;
  bool handlingRequest = false;
  bool writerActive = false;
  bool quiescent = true;
  bool httpListenerClosed = true;
  bool finishRequested = false;
  bool autoExitRequested = false;
  bool filesystemSyncRequested = false;
  bool filesystemSyncComplete = false;
  bool filesystemSyncOk = false;
  bool networkStopped = true;
  uint64_t writtenBytes = 0;
  uint64_t declaredBytes = 0;
  uint64_t freeBytes = 0;
  uint64_t totalBytes = 0;
  uint32_t elapsedMs = 0;
  uint32_t syncCount = 0;
  char currentFile[160] = {0};
  char message[128] = {0};
};

// Exclusive AP+STA + built-in WebServer service. Construction is passive:
// the direct AP always remains available, while saved home-Wi-Fi credentials
// optionally expose the same server on the local network.
// neither Wi-Fi nor a task is initialized until start() is called.
class WifiTransferModeController {
public:
  WifiTransferModeController() = default;
  ~WifiTransferModeController() = default;

  WifiTransferModeController(const WifiTransferModeController &) = delete;
  WifiTransferModeController &operator=(
      const WifiTransferModeController &) = delete;

  // Proves that the already-mounted TransferReadWrite session really can
  // create, write, sync and remove a new file.  The probe always uses a random
  // internal *.wav.uploading name and never overwrites or formats anything.
  // It performs no Wi-Fi operation and is intended to run before BLE teardown.
  bool preflightStorage(MediaFsStorage &storage,
                        const char *transferRoot = "/",
                        char *errorText = nullptr,
                        size_t errorCapacity = 0);

  // Starts a dedicated 16 KiB transfer task.  The task (not loopTask)
  // initializes the always-available direct AP, optional saved home-Wi-Fi
  // station connection, and the HTTP server. storage must still be the same
  // singleton RW mount proven by preflightStorage(); start() will perform the
  // probe itself if the caller has not already done so.
  bool start(MediaFsStorage &storage, const char *transferRoot = "/");

  // Entry rollback before the service has been exposed to uploads.  This
  // never tears down Wi-Fi directly: it requests the same quiesce, sync,
  // storage-ownership return, and only-then network shutdown used by Finish
  // Safely.  The request is rejected while a writer or callback is active.
  bool requestStartupAbort();

  // Releases a prepared/error workspace after entry rollback.  It is rejected
  // while a task, request, writer, or network service is still active.
  bool discardPreparedState();

  // Copy credentials for the on-device screen.  Password is deliberately
  // absent from snapshots, HTTP APIs and serial telemetry.  Callers must never
  // log the returned password.
  bool copyCredentials(char *ssid, size_t ssidCapacity,
                       char *password, size_t passwordCapacity,
                       char *numericIp, size_t ipCapacity) const;

  void snapshot(WifiTransferSnapshot &result) const;
  bool active() const;
  bool finishRequested() const;

  // Stage 1 of Finish Safely. New filesystem requests are rejected and, once
  // the current response has drained, the task closes the HTTP listener so no
  // polling client can keep exit alive. This never cancels an active upload:
  // Finish is rejected until the upload completes or is explicitly cancelled.
  void requestQuiesce();
  bool quiescent() const;

  // Emergency/idempotent cancellation.  The task closes and syncs the active
  // writer at its next raw-body callback and leaves the .uploading file.
  void requestAbortWriter();

  // Optional task-owned metadata sync.  Call only after requestQuiesce() and
  // quiescent(); the main entry/exit state machine may poll the snapshot flags.
  bool requestFilesystemSync();

  // Stage after the main state machine has synced and released transfer-RW
  // ownership.  The card may already be handed back to NormalReadOnly in place
  // (preferred) or physically unmounted during recovery.  The dedicated task
  // then stops HTTP, mDNS and the Wi-Fi radio, frees its resources and deletes
  // itself.  Radio shutdown is deliberately one mode transition rather than a
  // station disconnect + AP disconnect + mode reset sequence.
  bool stopNetworkAfterStorageRelease();
  bool networkStopped() const;

private:
  struct Workspace;
  enum class NetworkStopStage : uint8_t {
    Idle = 0,
    CloseHttp,
    StopMdns,
    DisableWifi,
    Complete
  };

  static void transferTaskThunk(void *context);
  static void uploadWriterTaskThunk(void *context);
  void transferTask();
  void uploadWriterTask();
  bool allocateWorkspace();
  void releaseWorkspace();
  bool startUploadPipeline();
  void stopUploadPipeline();
  bool resetUploadPipelineForNewFile();
  bool queueCurrentUploadBuffer(bool acquireReplacement);
  bool drainUploadPipeline(bool syncAfterWrite);
  bool writeUploadBlock(const uint8_t *buffer, size_t length,
                        bool syncAfterWrite);

  bool startSoftApAndServer();
  void configureRoutes();
  void serviceControlRequests();
  void closeHttpListenerIfRequested();
  void serviceStationConnection();
  bool loadSavedStationCredentials();
  bool saveStationCredentials(const char *ssid, const char *password);
  bool beginStationConnection();
  void stopMdns();
  bool stopHttpAndWifi();

  void setHandlingRequest(bool active);
  void noteWebActivity();
  void setMessage(const char *message);
  void setError(const char *message);
  void updateUploadSnapshot();
  void updateStorageSnapshot();

  void handleIndex();
  void handleStatus();
  void handleList(bool incompleteOnly);
  void handleMkdir();
  void handlePreflight();
  void handleUploadRaw();
  void handleUploadFinished();
  void handleFirmwareRaw();
  void handleFirmwareFinished();
  void handleSpdifDiagnostics();
  void handleCancel();
  void handleDeleteIncomplete();
  void handleDeleteFolder();
  void handleDeleteEntry();
  void handleFinish();
  void handleNetworkStatus();
  void handleNetworkScan();
  void handleNetworkSave();
  void handleNotFound();

  bool planUploadFromHeaders(uint64_t &declaredBytes, uint64_t &freeBytes,
                             int &httpCode, const char *&reason);
  bool decodeOptionalBaseHeader();
  bool combineRelative(const char *base, size_t baseLength,
                       const char *child, size_t childLength,
                       char *output, size_t outputCapacity,
                       size_t &outputLength);
  bool combineExistingRelative(const char *base, size_t baseLength,
                               const char *child, size_t childLength,
                               char *output, size_t outputCapacity,
                               size_t &outputLength);
  bool makeAbsolute(const char *relative, size_t relativeLength,
                    char *output, size_t outputCapacity);
  bool makeAbsoluteExisting(const char *relative, size_t relativeLength,
                            char *output, size_t outputCapacity);
  bool makeAbsoluteDirectoryFromQuery(const String &path,
                                      size_t &canonicalLength);
  bool ensureUploadParentDirectories();
  bool pathIsDirectory(const char *absolutePath);
  void closeDeleteWorkspace();
  bool scanFolderTreeForDeletion(const char *absolutePath,
                                 uint32_t &entryCount);
  bool deleteFolderTree(const char *absolutePath,
                        uint32_t &removedEntries);
  void handleDeletePath(bool expectDirectory);
  void closeIncompleteWriter(const char *reason);
  bool flushUploadBuffer(bool syncAfterWrite);
  bool finalizeUpload();
  void resetFirmwareUpdateState();
  void failFirmwareUpdate(int code, const char *reason,
                          const char *message, bool stopClient);
  bool validateFirmwareHeader();

  void sendJsonError(int code, const char *reason, const char *message);
  void sendJsonOk(const char *extraFields = nullptr);
  void beginChunkedJson(int code = 200);
  void sendJsonString(const char *text);
  void endChunkedJson();

  static bool parseUnsignedDecimal(const String &text, uint64_t maximum,
                                   uint64_t &value);
  static bool copyText(char *destination, size_t capacity,
                       const char *source);
  static const char *stateText(WifiTransferServiceState state);

  mutable portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
  WifiTransferServiceState state_ = WifiTransferServiceState::Stopped;
  WifiTransferPolicy::UploadPhase uploadPhase_ =
      WifiTransferPolicy::UploadPhase::Idle;
  TaskHandle_t taskHandle_ = nullptr;
  MediaFsStorage *storage_ = nullptr;
  Workspace *workspace_ = nullptr;
  // WebServer route handlers capture this global-lifetime controller.  Keep
  // one server object for the firmware lifetime and only close/reopen its
  // listener between transfer sessions.  Deleting the server during Finish
  // Safely was the last line reached by hardware logs before a permanent
  // StopNetwork hang.
  WebServer *server_ = nullptr;
  bool routesConfigured_ = false;
  NetworkStopStage networkStopStage_ = NetworkStopStage::Idle;

  bool storagePreflightValid_ = false;
  bool accepting_ = false;
  bool handlingRequest_ = false;
  bool finishRequested_ = false;
  bool autoExitRequested_ = false;
  bool abortWriterRequested_ = false;
  bool filesystemSyncRequested_ = false;
  bool filesystemSyncComplete_ = false;
  bool filesystemSyncOk_ = false;
  bool stopNetworkRequested_ = false;
  bool closeHttpRequested_ = false;
  bool httpListenerClosed_ = true;
  bool networkStopped_ = true;

  uint64_t writtenBytes_ = 0;
  uint64_t declaredBytes_ = 0;
  uint64_t freeBytes_ = 0;
  uint64_t totalBytes_ = 0;
  uint32_t uploadStartedMs_ = 0;
  uint32_t uploadElapsedMs_ = 0;
  uint32_t syncCount_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint32_t stationConnectStartedMs_ = 0;
  bool stationConnectPending_ = false;
  bool stationConnected_ = false;
  bool mdnsStarted_ = false;
  char savedStationSsid_[33] = {0};
  char savedStationPassword_[65] = {0};
  char stationIp_[16] = {0};
  char ssid_[33] = {0};
  char password_[17] = {0};
  char numericIp_[16] = {0};
  char currentFile_[160] = {0};
  char message_[128] = {0};

  // Local Web OTA always writes the inactive app partition. The fixed prefix
  // contains the ESP image/first-segment headers and esp_app_desc_t; it is
  // held until validated so a filesystem/full-flash image is never passed to
  // Update as an application image.
  static constexpr size_t kFirmwareHeaderCapacity = 288U;
  bool firmwareUpdateActive_ = false;
  bool firmwareUpdateResponseReady_ = false;
  bool firmwareUpdateSucceeded_ = false;
  bool firmwareHeaderValidated_ = false;
  int firmwareUpdateResponseCode_ = 500;
  size_t firmwareHeaderUsed_ = 0;
  uint64_t firmwareDeclaredBytes_ = 0;
  uint64_t firmwareReceivedBytes_ = 0;
  uint32_t firmwareStartedMs_ = 0;
  uint32_t firmwareRebootAt_ = 0;
  uint8_t firmwareHeader_[kFirmwareHeaderCapacity] = {0};
  char firmwareResponseReason_[48] = {0};
  char firmwareResponseMessage_[128] = {0};
  char firmwareVersion_[33] = {0};
};

extern WifiTransferModeController wifiTransferMode;
