#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace journalguide {
constexpr size_t MAX_BYTES = 65536;
constexpr size_t LINE_BYTES = 256;
constexpr size_t MAX_PAGES = 1024;
constexpr char MARKER[] = "Tiny Journal generated user guide | V. ";
inline bool generatedMarker(const char *line) {
  const size_t prefix = sizeof(MARKER) - 1;
  if (strncmp(line, MARKER, prefix)) return false;
  bool digit = false;
  size_t count = 0;
  for (const char *p = line + prefix; *p; ++p) {
    if (++count > 31) return false;
    if (*p >= '0' && *p <= '9') digit = true;
    else if (*p == '.' && digit) digit = false;
    else return false;
  }
  return digit;
}
// A target and all replacement/recovery sidecars must belong to the generated
// guide namespace before the checked writer may touch them.
inline bool safeTarget(bool mainExists, bool mainGenerated, bool backupExists,
                       bool backupGenerated, bool tempExists, bool tempGenerated) {
  return (!mainExists || mainGenerated) && (!backupExists || backupGenerated) &&
         (!tempExists || tempGenerated);
}
// Read one bounded visual line, without splitting a valid UTF-8 sequence. The
// reader provides read/position/seek; measure returns the rendered pixel width.
template<class Reader, class Measure>
bool nextLine(Reader &source, char *line, size_t capacity, int maxWidth, Measure measure) {
  if (capacity < 5) return false;
  size_t length = 0;
  line[0] = 0;
  for (;;) {
    uint32_t start = source.position();
    int first = source.read();
    if (first < 0) return length != 0;
    if (first == '\r') continue;
    if (first == '\n') return true;
    unsigned char c = first;
    char unit[5] = {char(c), 0, 0, 0, 0};
    unsigned bytes = c < 128 ? 1 : (c >= 0xc2 && c <= 0xdf ? 2 : (c >= 0xe0 && c <= 0xef ? 3 : (c >= 0xf0 && c <= 0xf4 ? 4 : 1)));
    bool valid = c < 128 || bytes > 1;
    for (unsigned i = 1; i < bytes; ++i) {
      int next = source.read();
      if (next < 0 || (next & 0xc0) != 0x80) { valid = false; source.seek(start + 1); break; }
      unit[i] = char(next);
    }
    if (!valid) { unit[0] = '?'; bytes = 1; }
    if (length + bytes >= capacity) { source.seek(start); return true; }
    memcpy(line + length, unit, bytes);
    line[length + bytes] = 0;
    if (length && measure(line) > maxWidth) {
      line[length] = 0;
      source.seek(start);
      return true;
    }
    length += bytes;
  }
}
} // namespace journalguide
