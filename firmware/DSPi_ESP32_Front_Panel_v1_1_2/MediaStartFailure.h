#pragma once
#include <cstring>

// Only positively identified content-format rejections justify trying another
// track. Route, DMA, memory, SD, decoder allocation and unknown errors are shared
// failures: trying every file cannot repair them and hides the useful error.
inline bool mediaMaySkipStartFailure(const char* reason){
  return reason&&(!std::strncmp(reason,"unsupported sample rate ",24)||
    !std::strcmp(reason,"only native 44.1/48 kHz files are accepted")||
    !std::strcmp(reason,"requires mono/stereo 16/24-bit WAV/FLAC or MP3"));
}
