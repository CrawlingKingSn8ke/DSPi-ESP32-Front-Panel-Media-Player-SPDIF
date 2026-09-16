#pragma once
#include <cstddef>
#include <cstring>
#include <new>

// Main/browser-owner only. Three exact query results, MRU first. No filesystem
// handles or caller-owned pointers survive a request. Allocation is optional.
template <typename Entry, typename Info>
class DirectoryPageCache {
 public:
  using Allocate = void *(*)(size_t);
  using Release = void (*)(void *);
  DirectoryPageCache(Allocate allocate, Release release)
      : allocate_(allocate), release_(release) {}
  ~DirectoryPageCache() { clear(); }
  DirectoryPageCache(const DirectoryPageCache &) = delete;
  DirectoryPageCache &operator=(const DirectoryPageCache &) = delete;

  void clear() {
    for (auto &slot : slots_) {
      if (slot) { slot->~Slot(); release_(slot); slot = nullptr; }
    }
  }

  bool copy(const char *path, size_t capacity, unsigned mode,
            const Entry *anchor, Entry *output, Info &info, size_t &count) {
    if (!valid(path, capacity, mode, anchor) || !output) return false;
    for (size_t i = 0; i < 3; ++i) {
      if (!matches(slots_[i], path, capacity, mode, anchor)) continue;
      Slot *slot = slots_[i];
      for (size_t j = 0; j < slot->count; ++j) output[j] = slot->entries[j];
      info = slot->info;
      count = slot->count;
      touch(i);
      return true;
    }
    return false;
  }

  bool remember(const char *path, size_t capacity, unsigned mode,
                const Entry *anchor, const Entry *entries, size_t count,
                const Info &info) {
    if (!valid(path, capacity, mode, anchor) || !entries || !count ||
        count > capacity) return false; // Never cache empty/error results.
    size_t index = 2;
    for (size_t i = 0; i < 3; ++i) {
      if (!slots_[i] || matches(slots_[i], path, capacity, mode, anchor)) {
        index = i; break;
      }
    }
    if (!slots_[index]) {
      void *memory = allocate_(sizeof(Slot));
      if (!memory) return false;
      slots_[index] = new (memory) Slot;
    }
    Slot &slot = *slots_[index];
    std::strcpy(slot.path, path);
    slot.capacity = capacity; slot.mode = mode; slot.count = count;
    if (mode != 0) slot.anchor = *anchor;
    slot.info = info;
    for (size_t i = 0; i < count; ++i) slot.entries[i] = entries[i];
    touch(index);
    return true;
  }

 private:
  struct Slot {
    char path[512];
    size_t capacity, count;
    unsigned mode;
    Entry anchor;
    Info info;
    Entry entries[96];
  };
  static_assert(sizeof(Slot) * 3 <= 320 * 1024,
                "Directory cache must stay within its PSRAM budget");
  static bool valid(const char *path, size_t capacity, unsigned mode,
                    const Entry *anchor) {
    return path && path[0] && std::strlen(path) < 512 && capacity &&
           capacity <= 96 && mode <= 4 && (mode == 0 || anchor);
  }
  static bool matches(const Slot *slot, const char *path, size_t capacity,
                      unsigned mode, const Entry *anchor) {
    return slot && slot->capacity == capacity && slot->mode == mode &&
        std::strcmp(slot->path, path) == 0 &&
        (mode == 0 || (slot->anchor.directory == anchor->directory &&
         slot->anchor.format == anchor->format &&
         std::strcmp(slot->anchor.path, anchor->path) == 0 &&
         std::strcmp(slot->anchor.name, anchor->name) == 0));
  }
  void touch(size_t index) {
    Slot *slot = slots_[index];
    while (index) { slots_[index] = slots_[index - 1]; --index; }
    slots_[0] = slot;
  }
  Allocate allocate_;
  Release release_;
  Slot *slots_[3] = {};
};
