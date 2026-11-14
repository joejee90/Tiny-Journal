#pragma once
#include <stddef.h>
#include <stdint.h>
namespace PrinterText {
// CP437 for UK/US receipts; unsupported characters are visible question marks.
inline uint8_t cp437(uint32_t c) {
  if (c >= 32 && c < 127)
    return (uint8_t)c;
  switch (c) {
  case 0xa3:
    return 156;
  case 0xe9:
    return 130;
  case 0xe4:
    return 132;
  case 0xe5:
    return 134;
  case 0xe7:
    return 135;
  case 0xea:
    return 136;
  case 0xeb:
    return 137;
  case 0xe8:
    return 138;
  case 0xef:
    return 139;
  case 0xee:
    return 140;
  case 0xec:
    return 141;
  case 0xc4:
    return 142;
  case 0xc5:
    return 143;
  case 0xc9:
    return 144;
  case 0xe6:
    return 145;
  case 0xc6:
    return 146;
  case 0xf4:
    return 147;
  case 0xf6:
    return 148;
  case 0xf2:
    return 149;
  case 0xfb:
    return 150;
  case 0xf9:
    return 151;
  case 0xff:
    return 152;
  case 0xd6:
    return 153;
  case 0xdc:
    return 154;
  case 0xa2:
    return 155;
  case 0xa5:
    return 157;
  case 0xe1:
    return 160;
  case 0xed:
    return 161;
  case 0xf3:
    return 162;
  case 0xfa:
    return 163;
  case 0xf1:
    return 164;
  case 0xd1:
    return 165;
  case 0xdf:
    return 225;
  case 0xfc:
    return 129;
  case 0xc7:
    return 128;
  case 0x2018:
  case 0x2019:
    return '\'';
  case 0x201c:
  case 0x201d:
    return '"';
  case 0x2013:
  case 0x2014:
    return '-';
  default:
    return '?';
  }
}
inline uint32_t next(const char *s, size_t size, size_t &i) {
  uint8_t c = (uint8_t)s[i++];
  if (c < 128)
    return c;
  unsigned n = c >= 0xc2 && c <= 0xdf   ? 1
               : c >= 0xe0 && c <= 0xef ? 2
               : c >= 0xf0 && c <= 0xf4 ? 3
                                        : 0;
  if (!n || i + n > size)
    return 0xfffd;
  uint32_t v = c & ((1u << (6 - n)) - 1);
  size_t start = i;
  for (unsigned j = 0; j < n; ++j) {
    uint8_t b = s[i];
    if ((b & 0xc0) != 0x80) {
      i = start;
      return 0xfffd;
    }
    v = (v << 6) | (b & 63);
    ++i;
  }
  if ((n == 1 && v < 128) || (n == 2 && v < 2048) || (n == 3 && v < 65536) ||
      v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
    return 0xfffd;
  return v;
}
} // namespace PrinterText
