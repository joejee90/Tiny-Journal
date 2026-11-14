#include <cassert>
#include <iostream>

#include "../JournalInput.h"
using namespace journal;
static void expect(int layout, uint8_t key, uint8_t mods, const char* want) {
  KeyMapper m;
  char out[16];
  assert(m.text(layout, key, mods, out));
  assert(std::string(out) == want);
}
int main() {
  const uint8_t boot[] = {0x05, 1,    0x09, 6,    0xa1, 1,    0x05, 7,    0x19, 0xe0, 0x29,
                          0xe7, 0x15, 0,    0x25, 1,    0x75, 1,    0x95, 8,    0x81, 2,
                          0x95, 1,    0x75, 8,    0x81, 1,    0x95, 6,    0x75, 8,    0x15,
                          0,    0x25, 0x65, 0x19, 0,    0x29, 0x65, 0x81, 0,    0xc0};
  HidMap m;
  assert(m.parse(boot, sizeof boot));
  assert(m.sizes[0] == 64);
  std::array<bool, 256> keys;
  uint8_t packet[] = {2, 0, 4, 5, 0, 0, 0, 0};
  assert(m.decode(0, packet, 8, keys) && keys[0xe1] && keys[4] && keys[5]);
  assert(!m.decode(0, packet, 7, keys));
  packet[2] = 1;
  assert(!m.decode(0, packet, 8, keys));
  // Composite mouse report and NKRO keyboard, separate report IDs. Only keyboard subscribed.
  const uint8_t composite[] = {
      5,    1,    9,    2, 0xa1, 1,    0x85, 1,    5,    9,    0x19, 1,    0x29, 3,    0x75, 1,
      0x95, 3,    0x81, 2, 0x75, 5,    0x95, 1,    0x81, 1,    0xc0, 5,    1,    9,    6,    0xa1,
      1,    0x85, 2,    5, 7,    0x19, 0,    0x29, 0x7f, 0x75, 1,    0x95, 0x80, 0x81, 2,    0xc0};
  assert(m.parse(composite, sizeof composite));
  assert(!m.hasKeyboard(1) && m.hasKeyboard(2));
  uint8_t nkro[16]{};
  nkro[0] = 1 << 4;
  nkro[12] = 1 << 5;
  assert(m.decode(2, nkro, 16, keys) && keys[4] && keys[101]);
  assert(!m.decode(1, nkro, 16, keys));
  assert(!m.parse(composite, sizeof composite - 1));  // truncated collection rejected
  const uint8_t bad[] = {0x75, 32, 0x95, 255, 0x81, 0};
  assert(!m.parse(bad, sizeof bad));
  const uint8_t withLeds[] = {5,    7, 0x19, 0, 0x29, 0x65, 0x75, 8, 0x95, 6,
                              0x81, 0, 5,    8, 0x19, 1,    0x29, 5, 0x75, 1,
                              0x95, 5, 0x91, 2, 0x75, 3,    0x95, 1, 0x91, 1};
  assert(m.parse(withLeds, sizeof withLeds));
  uint8_t led[64];
  assert(m.leds(0, true, led) == 1 && led[0] == 2);
  assert(m.leds(0, false, led) == 1 && led[0] == 0);
  expect(US, 0x1f, 2, "@");
  expect(UK, 0x1f, 2, "\"");
  expect(UK, 0x20, 2, "£");
  expect(UK, 0x35, 2, "¬");
  expect(UK, 0x32, 2, "~");
  expect(UK, 0x64, 2, "|");
  expect(UK, 0x21, 0x40, "€");
  expect(DE, 0x1c, 0, "z");
  expect(DE, 0x1d, 0, "y");
  expect(DE, 0x2f, 0, "ü");
  expect(DE, 0x14, 0x40, "@");
  expect(DE, 0x24, 0x40, "{");
  expect(DE, 0x30, 0x40, "~");
  expect(FR, 4, 0, "q");
  expect(FR, 0x14, 0, "a");
  expect(FR, 0x10, 2, "?");
  expect(FR, 0x1f, 0, "é");
  expect(FR, 0x1f, 0x40, "~");
  expect(FR, 0x27, 0x40, "@");
  expect(ES, 0x33, 0, "ñ");
  expect(ES, 0x33, 2, "Ñ");
  expect(RU, 4, 0, "ф");
  expect(RU, 4, 2, "Ф");
  expect(RU, 0x1f, 2, "\"");
  expect(RU, 0x20, 2, "№");
  expect(RU, 0x21, 2, ";");
  expect(RU, 0x23, 2, ":");
  KeyMapper caps;
  char out[16];
  assert(!caps.text(US, 0x39, 0, out));
  assert(caps.text(US, 4, 0, out) && std::string(out) == "A");
  assert(caps.text(US, 4, 2, out) && std::string(out) == "a");
  KeyMapper decaps;
  assert(!decaps.text(DE, 0x39, 0, out));
  assert(decaps.text(DE, 0x2f, 0, out) && std::string(out) == "Ü");
  assert(decaps.text(DE, 0x2f, 2, out) && std::string(out) == "ü");
  KeyMapper dead;
  assert(!dead.text(ES, 0x34, 0, out));
  assert(dead.text(ES, 8, 0, out) && std::string(out) == "é");
  assert(!dead.text(ES, 0x34, 0, out));
  assert(dead.text(ES, 0x1b, 0, out) && std::string(out) == "'x");
  const char* valid = "a£éЖ";
  size_t n = strlen(valid), i = 0, count = 0;
  while (i < n) {
    auto next = i + utf8Length(valid, n, i);
    assert(previous(valid, n, next) == i);
    i = next;
    count++;
  }
  assert(count == 4);
  const char invalid[] = {'a', char(0x9c), 'x', char(0xe0), char(0x80), char(0x80), 0};
  assert(previous(invalid, 6, 2) == 1);
  assert(utf8Length(invalid, 6, 3) == 1);
  assert(utf8Length("\xed\xa0\x80", 3, 0) == 1);
  assert(utf8Length("\xf4\x90\x80\x80", 4, 0) == 1);
  std::cout << "HID boot/NKRO/composite, layouts, composition and malformed UTF-8 passed\n";
}
