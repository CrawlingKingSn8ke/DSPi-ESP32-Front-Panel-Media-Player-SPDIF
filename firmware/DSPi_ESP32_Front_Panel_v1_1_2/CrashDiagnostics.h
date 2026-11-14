#pragma once
#include <cstddef>
namespace CrashDiagnostics {
// Read-only, called only by the exclusive transfer portal after media stops.
// No raw task/heap memory or stored credentials are exported.
bool formatJson(char* output,size_t capacity);
}
