#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
bool decodeNetworkArtwork(const uint8_t*,size_t,uint16_t*,const std::atomic<bool>&);
