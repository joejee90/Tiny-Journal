#pragma once
#include <cstddef>
#include <cstdint>
#include "JournalInput.h"

// Mirrors editor wrapping without allocating. stopAfter bounds hostile/imported
// files made of newlines or tabs before they can exhaust the visual-line
// vector.
inline size_t journalVisualLineCount(const char *text, size_t n, int columns,
                                     size_t stopAfter) {
  if (columns < 1)
    columns = 1;
  size_t index = 0, lineStart = 0, lastSpace = size_t(-1), count = 0;
  int col = 0;
  while (index < n) {
    uint8_t c = uint8_t(text[index]);
    if (c == '\n') {
      if (++count > stopAfter)
        return count;
      lineStart = ++index;
      col = 0;
      lastSpace = size_t(-1);
      continue;
    }
    size_t length = journal::utf8Length(text, n, index);
    int width = c == '\t' ? 4 - col % 4 : 1;
    if (c == ' ')
      lastSpace = index;
    if (col + width > columns) {
      if (++count > stopAfter)
        return count;
      if (col == 0 && width > columns) {
        index += length;
        lineStart = index;
      } else if (lastSpace != size_t(-1) && lastSpace >= lineStart) {
        index = lastSpace + 1;
        lineStart = index;
      } else if (index == lineStart) {
        index += length;
        lineStart = index;
      } else
        lineStart = index;
      col = 0;
      lastSpace = size_t(-1);
      continue;
    }
    col += width;
    index += length;
  }
  return count + 1;
}
