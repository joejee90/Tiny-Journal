#pragma once
#include <stddef.h>
constexpr size_t JOURNAL_MAX_TEMPLATES = 32;
constexpr size_t JOURNAL_TEMPLATE_LINE_BYTES = 512;
constexpr size_t JOURNAL_TEMPLATE_METADATA_BYTES = 4096;
enum class JournalTemplateLine { End, Line, Invalid };
// Read only the leading #@ metadata, never the template body. Fixed storage
// prevents a large SD template or metadata line exhausting heap during boot.
template <typename ReadByte>
JournalTemplateLine readJournalTemplateMetadataLine(ReadByte readByte, size_t &total,
                                                    char (&line)[JOURNAL_TEMPLATE_LINE_BYTES + 1]) {
  size_t length = 0;
  int prefix = 0;
  for (;;) {
    int value = readByte();
    if (value < -1) return JournalTemplateLine::Invalid;
    if (value >= 0 && ++total > JOURNAL_TEMPLATE_METADATA_BYTES) return JournalTemplateLine::Invalid;
    if (value < 0 || value == '\n') {
      line[length] = 0;
      return prefix == 2 ? JournalTemplateLine::Line : JournalTemplateLine::End;
    }
    if (length >= JOURNAL_TEMPLATE_LINE_BYTES || value == 0)
      return JournalTemplateLine::Invalid;
    char c = char(value);
    line[length++] = c;
    if (prefix == 0) {
      if (c == ' ' || c == '\t' || c == '\r') continue;
      if (c != '#') return JournalTemplateLine::End;
      prefix = 1;
    } else if (prefix == 1) {
      if (c != '@') return JournalTemplateLine::End;
      prefix = 2;
    }
  }
}
