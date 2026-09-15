#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/ArtworkCache.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>

static bool failAllocation = false;
static unsigned allocations = 0;
static void *allocate(size_t size) {
  if (failAllocation) return nullptr;
  void *p = std::malloc(size);
  if (p) ++allocations;
  return p;
}
static void release(void *p) {
  if (p) { assert(allocations); --allocations; std::free(p); }
}

int main() {
  const uint8_t jpeg[] = {0xff, 0xd8, 1, 2, 3, 0xff, 0xd9};
  const uint8_t other[] = {0xff, 0xd8, 1, 9, 3, 0xff, 0xd9};
  const uint16_t pixels[] = {0x1234, 0xabcd, 0, 0xffff};
  uint16_t *copy = nullptr;
  uint16_t w = 0, h = 0;
  {
    ArtworkCache cache(allocate, release);
    assert(!cache.copy(jpeg, sizeof jpeg, copy, w, h));
    assert(cache.remember(jpeg, sizeof jpeg, pixels, 2, 2));
    assert(cache.copy(jpeg, sizeof jpeg, copy, w, h));
    assert(w == 2 && h == 2 && !std::memcmp(copy, pixels, sizeof pixels));
    copy[0] = 0; // The UI owns its copy; changing/freeing it cannot alter the cache.
    release(copy);
    assert(!cache.copy(other, sizeof other, copy, w, h));
    assert(!cache.copy(jpeg, sizeof jpeg - 1, copy, w, h));
    assert(cache.copy(jpeg, sizeof jpeg, copy, w, h));
    assert(copy[0] == 0x1234);
    release(copy);
    failAllocation = true;
    assert(!cache.copy(jpeg, sizeof jpeg, copy, w, h));
    assert(copy == nullptr);
    assert(!cache.remember(other, sizeof other, pixels, 2, 2));
    failAllocation = false;
    assert(!cache.copy(other, sizeof other, copy, w, h));
    assert(cache.remember(other, sizeof other, pixels, 2, 2));
    assert(!cache.copy(jpeg, sizeof jpeg, copy, w, h));
    assert(cache.copy(other, sizeof other, copy, w, h));
    release(copy);
    assert(!cache.remember(jpeg, 256U * 1024U + 1U, pixels, 2, 2));
    assert(!cache.remember(jpeg, sizeof jpeg, pixels, 65535, 65535));
    assert(!cache.remember(jpeg, sizeof jpeg, pixels, 0, 2));
    assert(!cache.remember(nullptr, sizeof jpeg, pixels, 2, 2));
    assert(!cache.copy(nullptr, sizeof jpeg, copy, w, h));
    uint8_t sameBytes[sizeof jpeg];
    std::memcpy(sameBytes, jpeg, sizeof jpeg);
    for (unsigned i = 0; i < 1000; ++i) {
      assert(cache.remember(jpeg, sizeof jpeg, pixels, 2, 2));
      assert(cache.copy(sameBytes, sizeof sameBytes, copy, w, h));
      assert(!std::memcmp(copy, pixels, sizeof pixels));
      release(copy);
      assert(allocations == 1);
    }
  }
  assert(allocations == 0);
  std::puts("Artwork cache: exact identity, independent ownership, replacement, bounds and allocation failures passed.");
}
