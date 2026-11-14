#include "BluetoothRetryPolicy.h"
#include <algorithm>
#include <atomic>
// BLE transport is independent of the application input events. Callbacks only
// enqueue bytes or pairing state; SD/display/UI changes happen on the main
// task.
enum BtKeyboardLayout {
  BT_LAYOUT_US,
  BT_LAYOUT_UK,
  BT_LAYOUT_DE,
  BT_LAYOUT_FR,
  BT_LAYOUT_ES,
  BT_LAYOUT_RU,
  BT_LAYOUT_COUNT
};
enum BtSetupState {
  BT_STATE_IDLE,
  BT_STATE_SCANNING,
  BT_STATE_PICKER,
  BT_STATE_CONNECTING,
  BT_STATE_CONNECTED
};
BtKeyboardLayout btKeyboardLayout = BT_LAYOUT_UK;
BtSetupState btSetupState = BT_STATE_IDLE;
bool btKeyboardEnabled = false, btKeyboardConnected = false;
static bool btEnabledPreference = true, btPowerOnAutoConnect = true;
static bool btShutdownPending = false, btAutomaticAttempt = false;
static BluetoothRetryPolicy btRetryPolicy;
static int btMenuIndex = 0;
String btKeyboardName, btKeyboardAddress, btKeyboardSavedAddress,
    btKeyboardSavedName, btKeyboardLastError, btAuthDebug;
static uint8_t btSavedAddressType = 0;
static bool btInitDone = false, btConfigLoaded = false, btWanted = false,
            btAutoScan = false, btReconnectDirect = false;
static int btSelectedDevice = 0;
static unsigned long btRetryAt = 0, btPairStarted = 0;
static std::atomic<bool> btConnectBusy{false}, btLinkLost{false};
static std::atomic<int> btConnectResult{0};
static std::atomic<uint8_t> btPairKind{0};
static volatile uint32_t btPairPin = 0;
static NimBLEConnInfo *btPairInfo = nullptr;
static String btPairDigits;
static NimBLEClient *btKeyboardClient = nullptr;
static const NimBLEAdvertisedDevice *btKeyboardAdvDevice = nullptr;
static std::vector<const NimBLEAdvertisedDevice *> btScanDevices;
static journal::HidMap btReportMap;
static NimBLERemoteCharacteristic *btLedChar = nullptr;
static uint8_t btLedReportID = 0;
static bool btLedBoot = false, btLedDirty = false;
static journal::KeyMapper btTextMapper, builtInTextMapper;
static std::array<bool, 256> btHeld{};
struct BtReportSource {
  NimBLERemoteCharacteristic *chr;
  uint8_t id;
  bool boot;
  std::array<bool, 256> held;
};
static std::vector<BtReportSource> btSources;
struct BtHidReport {
  uint8_t data[64];
  uint8_t length;
  NimBLERemoteCharacteristic *chr;
};
static QueueHandle_t btHidReportQueue = nullptr;
static volatile uint32_t btReportCount = 0, btDroppedReportCount = 0;
static bool btAnyKeyDown = false, btRepeatEligible = false, btKeyHeld = false;
static unsigned long btKeyPressStartTime = 0, btLastKeyActionTime = 0;
static BtInputEvent btRepeatEvent;
static uint8_t btRepeatUsage = 0;
void btStartConnect();
void btRequestPowerOff(bool persist);
void btFinishPowerOff();
void btEnableKeyboard();
int uiFooterHeight();
void drawUiFooter(const String &text);
void drawUiScrollbar(int total, int first, int visible, int topY, int height);
void drawUiListLabel(const String &label, int x, int y, int widthPx,
                     bool selected);
int uiListLabelWidth(int total, int visible);
void btKeyboardPollConnection();
void applyUiTextSize();
String fitUiLabel(const String &text, int widthPx);
void handlePrinterSetupKeys(const Keyboard_Class::KeysState &ks);
void journalUndo();
void journalRedo();
void journalFind();

void btClearInputEventQueue() { btEventHead = btEventTail = 0; }
bool btQueuePushEvent(const BtInputEvent &e) {
  int next = (btEventHead + 1) % BT_INPUT_EVENT_QUEUE_SIZE;
  if (next == btEventTail)
    return false;
  btEventQueue[btEventHead] = e;
  btEventHead = next;
  return true;
}
bool btQueuePopEvent(BtInputEvent &e) {
  if (btEventHead == btEventTail)
    return false;
  e = btEventQueue[btEventTail];
  btEventTail = (btEventTail + 1) % BT_INPUT_EVENT_QUEUE_SIZE;
  return true;
}
bool btTextEvent(BtInputEvent &e, const char *s) {
  e.type = BT_EVENT_TEXT;
  strncpy(e.text, s, sizeof(e.text) - 1);
  e.text[sizeof(e.text) - 1] = 0;
  return true;
}
bool btCommandEvent(BtInputEvent &e, uint8_t c) {
  e.type = BT_EVENT_COMMAND;
  e.command = c;
  return true;
}
void loadBluetoothKeyboardConfigFromSD() {
  if (btConfigLoaded)
    return;
  btConfigLoaded = true;
  File f = SD.open(BT_KEYBOARD_CFG_PATH, FILE_READ);
  if (!f)
    return;
  btKeyboardSavedAddress = f.readStringUntil('\n');
  btKeyboardSavedAddress.trim();
  btKeyboardSavedName = f.readStringUntil('\n');
  btKeyboardSavedName.trim();
  int layout = f.readStringUntil('\n').toInt();
  if (layout >= 0 && layout < BT_LAYOUT_COUNT)
    btKeyboardLayout = (BtKeyboardLayout)layout;
  btSavedAddressType = f.readStringUntil('\n').toInt();
  if (btSavedAddressType > 1)
    btSavedAddressType = 0;
  // Legacy line 5 was the reconnect preference. Preserve it as power-on
  // auto-connect; line 6 independently stores whether the radio is enabled.
  String autoLine = f.available() ? f.readStringUntil('\n') : String("");
  autoLine.trim();
  btPowerOnAutoConnect = autoLine.length() ? autoLine.toInt() == 1 : true;
  btEnabledPreference =
      f.available() ? f.readStringUntil('\n').toInt() == 1 : true;
  f.close();
}
void saveBluetoothKeyboardConfigToSD() {
  String data = btKeyboardSavedAddress + "\n" + btKeyboardSavedName + "\n" +
                String((int)btKeyboardLayout) + "\n" +
                String(btSavedAddressType) + "\n" +
                String(btPowerOnAutoConnect ? 1 : 0) + "\n" +
                String(btEnabledPreference ? 1 : 0) + "\n";
  if (!writeFileSafely(BT_KEYBOARD_CFG_PATH, (const uint8_t *)data.c_str(),
                       data.length()))
    showStatusMessage("Bluetooth settings save failed", 2500);
}
void forgetBluetoothKeyboardConfig() {
  btWanted = false;
  btRetryPolicy.stop();
  btKeyboardSavedAddress = "";
  btKeyboardSavedName = "";
  if (btInitDone)
    NimBLEDevice::deleteAllBonds();
  saveBluetoothKeyboardConfigToSD();
  showStatusMessage("Keyboard forgotten", 1800);
}
void cycleKeyboardLayout() {
  btKeyboardLayout =
      (BtKeyboardLayout)((btKeyboardLayout + 1) % BT_LAYOUT_COUNT);
  btTextMapper.dead = builtInTextMapper.dead = 0;
  saveBluetoothKeyboardConfigToSD();
  showStatusMessage(String("Layout: ") + journal::layoutName(btKeyboardLayout),
                    1800);
}
bool btHidKeyToEvent(uint8_t key, uint8_t mods, BtInputEvent &e) {
  e = {};
  e.hidUsage = key;
  e.modifiers = mods;
  bool ctrl = (mods & 0x11) && !(mods & 0x40);
  if (ctrl) {
    if (key == 0x2c)
      return btCommandEvent(e, BT_CMD_LAYOUT);
    if (key == 0x1d)
      return btCommandEvent(e, BT_CMD_UNDO);
    if (key == 0x1c)
      return btCommandEvent(e, BT_CMD_REDO);
    if (key == 9)
      return btCommandEvent(e, BT_CMD_FIND);
  }
  switch (key) {
  case 0x29:
    btTextMapper.dead = 0;
    return btCommandEvent(e, BT_CMD_ESCAPE);
  case 0x28:
  case 0x58:
    return btCommandEvent(e, BT_CMD_ENTER);
  case 0x2a:
    return btCommandEvent(e, ctrl ? BT_CMD_DELETE_WORD_BACK : BT_CMD_BACKSPACE);
  case 0x2b:
    return btCommandEvent(e, BT_CMD_TAB);
  case 0x4c:
    return btCommandEvent(e, ctrl ? BT_CMD_DELETE_WORD_FORWARD
                                  : BT_CMD_DELETE_FORWARD);
  case 0x4a:
    return btCommandEvent(e, ctrl ? BT_CMD_DOC_HOME : BT_CMD_HOME);
  case 0x4d:
    return btCommandEvent(e, ctrl ? BT_CMD_DOC_END : BT_CMD_END);
  case 0x4b:
    return btCommandEvent(e, BT_CMD_PAGE_UP);
  case 0x4e:
    return btCommandEvent(e, BT_CMD_PAGE_DOWN);
  case 0x4f:
    return btCommandEvent(e, ctrl ? BT_CMD_WORD_RIGHT : BT_CMD_RIGHT);
  case 0x50:
    return btCommandEvent(e, ctrl ? BT_CMD_WORD_LEFT : BT_CMD_LEFT);
  case 0x51:
    return btCommandEvent(e, BT_CMD_DOWN);
  case 0x52:
    return btCommandEvent(e, BT_CMD_UP);
  }
  if (key == 0x39)
    btLedDirty = true;
  char out[16];
  if (!btTextMapper.text(btKeyboardLayout, key, mods, out))
    return false;
  return btTextEvent(e, out);
}
void btProcessHidReports() {
  if (!btHidReportQueue || btConnectBusy)
    return;
  if (btDroppedReportCount) {
    btHeld.fill(false);
    for (auto &s : btSources)
      s.held.fill(false);
    btRepeatEligible = false;
    btAnyKeyDown = false;
    btDroppedReportCount = 0;
  }
  BtHidReport raw;
  while (xQueueReceive(btHidReportQueue, &raw, 0) == pdTRUE) {
    auto source =
        std::find_if(btSources.begin(), btSources.end(),
                     [&](const BtReportSource &s) { return s.chr == raw.chr; });
    if (source == btSources.end())
      continue;
    auto &src = *source;
    std::array<bool, 256> keys{};
    if (src.boot) {
      if (raw.length != 8)
        continue;
      for (int i = 0; i < 8; i++)
        if (raw.data[0] & (1 << i))
          keys[0xe0 + i] = true;
      bool rollover = false;
      for (int i = 2; i < 8; i++) {
        uint8_t k = raw.data[i];
        if (k >= 1 && k <= 3)
          rollover = true;
        else if (k)
          keys[k] = true;
      }
      if (rollover)
        continue;
    } else if (!btReportMap.decode(src.id, raw.data, raw.length, keys))
      continue;
    src.held = keys;
    std::array<bool, 256> combined{};
    for (auto &source : btSources)
      for (int k = 0; k < 256; k++)
        combined[k] = combined[k] || source.held[k];
    uint8_t mods = 0;
    for (int i = 0; i < 8; i++)
      if (combined[0xe0 + i])
        mods |= 1 << i;
    for (int k = 4; k < 0xe0; k++)
      if (combined[k] && !btHeld[k]) {
        BtInputEvent e;
        if (btHidKeyToEvent(k, mods, e)) {
          if (!btQueuePushEvent(e)) {
            btRepeatEligible = false;
            break;
          }
          btRepeatEvent = e;
          btRepeatUsage = k;
          btRepeatEligible =
              e.type == BT_EVENT_TEXT ||
              (e.command > BT_CMD_FIND && e.command != BT_CMD_ESCAPE);
          btKeyPressStartTime = millis();
          btKeyHeld = false;
        }
      }
    btHeld = combined;
    btAnyKeyDown = false;
    for (int k = 4; k < 0xe0; k++)
      btAnyKeyDown |= combined[k];
    if (!combined[btRepeatUsage]) {
      btRepeatEligible = false;
      btKeyHeld = false;
    }
  }
}
void btNotifyCallback(NimBLERemoteCharacteristic *chr, uint8_t *data, size_t n,
                      bool notify) {
  (void)notify;
  if (!data || !n || n > 64 || !btHidReportQueue)
    return;
  BtHidReport r{};
  r.chr = chr;
  r.length = n;
  memcpy(r.data, data, n);
  btReportCount++;
  if (xQueueSend(btHidReportQueue, &r, 0) != pdTRUE)
    btDroppedReportCount++;
}
class JournalBtCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int) override { btLinkLost = true; }
  void onPassKeyEntry(NimBLEConnInfo &info) override {
    if (btPairInfo)
      return;
    btPairInfo = new NimBLEConnInfo(info);
    btPairPin = 0;
    btPairKind = 1;
  }
  uint32_t onPassKeyDisplay(NimBLEConnInfo &info) override {
    (void)info;
    uint32_t pin = esp_random() % 1000000;
    btPairPin = pin;
    btPairKind = 3;
    return pin;
  }
  void onConfirmPasskey(NimBLEConnInfo &info, uint32_t pin) override {
    if (btPairInfo)
      return;
    btPairInfo = new NimBLEConnInfo(info);
    btPairPin = pin;
    btPairKind = 2;
  }
};
static JournalBtCallbacks btClientCallbacks;
void initBluetoothKeyboard() {
  bool firstLoad = !btConfigLoaded;
  loadBluetoothKeyboardConfigFromSD();
  if (firstLoad) {
    btWanted = btEnabledPreference && btPowerOnAutoConnect &&
               btKeyboardSavedAddress.length();
    btRetryPolicy.boot(btEnabledPreference, btPowerOnAutoConnect,
                       btKeyboardSavedAddress.length());
  }
  if (btInitDone || !btEnabledPreference)
    return;
  NimBLEDevice::init("Cardputer Journal");
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
  NimBLEDevice::setSecurityAuth(true, true, true);
  btHidReportQueue = xQueueCreate(24, sizeof(BtHidReport));
  if (!btHidReportQueue) {
    btKeyboardLastError = "Keyboard queue allocation failed";
    btWanted = false;
    btRetryPolicy.stop();
    NimBLEDevice::deinit(true);
    return;
  }
  btInitDone = true;
  btKeyboardEnabled = true;
  btRetryAt = millis() + 1500;
}

void btEnableKeyboard() {
  if (btShutdownPending || btConnectBusy)
    return;
  btRetryPolicy.reset();
  btEnabledPreference = true;
  initBluetoothKeyboard();
  saveBluetoothKeyboardConfigToSD();
  needsRedraw = true;
}

void btRequestPowerOff(bool persist) {
  if (persist) {
    btEnabledPreference = false;
    saveBluetoothKeyboardConfigToSD();
  }
  btWanted = false;
  if (persist)
    btRetryPolicy.reset();
  else
    btRetryPolicy.stop();
  btAutoScan = false;
  btReconnectDirect = false;
  btShutdownPending = true;
  if (btKeyboardClient) {
    if (btConnectBusy)
      btKeyboardClient->cancelConnect();
    if (btKeyboardClient->isConnected())
      btKeyboardClient->disconnect();
  }
  if (btInitDone)
    NimBLEDevice::getScan()->stop();
  needsRedraw = true;
}

void btFinishPowerOff() {
  // The main task only releases NimBLE objects after the connect worker has
  // published completion. deinit waits for the host task to stop, so no HID
  // callback can race queue destruction or retain a live characteristic.
  if (!btShutdownPending || btConnectBusy)
    return;
  // disconnect() is asynchronous; isConnected() already becomes false while
  // DISCONNECTING. Wait for the GAP callback to release the actual handle,
  // otherwise deinit(true) can leave a client pending deferred deletion.
  if (btKeyboardClient &&
      btKeyboardClient->getConnHandle() != BLE_HS_CONN_HANDLE_NONE)
    return;
  if (btInitDone && !NimBLEDevice::deinit(true)) {
    btKeyboardLastError = "Bluetooth shutdown failed";
    return;
  }
  btInitDone = false;
  btKeyboardEnabled = false;
  btKeyboardConnected = false;
  btKeyboardClient = nullptr;
  btKeyboardAdvDevice = nullptr;
  btLedChar = nullptr;
  btLedDirty = false;
  btScanDevices.clear();
  btSources.clear();
  btReportMap = journal::HidMap{};
  btHeld.fill(false);
  btAnyKeyDown = btRepeatEligible = btKeyHeld = false;
  btClearInputEventQueue();
  if (btHidReportQueue) {
    vQueueDelete(btHidReportQueue);
    btHidReportQueue = nullptr;
  }
  if (btPairInfo) {
    delete btPairInfo;
    btPairInfo = nullptr;
  }
  btPairKind = 0;
  btPairStarted = 0;
  btLinkLost = false;
  btConnectResult = 0;
  btSetupState = BT_STATE_IDLE;
  btShutdownPending = false;
  needsRedraw = true;
}

void btKeyboardStartScan() {
  if (btShutdownPending || btConnectBusy)
    return;
  if (!btInitDone)
    btEnableKeyboard();
  if (!btInitDone)
    return;
  if (btConnectBusy)
    return;
  // An explicit scan is a manual action, independent of the boot retry budget.
  if (!btAutoScan) {
    btWanted = false;
    btRetryPolicy.stop();
  }
  btScanDevices.clear();
  btSelectedDevice = 0;
  btKeyboardLastError = "";
  btSetupState = BT_STATE_SCANNING;
  auto *scan = NimBLEDevice::getScan();
  scan->clearResults();
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(30);
  scan->start(8000, false, true);
  needsRedraw = true;
}
bool btKeyboardConnect() {
  if (!btKeyboardAdvDevice && !btReconnectDirect) {
    btKeyboardLastError = "No keyboard selected";
    return false;
  }
  if (!btKeyboardClient)
    btKeyboardClient = NimBLEDevice::createClient();
  if (!btKeyboardClient) {
    btKeyboardLastError = "BLE allocation failed";
    return false;
  }
  btKeyboardClient->setClientCallbacks(&btClientCallbacks, false);
  btKeyboardClient->setConnectTimeout(10000);
  if (!(btReconnectDirect
            ? btKeyboardClient->connect(NimBLEAddress(
                  btKeyboardSavedAddress.c_str(), btSavedAddressType))
            : btKeyboardClient->connect(btKeyboardAdvDevice))) {
    btKeyboardLastError = "Connection failed";
    return false;
  }
  if (!btKeyboardClient->secureConnection()) {
    btKeyboardLastError = "Pairing failed";
    btKeyboardClient->disconnect();
    return false;
  }
  if (!btKeyboardClient->discoverAttributes()) {
    btKeyboardLastError = "Discovery failed";
    btKeyboardClient->disconnect();
    return false;
  }
  auto *hid = btKeyboardClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hid) {
    btKeyboardLastError = "No HID service";
    btKeyboardClient->disconnect();
    return false;
  }
  btSources.clear();
  btLedChar = nullptr;
  btLedDirty = true;
  btHeld.fill(false);
  btRepeatEligible = false;
  btClearInputEventQueue();
  xQueueReset(btHidReportQueue);
  auto *map = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2a4b));
  bool haveMap = false;
  if (map) {
    auto v = map->readValue();
    haveMap = btReportMap.parse((const uint8_t *)v.data(), v.size());
  }
  const auto &chars = hid->getCharacteristics(true);
  btSources.reserve(8);
  for (auto *chr : chars) {
    if (!chr || !chr->getUUID().equals(NimBLEUUID((uint16_t)0x2a4d)) ||
        !(chr->canWrite() || chr->canWriteNoResponse()))
      continue;
    auto *ref = chr->getDescriptor(NimBLEUUID((uint16_t)0x2908));
    if (!ref)
      continue;
    auto v = ref->readValue();
    uint8_t out[64];
    if (v.size() == 2 && uint8_t(v[1]) == 2 &&
        btReportMap.leds(uint8_t(v[0]), false, out)) {
      btLedChar = chr;
      btLedReportID = uint8_t(v[0]);
      btLedBoot = false;
      break;
    }
  }

  if (haveMap)
    for (auto *chr : chars) {
      if (!chr || !chr->getUUID().equals(NimBLEUUID((uint16_t)0x2a4d)) ||
          !(chr->canNotify() || chr->canIndicate()))
        continue;
      auto *ref = chr->getDescriptor(NimBLEUUID((uint16_t)0x2908));
      if (!ref)
        continue;
      auto v = ref->readValue();
      if (v.size() != 2 || uint8_t(v[1]) != 1 ||
          !btReportMap.hasKeyboard(uint8_t(v[0])))
        continue;
      if (btSources.size() >= 8)
        break;
      btSources.push_back({chr, uint8_t(v[0]), false, {}});
      if (!chr->subscribe(chr->canNotify(), btNotifyCallback))
        btSources.pop_back();
    }
  if (btSources.empty()) {
    auto *mode = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2a4e));
    uint8_t boot = 0;
    bool ok =
        mode && mode->canWriteNoResponse() && mode->writeValue(&boot, 1, false);
    if (ok)
      for (auto *chr : chars) {
        if (!chr || !chr->getUUID().equals(NimBLEUUID((uint16_t)0x2a22)) ||
            !chr->canNotify())
          continue;
        btSources.push_back({chr, 0, true, {}});
        if (!chr->subscribe(true, btNotifyCallback))
          btSources.pop_back();
        break;
      }
  }
  if (!btSources.empty() && btSources[0].boot) {
    btLedChar = hid->getCharacteristic(NimBLEUUID((uint16_t)0x2a32));
    btLedBoot = true;
  }
  if (btSources.empty()) {
    btKeyboardLastError = "Unsupported keyboard report";
    btKeyboardClient->disconnect();
    return false;
  }
  return true;
}
void btConnectTask(void *) {
  bool ok = btKeyboardConnect();
  btConnectResult = ok ? 1 : -1;
  vTaskDelete(nullptr);
}
void btStartConnect() {
  if (btShutdownPending || !btInitDone || btConnectBusy ||
      (!btKeyboardAdvDevice && !btReconnectDirect))
    return;
  btConnectBusy = true;
  btConnectResult = 0;
  btSetupState = BT_STATE_CONNECTING;
  if (xTaskCreate(btConnectTask, "bt-connect", 8192, nullptr, 1, nullptr) !=
      pdPASS) {
    btConnectBusy = false;
    btSetupState = BT_STATE_IDLE;
    btKeyboardLastError = "Connect task failed";
  }
  needsRedraw = true;
}
void btKeyboardDisconnect() {
  btWanted = false;
  btRetryPolicy.stop();
  if (btKeyboardClient && btKeyboardClient->isConnected())
    btKeyboardClient->disconnect();
  btKeyboardConnected = false;
  btHeld.fill(false);
  btRepeatEligible = false;
  btAnyKeyDown = false;
  btClearInputEventQueue();
  btSetupState = BT_STATE_IDLE;
}
void btKeyboardPollConnection() {
  if (!btInitDone) {
    btFinishPowerOff();
    return;
  }
  if (!btShutdownPending && btKeyboardConnected && !btConnectBusy && btLedDirty && btLedChar) {
    uint8_t out[64];
    size_t n = 0;
    if (btLedBoot) {
      out[0] = btTextMapper.caps ? 2 : 0;
      n = 1;
    } else
      n = btReportMap.leds(btLedReportID, btTextMapper.caps, out);
    if (n)
      btLedChar->writeValue(out, n, !btLedChar->canWriteNoResponse());
    btLedDirty = false;
  }
  int connectResult = btConnectResult.exchange(0);
  if (connectResult) {
    bool ok = connectResult == 1;
    btConnectBusy = false;
    btKeyboardConnected = ok && !btShutdownPending;
    btPairKind = 0;
    if (btPairInfo) {
      delete btPairInfo;
      btPairInfo = nullptr;
    }
    btPairStarted = 0;
    btRetryAt = millis() + 10000;
    btSetupState = ok ? BT_STATE_CONNECTED : BT_STATE_IDLE;
    if (ok && !btShutdownPending) {
      btRetryPolicy.connected();
      btLinkLost = false;
      btWanted = true;
      auto info = btKeyboardClient->getConnInfo();
      btKeyboardSavedAddress = info.getIdAddress().toString().c_str();
      btSavedAddressType = info.getIdAddress().getType();
      btKeyboardSavedName = btKeyboardName;
      saveBluetoothKeyboardConfigToSD();
      showStatusMessage("Keyboard connected", 1500);
    } else if (!btShutdownPending) {
      showStatusMessage("Keyboard: " + btKeyboardLastError, 2500);
      if (btAutomaticAttempt && btRetryPolicy.exhausted()) {
        btKeyboardLastError = "Three attempts failed; Bluetooth off";
        showStatusMessage(btKeyboardLastError, 4000);
        btRequestPowerOff(false);
      }
    }
    btAutomaticAttempt = false;
    needsRedraw = true;
  }
  if (btShutdownPending && !btConnectBusy) {
    btFinishPowerOff();
    return;
  }
  if (btLinkLost && !btConnectBusy) {
    bool wasConnected = btKeyboardConnected;
    btLinkLost = false;
    btKeyboardConnected = false;
    if (wasConnected && btWanted)
      btRetryPolicy.reconnect(btKeyboardSavedAddress.length());
    btHeld.fill(false);
    btSources.clear();
    btRepeatEligible = false;
    btAnyKeyDown = false;
    btRetryAt = millis() + 1500;
    btSetupState = BT_STATE_IDLE;
    needsRedraw = true;
  }
  if (btSetupState == BT_STATE_SCANNING &&
      !NimBLEDevice::getScan()->isScanning()) {
    auto results = NimBLEDevice::getScan()->getResults();
    btScanDevices.clear();
    const NimBLEAdvertisedDevice *saved = nullptr;
    for (int i = 0; i < results.getCount(); i++) {
      auto *d = results.getDevice(i);
      if (!d || !d->isAdvertisingService(NimBLEUUID((uint16_t)0x1812)))
        continue;
      if (btScanDevices.size() < 32)
        btScanDevices.push_back(d);
      if (String(d->getAddress().toString().c_str())
              .equalsIgnoreCase(btKeyboardSavedAddress))
        saved = d;
      for (int j = 0; !saved && j < NimBLEDevice::getNumBonds(); j++)
        if (d->getAddress() == NimBLEDevice::getBondedAddress(j))
          saved = d;
    }
    btSetupState = btScanDevices.empty() ? BT_STATE_IDLE : BT_STATE_PICKER;
    btSelectedDevice = 0;
    needsRedraw = true;
    if (btAutoScan) {
      btAutoScan = false;
      if (saved) {
        btKeyboardAdvDevice = saved;
        btKeyboardName = saved->getName().c_str();
        btKeyboardAddress = saved->getAddress().toString().c_str();
        btStartConnect();
      } else
        btSetupState = BT_STATE_IDLE;
      btRetryAt = millis() + 10000;
    }
  }
  if (btWanted && !btKeyboardConnected && !btConnectBusy &&
      btSetupState == BT_STATE_IDLE && (long)(millis() - btRetryAt) >= 0) {
    if (!btRetryPolicy.takeAttempt()) {
      btKeyboardLastError = "Three attempts failed; Bluetooth off";
      showStatusMessage(btKeyboardLastError, 4000);
      btRequestPowerOff(false);
      btFinishPowerOff();
      return;
    }
    btAutomaticAttempt = true;
    btReconnectDirect = true;
    btKeyboardName = btKeyboardSavedName;
    btStartConnect();
  }
  if (btPairKind) {
    appMode = MODE_MENU;
    currentMenu = MENU_SETTINGS_BT;
    if (!btPairStarted) {
      btPairStarted = millis();
      btPairDigits = "";
    }
    if (millis() - btPairStarted > 60000) {
      if (btPairInfo && btPairKind == 2)
        NimBLEDevice::injectConfirmPasskey(*btPairInfo, false);
      if (btKeyboardClient)
        btKeyboardClient->disconnect();
      btPairKind = 0;
    }
    needsRedraw = true;
  }
}
bool btEventToKeys(const BtInputEvent &e, Keyboard_Class::KeysState &ks) {
  ks.reset();
  ks.modifiers = e.modifiers;
  ks.ctrl = e.modifiers & 0x11;
  ks.shift = e.modifiers & 0x22;
  ks.alt = e.modifiers & 0x44;
  ks.opt = e.modifiers & 0x04;
  if (e.type == BT_EVENT_TEXT) {
    for (const char *p = e.text; *p; p++)
      ks.word.push_back(*p);
  } else if (e.command == BT_CMD_ENTER)
    ks.enter = true;
  else if (e.command == BT_CMD_BACKSPACE)
    ks.del = true;
  else if (e.command == BT_CMD_TAB)
    ks.tab = true;
  else if (e.command == BT_CMD_ESCAPE)
    ks.word.push_back(27);
  else if (e.command == BT_CMD_UP)
    ks.word.push_back(';');
  else if (e.command == BT_CMD_DOWN)
    ks.word.push_back('.');
  else if (e.command == BT_CMD_LEFT)
    ks.word.push_back(',');
  else if (e.command == BT_CMD_RIGHT)
    ks.word.push_back('/');
  return true;
}
bool readAppKeys(Keyboard_Class::KeysState &ks) {
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
  if (!btQueuePopEvent(e))
    return false;
  if (e.type == BT_EVENT_COMMAND && e.command == BT_CMD_LAYOUT) {
    cycleKeyboardLayout();
    return false;
  }
  return btEventToKeys(e, ks);
}
bool anyAppKeyPressed() {
  Keyboard_Class::KeysState ks;
  bool pressed = readAppKeys(ks);
  if (pressed)
    btClearInputEventQueue(); // all simultaneous keys only stop audio
  return pressed;
}
void openBluetoothKeyboardSetup() {
  btMenuIndex = 0;
  currentMenu = MENU_SETTINGS_BT;
  needsRedraw = true;
}
void handleBluetoothKeyboardSetupKeys(const Keyboard_Class::KeysState &ks) {
  if (!btPairKind && ks.ctrl && ks.space) {
    cycleKeyboardLayout();
    return;
  }
  if (btPairKind) {
    if (ks.del) {
      if (btPairKind == 1 && btPairDigits.length())
        btPairDigits.remove(btPairDigits.length() - 1);
      else {
        if (btPairInfo && btPairKind == 2)
          NimBLEDevice::injectConfirmPasskey(*btPairInfo, false);
        if (btKeyboardClient)
          btKeyboardClient->disconnect();
        btPairKind = 0;
      }
    } else if (ks.enter && btPairInfo) {
      if (btPairKind == 2)
        NimBLEDevice::injectConfirmPasskey(*btPairInfo, true);
      else if (btPairKind == 1 && btPairDigits.length() == 6)
        NimBLEDevice::injectPassKey(*btPairInfo, btPairDigits.toInt());
      else
        return;
      delete btPairInfo;
      btPairInfo = nullptr;
      btPairKind = 0;
    } else
      for (char c : ks.word)
        if (c >= '0' && c <= '9' && btPairDigits.length() < 6)
          btPairDigits += c;
    needsRedraw = true;
    return;
  }
  if (ks.opt && ks.enter) {
    if (btConnectBusy) {
      btRequestPowerOff(false);
      return;
    }
    btKeyboardDisconnect();
    forgetBluetoothKeyboardConfig();
    return;
  }
  if (ks.opt) {
    cycleKeyboardLayout();
    return;
  }
  bool back = ks.del;
  for (auto c : ks.word)
    if (c == 27 || c == '`')
      back = true;
  if (back) {
    if (btSetupState == BT_STATE_PICKER || btSetupState == BT_STATE_SCANNING) {
      if (btInitDone)
        NimBLEDevice::getScan()->stop();
      btSetupState = BT_STATE_IDLE;
      btWanted = false;
      btRetryPolicy.stop();
    } else
      openMenu(MENU_SETTINGS_SYNC);
    needsRedraw = true;
    return;
  }
  if (btSetupState == BT_STATE_PICKER) {
    for (auto c : ks.word) {
      if (c == ';' && btSelectedDevice > 0)
        --btSelectedDevice;
      if (c == '.' && btSelectedDevice + 1 < (int)btScanDevices.size())
        ++btSelectedDevice;
    }
    if (ks.enter && !btScanDevices.empty()) {
      btAutomaticAttempt = false;
      btReconnectDirect = false;
      btKeyboardAdvDevice = btScanDevices[btSelectedDevice];
      btKeyboardName = btKeyboardAdvDevice->getName().c_str();
      btKeyboardAddress = btKeyboardAdvDevice->getAddress().toString().c_str();
      btStartConnect();
    }
    needsRedraw = true;
    return;
  }
  for (auto c : ks.word) {
    if (c == ';' && btMenuIndex > 0)
      --btMenuIndex;
    if (c == '.' && btMenuIndex < 5)
      ++btMenuIndex;
  }
  if (ks.enter) {
    switch (btMenuIndex) {
    case 0:
      if (btConnectBusy)
        btRequestPowerOff(false);
      else if (btKeyboardConnected)
        btKeyboardDisconnect();
      else if (btSetupState == BT_STATE_SCANNING) {
        NimBLEDevice::getScan()->stop();
        btSetupState = BT_STATE_IDLE;
        btWanted = false;
        btRetryPolicy.stop();
      } else {
        btAutoScan = false;
        btAutomaticAttempt = false;
        btKeyboardStartScan();
      }
      break;
    case 1:
      if (btInitDone || btShutdownPending)
        btRequestPowerOff(true);
      else
        btEnableKeyboard();
      break;
    case 2:
      btPowerOnAutoConnect = !btPowerOnAutoConnect;
      saveBluetoothKeyboardConfigToSD();
      break;
    case 3:
      cycleKeyboardLayout();
      break;
    case 4:
      if (!btConnectBusy) {
        btKeyboardDisconnect();
        forgetBluetoothKeyboardConfig();
      } else
        showStatusMessage("Cancel connection before forgetting", 2500);
      break;
    case 5:
      openMenu(MENU_SETTINGS_SYNC);
      break;
    }
  }
  needsRedraw = true;
}
void drawBluetoothKeyboardSetup() {
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  int w = M5Cardputer.Display.width();
  M5Cardputer.Display.setCursor(3, 0);
  M5Cardputer.Display.print(fitUiLabel("Bluetooth keyboard", w - 6));
  int y = topBarHeight;
  if (btPairKind) {
    auto row = [&](const String &t) {
      if (y + lineHeight <= M5Cardputer.Display.height() - uiFooterHeight()) {
        M5Cardputer.Display.setCursor(3, y);
        M5Cardputer.Display.print(fitUiLabel(t, w - 6));
        y += lineHeight;
      }
    };
    row(btPairKind == 1   ? "Enter passkey:"
        : btPairKind == 2 ? "Confirm number:"
                          : "Type on keyboard:");
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)btPairPin);
    row(btPairKind == 1 ? btPairDigits : String(pin));
    drawUiFooter("Enter: accept  Back: cancel");
  } else if (btSetupState == BT_STATE_PICKER) {
    int visible = std::max(1, listVisibleLines);
    int first = std::max(0, btSelectedDevice - visible + 1);
    int total = btScanDevices.size();
    for (int row = 0; row < visible && first + row < total; ++row) {
      int i = first + row;
      drawUiListLabel(btScanDevices[i]->getName().c_str(), 3,
                      y + row * lineHeight, uiListLabelWidth(total, visible),
                      i == btSelectedDevice);
    }
    drawUiScrollbar(total, first, visible, y, visible * lineHeight);
    drawUiFooter("Arrows: choose  Enter: pair  Back: cancel");
  } else {
    String action = btShutdownPending     ? "Switching Bluetooth off..."
                    : btConnectBusy       ? "Cancel connection"
                    : btKeyboardConnected ? "Disconnect: " + btKeyboardName
                    : btSetupState == BT_STATE_SCANNING
                        ? "Cancel scan"
                        : "Scan / pair keyboard";
    String rows[] = {action,
                     String("Bluetooth: ") +
                         (btInitDone ? "On (Enter: off)" : "Off (Enter: on)"),
                     String("Power-on connect: ") +
                         (btPowerOnAutoConnect ? "On" : "Off"),
                     String("Layout: ") + journal::layoutName(btKeyboardLayout),
                     "Forget saved keyboard",
                     "Back"};
    int visible = std::max(1, listVisibleLines);
    int first = std::max(0, btMenuIndex - visible + 1);
    for (int row = 0; row < visible && first + row < 6; ++row) {
      int i = first + row;
      drawUiListLabel(rows[i], 3, y + row * lineHeight,
                      uiListLabelWidth(6, visible), i == btMenuIndex);
    }
    drawUiScrollbar(6, first, visible, y, visible * lineHeight);
    drawUiFooter(btKeyboardLastError.length()
                     ? btKeyboardLastError
                     : "Ctrl+Space: layout  Enter: select");
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = MENU_SETTINGS_BT;
}
void handleBtKeyRepeat() {
  if (appMode != MODE_EDITOR || !btKeyboardConnected || !btRepeatEligible ||
      !btHeld[btRepeatUsage])
    return;
  unsigned long now = millis();
  if (!btKeyHeld) {
    if (now - btKeyPressStartTime < KEY_REPEAT_DELAY_MS)
      return;
    btKeyHeld = true;
  } else if (now - btLastKeyActionTime < KEY_REPEAT_RATE_MS)
    return;
  btLastKeyActionTime = now;
  btQueuePushEvent(btRepeatEvent);
}

void handleBluetoothInput() {
  btKeyboardPollConnection();

  if (!btKeyboardConnected) {
    return;
  }

  btProcessHidReports();

  BtInputEvent event;
  bool any = false;

  while (btQueuePopEvent(event)) {
    any = true;
    // Built-in repeat and BLE repeat have separate ownership. A BLE action
    // must never become the action repeated by a held Cardputer key.
    haveLastAction = false;
    keyHeld = false;

    unsigned long now = millis();
    lastKeyPressTime = now;
    savedAfterIdle = false;

    if (event.type == BT_EVENT_COMMAND && event.command == BT_CMD_LAYOUT) {
      cycleKeyboardLayout();
      btRepeatEligible = false;
      btKeyHeld = false;
      needsRedraw = true;
      continue;
    }
    if (appMode == MODE_EDITOR) {
      if (event.type == BT_EVENT_TEXT) {
        insertText(event.text);
      } else if (event.type == BT_EVENT_COMMAND) {
        switch (event.command) {
        case BT_CMD_LAYOUT:
          cycleKeyboardLayout();
          btRepeatEligible = false;
          break;
        case BT_CMD_UNDO:
          journalUndo();
          btRepeatEligible = false;
          break;
        case BT_CMD_REDO:
          journalRedo();
          btRepeatEligible = false;
          break;
        case BT_CMD_FIND:
          journalFind();
          btRepeatEligible = false;
          break;
        case BT_CMD_ESCAPE:
          openMainMenu();
          btRepeatEligible = false;
          break;
        case BT_CMD_BACKSPACE:
          backspaceChar();
          break;

        case BT_CMD_ENTER:
          insertNewline();
          break;

        case BT_CMD_TAB:
          insertTab();
          break;

        case BT_CMD_UP:
          moveCursorUp();
          break;

        case BT_CMD_DOWN:
          moveCursorDown();
          break;

        case BT_CMD_LEFT:
          moveCursorLeft();
          break;

        case BT_CMD_RIGHT:
          moveCursorRight();
          break;

        case BT_CMD_DELETE_WORD_BACK:
          deleteWordBackwards();
          break;

        case BT_CMD_DELETE_WORD_FORWARD:
          deleteWordForwards();
          break;

        case BT_CMD_WORD_LEFT:
          moveCursorWordLeft();
          break;

        case BT_CMD_WORD_RIGHT:
          moveCursorWordRight();
          break;

        case BT_CMD_PAGE_UP:
          scrollPageUp();
          break;

        case BT_CMD_PAGE_DOWN:
          scrollPageDown();
          break;

        case BT_CMD_HOME:
          moveCursorHome();
          break;

        case BT_CMD_END:
          moveCursorEnd();
          break;

        case BT_CMD_DOC_HOME:
          moveCursorDocHome();
          break;

        case BT_CMD_DOC_END:
          moveCursorDocEnd();
          break;

        case BT_CMD_DELETE_FORWARD:
          deleteForwardChar();
          break;

        default:
          break;
        }
      }

      needsRedraw = true;

    } else if (appMode == MODE_MENU) {
      btRepeatEligible = false;
      btKeyHeld = false;
      Keyboard_Class::KeysState ks;
      btEventToKeys(event, ks);
      if (btPairKind && event.type == BT_EVENT_COMMAND &&
          event.command == BT_CMD_ESCAPE) {
        ks.del = true;
        handleBluetoothKeyboardSetupKeys(ks);
      } else if (event.type == BT_EVENT_COMMAND &&
                 event.command == BT_CMD_ESCAPE) {
        menuBack();
      } else if (currentMenu == MENU_SETTINGS_WIFI)
        handleWiFiSetupKeys(ks);
      else if (currentMenu == MENU_SETTINGS_BT)
        handleBluetoothKeyboardSetupKeys(ks);
      else if (currentMenu == MENU_SETTINGS_DATETIME)
        handleDateTimeEditorKeys(ks);
      else if (currentMenu == MENU_SETTINGS_ABOUT)
        handleAboutScreenKeys(ks);
      else if (currentMenu == MENU_SETTINGS_PRINTER)
        handlePrinterSetupKeys(ks);
      else if (event.type == BT_EVENT_COMMAND) {
        if (event.command == BT_CMD_ENTER)
          menuSelect();
        else if (event.command == BT_CMD_BACKSPACE)
          menuBack();
        else if (event.command == BT_CMD_UP)
          menuMoveUp();
        else if (event.command == BT_CMD_DOWN)
          menuMoveDown();
      }
    }
  }

  if (any) {
    needsRedraw = true;
  }
}
