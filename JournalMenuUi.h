#pragma once
#include <algorithm>
#include <cstdint>
namespace journalui {
struct ScrollThumb { int top, height; bool visible; };
inline ScrollThumb scrollThumb(int count, int first, int rows, int trackHeight) {
  if (rows < 1 || count <= rows || trackHeight < 1) return {0, 0, false};
  first = std::max(0, std::min(first, count - rows));
  int size = std::min(trackHeight, std::max(8, int(int64_t(trackHeight) * rows / count)));
  return {int(int64_t(trackHeight - size) * first / (count - rows)), size, true};
}
inline int marqueeOffset(uint32_t elapsed, int overflow) {
  if (overflow <= 0) return 0;
  uint32_t travel = (uint32_t(overflow) * 1000 + 29) / 30;
  uint32_t phase = elapsed % (2 * travel + 1800);
  if (phase < 900) return 0;
  phase -= 900;
  if (phase < travel) return std::min(overflow, int(uint64_t(phase) * 30 / 1000));
  phase -= travel;
  if (phase < 900) return overflow;
  phase -= 900;
  return std::max(0, overflow - int(uint64_t(phase) * 30 / 1000));
}
}
