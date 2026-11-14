#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <cstddef>
#include <cstring>

// History is deliberately bounded independently of the document size.
class JournalHistory {
public:
  struct State {
    std::string text;
    size_t cursor;
  };
  static constexpr size_t budget = 32768;
  void clear() {
    undo_.clear();
    redo_.clear();
  }
  bool remember(const char *text, size_t cursor) {
    // Refuse oversized snapshots before allocating a redundant document copy.
    if (std::strlen(text) > budget / 2) { clear(); return false; }
    State s{text, cursor};
    redo_.clear();
    if (!undo_.empty() && undo_.back().text == s.text) return true;
    undo_.push_back(std::move(s));
    trim();
    return true;
  }
  bool undo(const char *current, size_t cursor, State &out) {
    return move(undo_, redo_, current, cursor, out);
  }
  bool redo(const char *current, size_t cursor, State &out) {
    return move(redo_, undo_, current, cursor, out);
  }

private:
  std::deque<State> undo_, redo_;
  void trim() {
    size_t bytes = 0;
    for (auto &s : undo_)
      bytes += s.text.size() + sizeof(State);
    for (auto &s : redo_)
      bytes += s.text.size() + sizeof(State);
    while (bytes > budget && !undo_.empty()) {
      bytes -= undo_.front().text.size() + sizeof(State);
      undo_.pop_front();
    }
    while (bytes > budget && !redo_.empty()) {
      bytes -= redo_.front().text.size() + sizeof(State);
      redo_.pop_front();
    }
    while (undo_.size() > 32)
      undo_.pop_front();
    while (redo_.size() > 32)
      redo_.pop_front();
  }
  bool move(std::deque<State> &from, std::deque<State> &to, const char *current,
            size_t cursor, State &out) {
    if (from.empty())
      return false;
    to.push_back({current, cursor});
    out = std::move(from.back());
    from.pop_back();
    trim();
    return true;
  }
};
