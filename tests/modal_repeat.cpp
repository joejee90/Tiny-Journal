#include <cassert>
#include <cstdint>
#include <deque>
#include <array>
#include <iostream>
namespace Keyboard_Class { struct KeysState { int value=0; }; }
struct BtInputEvent { int type=0,command=0; };
static constexpr int BT_EVENT_COMMAND=1,BT_CMD_LAYOUT=2,MODE_EDITOR=0,MODE_MENU=1;
bool haveLastAction=true,keyHeld=true,btRepeatEligible=true,btKeyHeld=true,btKeyboardConnected=true;
bool injectHeldKey=true;
std::array<bool,256> btHeld{};
int btRepeatUsage=4,appMode=MODE_EDITOR;
unsigned long btKeyPressStartTime=0,btLastKeyActionTime=0;
const unsigned long KEY_REPEAT_DELAY_MS=500,KEY_REPEAT_RATE_MS=100;
BtInputEvent btRepeatEvent;
std::deque<BtInputEvent> queue;
struct Keyboard { bool isChange(){return false;} bool isPressed(){return false;} Keyboard_Class::KeysState keysState(){return {};} };
struct Cardputer { Keyboard Keyboard; } M5Cardputer;
void btKeyboardPollConnection(){}
void btProcessHidReports(){ if(injectHeldKey){btHeld[4]=true;btRepeatEligible=true;} }
bool btQueuePopEvent(BtInputEvent&e){if(queue.empty())return false;e=queue.front();queue.pop_front();return true;}
bool btQueuePushEvent(const BtInputEvent&e){queue.push_back(e);return true;}
void btClearInputEventQueue(){queue.clear();}
bool btEventToKeys(const BtInputEvent&,Keyboard_Class::KeysState&){return true;}
void cycleKeyboardLayout(){}
unsigned long millis(){return 1000;}
bool readAppKeys(Keyboard_Class::KeysState& ks) {
  btKeyboardPollConnection();
  btProcessHidReports();
  // Modal consumers own the keypress. Do not let a dialog's held key resume
  // an editor action after the dialog closes.
  haveLastAction = false;
  keyHeld = false;
  btRepeatEligible = false;
  btKeyHeld = false;
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    ks = M5Cardputer.Keyboard.keysState();
    return true;
  }
  BtInputEvent e;
  if (!btQueuePopEvent(e)) return false;
  if (e.type == BT_EVENT_COMMAND && e.command == BT_CMD_LAYOUT) {
    cycleKeyboardLayout();
    return false;
  }
  return btEventToKeys(e, ks);
}
bool anyAppKeyPressed() {
  Keyboard_Class::KeysState ks;
  bool pressed = readAppKeys(ks);
  if (pressed) btClearInputEventQueue(); // all simultaneous keys only stop audio
  return pressed;
}
void handleBtKeyRepeat() {
  if (appMode != MODE_EDITOR || !btKeyboardConnected || !btRepeatEligible || !btHeld[btRepeatUsage])
    return;
  unsigned long now = millis();
  if (!btKeyHeld) {
    if (now - btKeyPressStartTime < KEY_REPEAT_DELAY_MS) return;
    btKeyHeld = true;
  } else if (now - btLastKeyActionTime < KEY_REPEAT_RATE_MS)
    return;
  btLastKeyActionTime = now;
  btQueuePushEvent(btRepeatEvent);
}
int main(){
 Keyboard_Class::KeysState ks;
 queue.push_back({});
 assert(readAppKeys(ks));
 assert(!haveLastAction&&!keyHeld&&!btRepeatEligible&&!btKeyHeld);
 handleBtKeyRepeat();assert(queue.empty());
 queue.push_back({});queue.push_back({});
 assert(anyAppKeyPressed());assert(queue.empty());
 injectHeldKey=false;btRepeatEligible=true;btHeld[4]=false;
 handleBtKeyRepeat();assert(queue.empty());
 std::cout<<"PASS: modal input disarms both repeat sources, audio stop consumes simultaneous keys, released BLE key cannot repeat\n";
}
