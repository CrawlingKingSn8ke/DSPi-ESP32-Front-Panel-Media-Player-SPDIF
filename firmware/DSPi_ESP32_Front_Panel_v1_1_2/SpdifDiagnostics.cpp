#include "SpdifDiagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <freertos/FreeRTOS.h>

namespace {

constexpr size_t kEntryCapacity = 128;
SpdifDiagnosticEntry entries[kEntryCapacity];
portMUX_TYPE diagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t nextSequence = 1;
size_t entryCount = 0;
size_t nextEntry = 0;

}  // namespace

void spdifDiagnosticLog(const char *format, ...)
{
  if (!format) return;

  char message[112] = {0};
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  const uint32_t uptimeMs = millis();

  portENTER_CRITICAL(&diagnosticsMux);
  SpdifDiagnosticEntry &entry = entries[nextEntry];
  entry.sequence = nextSequence++;
  entry.uptimeMs = uptimeMs;
  memcpy(entry.message, message, sizeof(entry.message));
  entry.message[sizeof(entry.message) - 1] = '\0';
  nextEntry = (nextEntry + 1) % kEntryCapacity;
  if (entryCount < kEntryCapacity) ++entryCount;
  portEXIT_CRITICAL(&diagnosticsMux);
}

size_t spdifDiagnosticCount()
{
  portENTER_CRITICAL(&diagnosticsMux);
  const size_t count = entryCount;
  portEXIT_CRITICAL(&diagnosticsMux);
  return count;
}

bool spdifDiagnosticRead(size_t oldestFirstIndex,
                         SpdifDiagnosticEntry &entry)
{
  portENTER_CRITICAL(&diagnosticsMux);
  if (oldestFirstIndex >= entryCount) {
    portEXIT_CRITICAL(&diagnosticsMux);
    return false;
  }
  const size_t oldest = (nextEntry + kEntryCapacity - entryCount) %
                        kEntryCapacity;
  entry = entries[(oldest + oldestFirstIndex) % kEntryCapacity];
  portEXIT_CRITICAL(&diagnosticsMux);
  return true;
}
