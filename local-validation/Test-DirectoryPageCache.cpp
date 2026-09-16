#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/DirectoryPageCache.h"
struct Entry { char path[512] = {}; char name[512] = {}; bool directory = false; int format = 0; };
struct Info { unsigned total = 0; bool next = false; };
static int allocations = 0;
static bool failAllocation = false;
void *allocate(size_t n) { if (failAllocation) return nullptr; ++allocations; return std::malloc(n); }
void release(void *p) { if (p) { --allocations; std::free(p); } }
int main() {
  {
    DirectoryPageCache<Entry, Info> cache(allocate, release);
    Entry input[2]; std::strcpy(input[0].path, "/album/one.flac");
    std::strcpy(input[0].name, "one.flac"); input[1] = input[0];
    Entry out[2], anchor = input[0]; Info info{42, true}, got; size_t count = 0;
    assert(!cache.copy("/album", 2, 0, nullptr, out, got, count));
    assert(cache.remember("/album", 2, 0, nullptr, input, 2, info));
    assert(cache.copy("/album", 2, 0, nullptr, out, got, count));
    assert(count == 2 && got.total == 42 && got.next);
    assert(std::strcmp(out[0].path, "/album/one.flac") == 0);
    out[0].path[0] = 'X';
    assert(cache.copy("/album", 2, 0, nullptr, out, got, count) && out[0].path[0] == '/');
    assert(!cache.copy("/Album", 2, 0, nullptr, out, got, count));
    assert(!cache.copy("/album", 1, 0, nullptr, out, got, count));
    assert(!cache.copy("/album", 2, 1, &anchor, out, got, count));
    assert(cache.remember("/album", 2, 1, &anchor, input, 1, info));
    assert(cache.copy("/album", 2, 1, &anchor, out, got, count) && count == 1);
    anchor.directory = true;
    assert(!cache.copy("/album", 2, 1, &anchor, out, got, count));
    anchor.directory = false; anchor.name[0] = 'X';
    assert(!cache.copy("/album", 2, 1, &anchor, out, got, count));
    assert(cache.remember("/third", 2, 0, nullptr, input, 2, info));
    assert(allocations == 3);
    assert(cache.copy("/album", 2, 0, nullptr, out, got, count));
    assert(cache.remember("/fourth", 2, 0, nullptr, input, 2, info));
    assert(allocations == 3);
    assert(cache.copy("/album", 2, 0, nullptr, out, got, count));
    anchor = input[0];
    assert(!cache.copy("/album", 2, 1, &anchor, out, got, count));
    assert(!cache.remember("/empty", 2, 0, nullptr, input, 0, info));
    assert(!cache.remember("/oversize", 97, 0, nullptr, input, 2, info));
    assert(!cache.remember("/invalid", 1, 0, nullptr, input, 2, info));
    assert(!cache.remember("/invalid", 2, 1, nullptr, input, 2, info));
    assert(!cache.remember("/invalid", 2, 5, &anchor, input, 2, info));
    char longPath[513]; std::memset(longPath, 'a', 512); longPath[512] = 0;
    assert(!cache.remember(longPath, 2, 0, nullptr, input, 2, info));
    cache.clear(); assert(allocations == 0);
    assert(!cache.copy("/album", 2, 0, nullptr, out, got, count));
    for (unsigned mode = 1; mode <= 4; ++mode) {
      assert(cache.remember("/album", 2, mode, &anchor, input, 2, info));
      assert(cache.copy("/album", 2, mode, &anchor, out, got, count));
      Entry changed = anchor; changed.path[0] = 'X';
      assert(!cache.copy("/album", 2, mode, &changed, out, got, count));
      changed = anchor; changed.format = 9;
      assert(!cache.copy("/album", 2, mode, &changed, out, got, count));
      cache.clear();
    }
    assert(cache.remember("/retained", 2, 0, nullptr, input, 2, info));
    failAllocation = true;
    assert(!cache.remember("/failed", 2, 0, nullptr, input, 2, info));
    assert(cache.copy("/retained", 2, 0, nullptr, out, got, count));
    cache.clear();
    failAllocation = true;
    assert(!cache.remember("/album", 2, 0, nullptr, input, 2, info));
    failAllocation = false;
    Entry full[96], fullOut[96];
    std::strcpy(full[95].name, "last.flac");
    assert(cache.remember("/full", 96, 4, &anchor, full, 96, info));
    assert(cache.copy("/full", 96, 4, &anchor, fullOut, got, count));
    assert(count == 96 && std::strcmp(fullOut[95].name, "last.flac") == 0);
    cache.clear();
    for (int i = 0; i < 1000; ++i) {
      assert(cache.remember("/album", 2, 0, nullptr, input, 2, info));
      assert(cache.copy("/album", 2, 0, nullptr, out, got, count));
      cache.clear();
    }
  }
  assert(allocations == 0);
  std::puts("Directory page cache behaviour passed");
}
