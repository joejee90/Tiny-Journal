#include "../BluetoothRetryPolicy.h"
#include <cassert>
#include <iostream>

int main() {
  for (bool enabled : {false, true})
    for (bool automatic : {false, true})
      for (bool saved : {false, true}) {
        BluetoothRetryPolicy p;
        p.boot(enabled, automatic, saved);
        assert(p.active() == (enabled && automatic && saved));
        assert(p.takeAttempt() == (enabled && automatic && saved));
      }
  BluetoothRetryPolicy p;
  p.boot(true, true, true);
  assert(p.takeAttempt() && p.attempts() == 1 && !p.exhausted());
  assert(p.takeAttempt() && p.attempts() == 2 && !p.exhausted());
  assert(p.takeAttempt() && p.attempts() == 3 && p.exhausted());
  assert(!p.takeAttempt());
  assert(!p.takeAttempt()); // merely observing a failed disconnect cannot reset it
  p.stop(); assert(!p.takeAttempt() && !p.active());
  p.reconnect(true); assert(p.takeAttempt());
  p.connected(); assert(!p.active() && p.attempts() == 0 && !p.takeAttempt());
  p.reconnect(true); assert(p.takeAttempt()); // a new usable connection may later drop
  p.reconnect(false); assert(!p.takeAttempt());
  p.boot(true,true,true); assert(p.takeAttempt()); p.reset(); assert(p.attempts()==0 && !p.active());
  std::cout << "Bluetooth boot settings and exactly-three-attempt retry budget passed\n";
}
