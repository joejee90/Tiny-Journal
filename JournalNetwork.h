#pragma once
#include "GoogleRoots.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <climits>
#include <mbedtls/md.h>

// Small helpers shared by Drive and printer configuration. No credentials are
// logged.
namespace JournalNet {
inline String urlEncode(const String &s) {
  const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      out += (char)c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
inline String hexDigest(const uint8_t *digest, size_t n) {
  const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    out += hex[digest[i] >> 4];
    out += hex[digest[i] & 15];
  }
  return out;
}
inline String sha256(const String &s) {
  uint8_t digest[32];
  if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                 (const uint8_t *)s.c_str(), s.length(), digest))
    return "";
  return hexDigest(digest, 32);
}
inline String fileHash(File &f) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0)) {
    mbedtls_md_free(&ctx);
    return "";
  }
  mbedtls_md_starts(&ctx);
  uint8_t buf[1024];
  size_t remaining = f.size();
  bool ok = true;
  while (remaining) {
    size_t n = f.read(buf, std::min(remaining, sizeof(buf)));
    if (!n) {
      ok = false;
      break;
    }
    if (mbedtls_md_update(&ctx, buf, n)) {
      ok = false;
      break;
    }
    remaining -= n;
    delay(0);
  }
  uint8_t digest[32];
  if (mbedtls_md_finish(&ctx, digest))
    ok = false;
  mbedtls_md_free(&ctx);
  f.seek(0);
  return ok ? hexDigest(digest, 32) : String("");
}
inline bool validId(const String &id) {
  if (!id.length() || id.length() > 256)
    return false;
  for (size_t i = 0; i < id.length(); ++i) {
    char c = id[i];
    if (!isalnum((unsigned char)c) && c != '_' && c != '-')
      return false;
  }
  return true;
}
// Stream multipart bodies without buffering audio recordings in RAM.
class MultipartStream : public Stream {
  const String &prefix;
  File &file;
  const String &suffix;
  size_t pos = 0;
  size_t fileSize;
  bool failed = false;

public:
  MultipartStream(const String &p, File &f, const String &s)
      : prefix(p), file(f), suffix(s), fileSize(f.size()) {}
  int available() override {
    return failed ? 0
                  : (int)std::min((size_t)INT_MAX, prefix.length() + fileSize +
                                                       suffix.length() - pos);
  }
  int read() override {
    if (!available())
      return -1;
    int c;
    if (pos < prefix.length())
      c = (uint8_t)prefix[pos];
    else if (pos < prefix.length() + fileSize) {
      c = file.read();
      if (c < 0) {
        failed = true;
        return -1;
      }
    } else
      c = (uint8_t)suffix[pos - prefix.length() - fileSize];
    ++pos;
    return c;
  }
  int peek() override {
    if (!available())
      return -1;
    if (pos < prefix.length())
      return (uint8_t)prefix[pos];
    if (pos < prefix.length() + fileSize)
      return file.peek();
    return (uint8_t)suffix[pos - prefix.length() - fileSize];
  }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}
  bool complete() const {
    return !failed && pos == prefix.length() + fileSize + suffix.length();
  }
};
} // namespace JournalNet
