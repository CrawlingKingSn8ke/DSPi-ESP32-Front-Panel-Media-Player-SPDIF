#pragma once
#include <stdint.h>

namespace PanelHomeVolume {
enum Choice : uint8_t { User = 0, Master = 1 };
inline uint8_t validChoice(uint8_t choice) { return choice <= Master ? choice : User; }
inline float minimum(uint8_t choice) { return choice == Master ? -128.0f : -60.0f; }
inline float level(uint8_t choice, bool, float user, float master) {
  return choice == Master ? master : user;
}
inline bool muted(uint8_t choice, bool, bool userMuted, float master) {
  return userMuted || (choice == Master && master <= -128.0f);
}
}
