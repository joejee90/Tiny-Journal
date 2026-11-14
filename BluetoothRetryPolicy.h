#pragma once
#include <stdint.h>

// A reconnect episode ends on success, manual cancellation, or three attempts.
// Starting a later episode is explicit, so failed-link callbacks cannot reset
// the budget of an attempt that never established a usable keyboard connection.
class BluetoothRetryPolicy {
public:
  static constexpr uint8_t limit = 3;
  void boot(bool enabled, bool autoConnect, bool hasKeyboard) {
    attempts_ = 0;
    active_ = enabled && autoConnect && hasKeyboard;
  }
  void reconnect(bool hasKeyboard) {
    attempts_ = 0;
    active_ = hasKeyboard;
  }
  bool takeAttempt() {
    if (!active_ || attempts_ >= limit)
      return false;
    ++attempts_;
    return true;
  }
  void connected() {
    active_ = false;
    attempts_ = 0;
  }
  void stop() { active_ = false; }
  void reset() {
    active_ = false;
    attempts_ = 0;
  }
  bool active() const { return active_; }
  bool exhausted() const { return active_ && attempts_ >= limit; }
  uint8_t attempts() const { return attempts_; }

private:
  uint8_t attempts_ = 0;
  bool active_ = false;
};
