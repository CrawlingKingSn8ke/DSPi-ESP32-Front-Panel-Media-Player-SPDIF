#pragma once

#include <Arduino.h>

// Volatile, RAM-only S/PDIF diagnostics. Logging never touches SD, NVS, flash,
// Wi-Fi, or Serial, so playback timing is not coupled to diagnostic storage.
struct SpdifDiagnosticEntry {
  uint32_t sequence = 0;
  uint32_t uptimeMs = 0;
  char message[112] = {0};
};

void spdifDiagnosticLog(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
size_t spdifDiagnosticCount();
bool spdifDiagnosticRead(size_t oldestFirstIndex,
                         SpdifDiagnosticEntry &entry);

