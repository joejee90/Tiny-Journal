#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <vector>
namespace journal {
// Malformed bytes are individual editable units; never swallow adjacent text.
inline size_t utf8Length(const char* s, size_t n, size_t p) {
  if (p >= n) return 0;
  uint8_t a = uint8_t(s[p]);
  if (a < 128) return 1;
  size_t z = a >= 0xc2 && a <= 0xdf   ? 2
             : a >= 0xe0 && a <= 0xef ? 3
             : a >= 0xf0 && a <= 0xf4 ? 4
                                      : 1;
  if (z == 1 || p + z > n) return 1;
  for (size_t i = 1; i < z; i++)
    if ((uint8_t(s[p + i]) & 0xc0) != 0x80) return 1;
  uint8_t b = uint8_t(s[p + 1]);
  if ((a == 0xe0 && b < 0xa0) || (a == 0xed && b >= 0xa0) || (a == 0xf0 && b < 0x90) ||
      (a == 0xf4 && b >= 0x90))
    return 1;
  return z;
}
inline size_t previous(const char* s, size_t n, size_t p) {
  if (p > n) p = n;
  if (!p) return 0;
  size_t candidate = p - 1;
  while (candidate && p - candidate < 4 && (uint8_t(s[candidate]) & 0xc0) == 0x80) --candidate;
  if (candidate + utf8Length(s, n, candidate) == p) return candidate;
  return p - 1;
}
inline size_t encode(uint32_t c, char* out) {
  size_t n = 0;
  if (c < 128)
    out[n++] = char(c);
  else if (c < 0x800) {
    out[n++] = char(0xc0 | (c >> 6));
    out[n++] = char(0x80 | (c & 63));
  } else if (c < 0x10000) {
    out[n++] = char(0xe0 | (c >> 12));
    out[n++] = char(0x80 | ((c >> 6) & 63));
    out[n++] = char(0x80 | (c & 63));
  } else {
    out[n++] = char(0xf0 | (c >> 18));
    out[n++] = char(0x80 | ((c >> 12) & 63));
    out[n++] = char(0x80 | ((c >> 6) & 63));
    out[n++] = char(0x80 | (c & 63));
  }
  out[n] = 0;
  return n;
}
struct HidField {
  uint8_t report;
  uint16_t offset, count;
  uint8_t bits;
  bool variable;
  uint16_t first;
  std::vector<uint16_t> usages;
};
struct HidMap {
  std::vector<HidField> fields, ledFields;
  std::array<uint16_t, 256> sizes{}, outputSizes{};
  bool valid = false;
  bool parse(const uint8_t* d, size_t n) {
    fields.clear();
    ledFields.clear();
    outputSizes.fill(0);
    sizes.fill(0);
    valid = false;
    struct G {
      uint32_t page = 0, size = 0, count = 0, id = 0;
    } g;
    std::vector<G> stack;
    std::vector<uint16_t> usages;
    uint32_t low = 0, high = 0;
    int collections = 0;
    for (size_t p = 0; p < n;) {
      uint8_t b = d[p++];
      if (b == 0xfe) {
        if (p + 2 > n) return false;
        size_t len = d[p];
        p += 2;
        if (p + len > n) return false;
        p += len;
        continue;
      }
      size_t len = b & 3;
      if (len == 3) len = 4;
      if (p + len > n) return false;
      uint32_t v = 0;
      for (size_t i = 0; i < len; i++) v |= uint32_t(d[p++]) << (8 * i);
      uint8_t type = (b >> 2) & 3, tag = b >> 4;
      if (type == 1) {
        switch (tag) {
          case 0:
            g.page = v;
            break;
          case 7:
            g.size = v;
            break;
          case 8:
            if (!v || v > 255) return false;
            g.id = v;
            break;
          case 9:
            g.count = v;
            break;
          case 10:
            if (stack.size() >= 8) return false;
            stack.push_back(g);
            break;
          case 11:
            if (stack.empty()) return false;
            g = stack.back();
            stack.pop_back();
            break;
        }
      }
      if (type == 2) {
        if (tag == 0) {
          if (usages.size() < 256) usages.push_back(v & 0xffff);
        } else if (tag == 1)
          low = v;
        else if (tag == 2)
          high = v;
      }
      if (type == 0) {
        if (tag == 10) {
          if (++collections > 16) return false;
        }
        if (tag == 12) {
          if (--collections < 0) return false;
        }
        if (tag == 8) {
          if (g.size > 32 || g.count > 512 || sizes[g.id] + g.size * g.count > 512) return false;
          if (!(v & 1) && g.page == 7 && g.size && g.size <= 16) {
            if (fields.size() >= 64) return false;
            HidField f{uint8_t(g.id), sizes[g.id], uint16_t(g.count), uint8_t(g.size), bool(v & 2),
                       uint16_t(low), usages};
            if (f.variable && f.usages.empty() && high < low) return false;
            fields.push_back(f);
          }
          sizes[g.id] += g.size * g.count;
        }
        if (tag == 9) {
          if (g.size > 32 || g.count > 512 || outputSizes[g.id] + g.size * g.count > 512)
            return false;
          if (!(v & 1) && (v & 2) && g.page == 8 && g.size == 1) {
            if (ledFields.size() >= 16) return false;
            ledFields.push_back({uint8_t(g.id), outputSizes[g.id], uint16_t(g.count),
                                 uint8_t(g.size), true, uint16_t(low), usages});
          }
          outputSizes[g.id] += g.size * g.count;
        }
        usages.clear();
        low = high = 0;
      }
    }
    valid = !fields.empty() && collections == 0 && stack.empty();
    return valid;
  }
  static uint32_t bits(const uint8_t* d, size_t bit, uint8_t n) {
    uint32_t v = 0;
    for (uint8_t k = 0; k < n; k++)
      if (d[(bit + k) / 8] & (1 << ((bit + k) % 8))) v |= 1u << k;
    return v;
  }
  bool decode(uint8_t id, const uint8_t* d, size_t n, std::array<bool, 256>& keys) const {
    keys.fill(false);
    if (!valid || n * 8 < sizes[id]) return false;
    bool found = false;
    for (const auto& f : fields) {
      if (f.report != id) continue;
      found = true;
      for (size_t i = 0; i < f.count; i++) {
        uint32_t v = bits(d, f.offset + i * f.bits, f.bits);
        uint32_t usage = f.variable ? (i < f.usages.size() ? f.usages[i] : f.first + i) : v;
        if (f.variable && !v) continue;
        if (!f.variable && usage >= 1 && usage <= 3) return false;
        if (usage < 256 && usage) keys[usage] = true;
      }
    }
    return found;
  }
  size_t leds(uint8_t id, bool caps, uint8_t out[64]) const {
    memset(out, 0, 64);
    bool found = false;
    for (const auto& f : ledFields) {
      if (f.report != id) continue;
      found = true;
      for (size_t i = 0; i < f.count; i++) {
        uint16_t usage = i < f.usages.size() ? f.usages[i] : f.first + i;
        if (usage == 2 && caps) {
          size_t bit = f.offset + i;
          out[bit / 8] |= 1 << (bit % 8);
        }
      }
    }
    return found ? (outputSizes[id] + 7) / 8 : 0;
  }
  bool hasKeyboard(uint8_t id) const {
    for (const auto& f : fields)
      if (f.report == id) return true;
    return false;
  }
};
enum Layout { US, UK, DE, FR, ES, RU, LAYOUT_COUNT };
inline const char* layoutName(int i) {
  static const char* n[] = {"US", "UK", "DE", "FR", "ES", "RU"};
  return i >= 0 && i < LAYOUT_COUNT ? n[i] : "UK";
}
struct KeyMapper {
  bool caps = false;
  uint32_t dead = 0;
  static uint32_t upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x430 && c <= 0x44f) return c - 32;
    if (c == 0x451) return 0x401;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 32;
    return c;
  }
  static uint32_t compose(uint32_t a, uint32_t c) {
    const uint32_t accents[] = {0x27, 0x60, 0x5e, 0x22, 0x7e};
    const char* base = "aeiouAEIOU";
    const uint32_t rows[5][10] = {{0xe1, 0xe9, 0xed, 0xf3, 0xfa, 0xc1, 0xc9, 0xcd, 0xd3, 0xda},
                                  {0xe0, 0xe8, 0xec, 0xf2, 0xf9, 0xc0, 0xc8, 0xcc, 0xd2, 0xd9},
                                  {0xe2, 0xea, 0xee, 0xf4, 0xfb, 0xc2, 0xca, 0xce, 0xd4, 0xdb},
                                  {0xe4, 0xeb, 0xef, 0xf6, 0xfc, 0xc4, 0xcb, 0xcf, 0xd6, 0xdc},
                                  {0xe3, 0, 0, 0xf5, 0, 0xc3, 0, 0, 0xd5, 0}};
    if (a == '~' && c == 'n') return 0xf1;
    if (a == '~' && c == 'N') return 0xd1;
    for (int j = 0; j < 5; j++)
      if (a == accents[j])
        for (int i = 0; i < 10; i++)
          if (c == uint32_t(base[i])) return rows[j][i];
    return 0;
  }
  bool text(int layout, uint8_t key, uint8_t mods, char out[16]) {
    out[0] = 0;
    bool shift = mods & 0x22, altgr = mods & 0x40;
    if ((mods & 0x11) && !altgr) return false;
    if ((mods & 0x88)) return false;
    if (altgr) {
      uint32_t value = 0;
      if (key == 8 && layout != RU) value = 0x20ac;
      if (layout == UK) {
        if (key == 0x21) value = 0x20ac;
        if (key == 0x35) value = 0xa6;
      }
      if (layout == DE) {
        switch (key) {
          case 0x14:
            value = '@';
            break;
          case 0x10:
            value = 0xb5;
            break;
          case 0x1f:
            value = 0xb2;
            break;
          case 0x20:
            value = 0xb3;
            break;
          case 0x24:
            value = '{';
            break;
          case 0x25:
            value = '[';
            break;
          case 0x26:
            value = ']';
            break;
          case 0x27:
            value = '}';
            break;
          case 0x2d:
            value = '\\';
            break;
          case 0x30:
            value = '~';
            break;
          case 0x64:
            value = '|';
            break;
        }
      }
      if (layout == FR) {
        switch (key) {
          case 0x1f:
            value = '~';
            break;
          case 0x20:
            value = '#';
            break;
          case 0x21:
            value = '{';
            break;
          case 0x22:
            value = '[';
            break;
          case 0x23:
            value = '|';
            break;
          case 0x24:
            value = '`';
            break;
          case 0x25:
            value = '\\';
            break;
          case 0x26:
            value = '^';
            break;
          case 0x27:
            value = '@';
            break;
          case 0x2d:
            value = ']';
            break;
          case 0x2e:
            value = '}';
            break;
        }
      }
      if (layout == ES) {
        switch (key) {
          case 0x1e:
            value = '|';
            break;
          case 0x1f:
            value = '@';
            break;
          case 0x20:
            value = '#';
            break;
          case 0x21:
            value = '~';
            break;
          case 0x2f:
            value = '[';
            break;
          case 0x30:
            value = ']';
            break;
          case 0x31:
            value = '}';
            break;
          case 0x34:
            value = '{';
            break;
          case 0x35:
            value = '\\';
            break;
        }
      }
      if (!value) return false;
      encode(value, out);
      return true;
    }
    if (key == 0x39) {
      caps = !caps;
      return false;
    }
    uint32_t c = 0;
    bool isDead = false;
    if (key >= 4 && key <= 29) {
      c = 'a' + key - 4;
      if (layout == DE) {
        if (c == 'y')
          c = 'z';
        else if (c == 'z')
          c = 'y';
      }
      if (layout == FR) {
        if (c == 'a')
          c = 'q';
        else if (c == 'q')
          c = 'a';
        else if (c == 'w')
          c = 'z';
        else if (c == 'z')
          c = 'w';
        else if (c == 'm')
          c = shift ? '?' : ',';
      }
      if (layout == RU) {
        static const uint32_t ru[] = {0x444, 0x438, 0x441, 0x432, 0x443, 0x430, 0x43f, 0x440, 0x448,
                                      0x43e, 0x43b, 0x434, 0x44c, 0x442, 0x449, 0x437, 0x439, 0x43a,
                                      0x44b, 0x435, 0x433, 0x43c, 0x446, 0x447, 0x43d, 0x44f};
        c = ru[key - 4];
      }
      if (altgr) {
        if (key == 8)
          c = 0x20ac;
        else if (layout == DE && key == 20)
          c = '@';
        else if (layout == DE && key == 16)
          c = 0xb5;
        else
          return false;
      } else if (shift != caps)
        c = upper(c);
    } else if (key >= 0x1e && key <= 0x27) {
      int i = key - 0x1e;
      const char *plain = "1234567890", *s = "!@#$%^&*()";
      c = shift ? s[i] : plain[i];
      if (layout == UK && shift && i == 1) c = '"';
      if (layout == UK && shift && i == 2) c = 0xa3;
      if (layout == RU && shift) {
        const uint32_t x[] = {'!', '"', 0x2116, ';', '%', ':', '?', '*', '(', ')'};
        c = x[i];
      }
      if (layout == DE && shift) {
        const uint32_t x[] = {'!', '"', 0xa7, '$', '%', '&', '/', '(', ')', '='};
        c = x[i];
      }
      if (layout == FR) {
        const uint32_t x[] = {'&', 0xe9, '"', '\'', '(', '-', 0xe8, '_', 0xe7, 0xe0};
        c = shift ? plain[i] : x[i];
      }
      if (layout == ES && shift) {
        const uint32_t x[] = {'!', '"', 0xb7, '$', '%', '&', '/', '(', ')', '='};
        c = x[i];
      }
      if (altgr) {
        if ((layout == UK || layout == ES) && i == 3)
          c = 0x20ac;
        else if (layout == DE && i >= 6) {
          const char* x = "{[]}";
          c = x[i - 6];
        } else if (layout == FR) {
          const char* x = "~#{[|`\\^@";
          c = i < 9 ? x[i] : '}';
        } else
          return false;
      }
    } else {
      switch (key) {
        case 0x2c:
          c = ' ';
          break;
        case 0x2d:
          c = shift ? '_' : '-';
          break;
        case 0x2e:
          c = shift ? '+' : '=';
          break;
        case 0x2f:
          c = shift ? '{' : '[';
          break;
        case 0x30:
          c = shift ? '}' : ']';
          break;
        case 0x31:
        case 0x64:
          c = shift ? '|' : '\\';
          break;
        case 0x32:
          c = shift ? '~' : '#';
          break;
        case 0x33:
          c = shift ? ':' : ';';
          break;
        case 0x34:
          c = shift ? (layout == UK ? '@' : '"') : '\'';
          break;
        case 0x35:
          c = shift ? (layout == UK ? 0xac : '~') : '`';
          break;
        case 0x36:
          c = shift ? '<' : ',';
          break;
        case 0x37:
          c = shift ? '>' : '.';
          break;
        case 0x38:
          c = shift ? '?' : '/';
          break;
        default:
          if (key >= 0x59 && key <= 0x61)
            c = '1' + key - 0x59;
          else if (key == 0x62)
            c = '0';
          else if (key == 0x63)
            c = '.';
          else
            return false;
      }
      if (layout == RU) {
        switch (key) {
          case 0x2f:
            c = 0x445;
            break;
          case 0x30:
            c = 0x44a;
            break;
          case 0x33:
            c = 0x436;
            break;
          case 0x34:
            c = 0x44d;
            break;
          case 0x35:
            c = 0x451;
            break;
          case 0x36:
            c = 0x431;
            break;
          case 0x37:
            c = 0x44e;
            break;
          case 0x38:
            c = shift ? ',' : '.';
            break;
        }
        if (shift != caps) c = upper(c);
      }
      if (layout == DE) {
        switch (key) {
          case 0x2d:
            c = shift ? '?' : 0xdf;
            break;
          case 0x2e:
            c = shift ? '`' : '\'';
            isDead = true;
            break;
          case 0x2f:
            c = (shift != caps) ? 0xdc : 0xfc;
            break;
          case 0x30:
            c = shift ? '*' : '+';
            break;
          case 0x31:
          case 0x32:
            c = shift ? '\'' : '#';
            break;
          case 0x33:
            c = (shift != caps) ? 0xd6 : 0xf6;
            break;
          case 0x34:
            c = (shift != caps) ? 0xc4 : 0xe4;
            break;
          case 0x35:
            c = shift ? 0xb0 : '^';
            isDead = !shift;
            break;
          case 0x38:
            c = shift ? '_' : '-';
            break;
          case 0x64:
            c = altgr ? '|' : shift ? '>' : '<';
            break;
        }
        if (altgr && key == 0x2d) c = '\\';
      }
      if (layout == FR) {
        switch (key) {
          case 0x2d:
            c = shift ? 0xb0 : ')';
            break;
          case 0x2e:
            c = shift ? '+' : '=';
            break;
          case 0x2f:
            c = shift ? '"' : '^';
            isDead = true;
            break;
          case 0x30:
            c = shift ? 0xa3 : '$';
            break;
          case 0x33:
            c = (shift != caps) ? 'M' : 'm';
            break;
          case 0x34:
            c = shift ? '%' : 0xf9;
            break;
          case 0x35:
            c = 0xb2;
            break;
          case 0x36:
            c = shift ? '.' : ';';
            break;
          case 0x37:
            c = shift ? '/' : ':';
            break;
          case 0x38:
            c = shift ? 0xa7 : '!';
            break;
          case 0x64:
            c = shift ? '>' : '<';
            break;
        }
      }
      if (layout == ES) {
        switch (key) {
          case 0x2d:
            c = shift ? '?' : '\'';
            break;
          case 0x2e:
            c = shift ? 0xbf : 0xa1;
            break;
          case 0x2f:
            c = shift ? '^' : '`';
            isDead = true;
            break;
          case 0x30:
            c = shift ? '*' : '+';
            break;
          case 0x33:
            c = (shift != caps) ? 0xd1 : 0xf1;
            break;
          case 0x34:
            c = shift ? '"' : '\'';
            isDead = true;
            break;
          case 0x35:
            c = shift ? 0xaa : 0xba;
            break;
          case 0x38:
            c = shift ? '_' : '-';
            break;
          case 0x64:
            c = shift ? '>' : '<';
            break;
        }
      }
    }
    if (!c) return false;
    if (isDead && !altgr) {
      if (!dead) {
        dead = c;
        return false;
      }
    }
    if (dead) {
      uint32_t composed = compose(dead, c);
      size_t z = 0;
      if (c == ' ' || c == dead)
        z = encode(dead, out);
      else if (composed)
        z = encode(composed, out);
      else {
        z = encode(dead, out);
        encode(c, out + z);
      }
      dead = 0;
      return true;
    }
    encode(c, out);
    return true;
  }
};
}  // namespace journal
