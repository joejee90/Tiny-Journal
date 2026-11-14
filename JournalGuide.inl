#include "JournalGuide.h"
#include "JournalGuideText.h"

static const char *journalGuidePath = nullptr;

static bool guideFileIsGenerated(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) { if (file) file.close(); return false; }
  char marker[96]{};
  size_t used = 0;
  bool newline = false;
  while (file.available() && used + 1 < sizeof(marker)) {
    int c = file.read();
    if (c == '\n') { newline = true; break; }
    if (c < 0 || c == 0 || c == '\r') break;
    marker[used++] = char(c);
  }
  file.close();
  return newline && journalguide::generatedMarker(marker);
}

static bool guideTargetIsSafe(const char *name) {
  String path(name), backup = path + ".bak", temp = path + ".tmp";
  bool a = SD.exists(path), b = SD.exists(backup), t = SD.exists(temp);
  return journalguide::safeTarget(a, !a || guideFileIsGenerated(path),
      b, !b || guideFileIsGenerated(backup), t, !t || guideFileIsGenerated(temp));
}

static bool guideMatchesFirmware(const char *path) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.size() != TJ_GUIDE_LENGTH) { if (file) file.close(); return false; }
  uint8_t chunk[256];
  size_t total = 0;
  bool matches = true;
  while (total < TJ_GUIDE_LENGTH) {
    size_t size = std::min(sizeof(chunk), TJ_GUIDE_LENGTH - total);
    if (file.read(chunk, size) != size || memcmp(chunk, TJ_GUIDE_TEXT + total, size)) { matches = false; break; }
    total += size;
  }
  file.close();
  return matches;
}

void ensureUserGuide() {
  journalGuidePath = nullptr;
  const char *paths[] = {"/journal/README.txt", "/journal/Tiny-Journal-README.txt"};
  for (const char *path : paths) {
    if (!guideTargetIsSafe(path)) continue;
    if (guideMatchesFirmware(path) || writeFileSafely(path, reinterpret_cast<const uint8_t *>(TJ_GUIDE_TEXT), TJ_GUIDE_LENGTH)) {
      journalGuidePath = path;
      return;
    }
    // A read-only or failing SD card must not prevent access to the flash copy.
    break;
  }
  showStatusMessage("Guide available; SD copy not installed", 3500);
}

struct JournalGuideSource {
  File file;
  bool embedded = true, failed = false;
  uint32_t offset = 0, length = TJ_GUIDE_LENGTH;
  int read() {
    if (offset >= length) return -1;
    int c = embedded ? uint8_t(TJ_GUIDE_TEXT[offset]) : file.read();
    if (c >= 0) ++offset;
    else failed = true;
    return c;
  }
  uint32_t position() const { return offset; }
  bool seek(uint32_t value) {
    if (value > length || (!embedded && !file.seek(value))) { failed = true; return false; }
    offset = value;
    return true;
  }
};

void showUserGuide() {
  stopUiMarquee();
  JournalGuideSource source;
  if (journalGuidePath) {
    source.file = SD.open(journalGuidePath, FILE_READ);
    if (source.file && source.file.size() <= journalguide::MAX_BYTES && !source.file.isDirectory()) {
      source.embedded = false;
      source.length = source.file.size();
    } else if (source.file) source.file.close();
  }
  std::unique_ptr<uint32_t[]> offsets(new (std::nothrow) uint32_t[journalguide::MAX_PAGES]);
  if (!offsets) { if (source.file) source.file.close(); showStatusMessage("Guide: not enough memory", 3000); return; }
  offsets[0] = 0;
  size_t page = 0;
  bool running = true;
  while (running) {
    applyUiTextSize();
    M5Cardputer.Display.fillScreen(editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(3, 0);
    M5Cardputer.Display.print(fitUiLabel("User guide - page " + String(page + 1), M5Cardputer.Display.width() - 6));
    if (!source.seek(offsets[page])) break;
    char line[journalguide::LINE_BYTES];
    auto measure = [](const char *text) { return M5Cardputer.Display.textWidth(text); };
    for (int row = 0; row < listVisibleLines; ++row) {
      if (!journalguide::nextLine(source, line, sizeof(line), M5Cardputer.Display.width() - 6, measure)) break;
      M5Cardputer.Display.setCursor(3, topBarHeight + row * lineHeight);
      M5Cardputer.Display.print(line);
    }
    if (source.failed) break;
    uint32_t next = source.position();
    bool more = next < source.length && next > offsets[page];
    drawUiFooter(more ? "Up/Down: page Enter/Esc: back" : "End - Up: page Enter/Esc: back");
    bool changed = false;
    while (!changed && running) {
      M5Cardputer.update(); updateRTC(); updateBatteryStatus();
      Keyboard_Class::KeysState keys;
      if (readAppKeys(keys)) {
        if (keys.enter || keys.del) { running = false; break; }
        for (char c : keys.word) {
          if (c == 27 || c == '`') { running = false; break; }
          if ((c == ';' || c == 'w' || c == 'W') && page) { --page; changed = true; break; }
          if ((c == '.' || c == 's' || c == 'S') && more) {
            if (page + 1 < journalguide::MAX_PAGES) { offsets[++page] = next; changed = true; }
            else { drawUiFooter("Guide page limit - Enter: back"); }
            break;
          }
        }
      }
      delay(10);
    }
  }
  if (source.file) source.file.close();
  haveLastAction = keyHeld = false;
  if (source.failed) showStatusMessage("Guide read failed", 3000);
  needsRedraw = true;
}
