#pragma once

#include <cstdint>

struct UiPagedWindow {
  uint8_t first;
  uint8_t end;
  bool below;
};

constexpr UiPagedWindow uiPagedWindow(uint8_t count, uint8_t selected,
                                      uint8_t visibleRows, uint8_t currentFirst)
{
  if (!count || !visibleRows) return {0, 0, false};
  const uint8_t last = selected < count ? selected : count - 1;
  const uint8_t maxFirst = count > visibleRows ? count - visibleRows : 0;
  uint8_t first = currentFirst < maxFirst ? currentFirst : maxFirst;
  if (last < first) first = last;
  if (static_cast<unsigned>(last) >= static_cast<unsigned>(first) + visibleRows)
    first = last - visibleRows + 1;
  const unsigned end = static_cast<unsigned>(first) + visibleRows;
  const uint8_t boundedEnd = end < count ? static_cast<uint8_t>(end) : count;
  return {first, boundedEnd, boundedEnd < count};
}

constexpr bool uiShowDownArrow(bool below, bool toastVisible)
{
  return below && !toastVisible;
}
