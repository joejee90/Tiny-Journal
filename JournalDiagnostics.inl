// Local USB diagnostics only. Production firmware compiles none of this code.
// No command reads notes, credentials, tokens, or initiates Drive sync.
#ifdef TJ_DIAGNOSTICS
#include <ctype.h>
#include <stdlib.h>

static void diagnosticReply(const char* message) {
  Serial.print("TJ ");
  Serial.println(message);
}

static bool diagnosticDigits(const String& value, size_t length) {
  if (value.length() != length) return false;
  for (size_t i = 0; i < length; ++i)
    if (value[i] < '0' || value[i] > '9') return false;
  return true;
}

static void diagnosticStorageCheck() {
  if (SD.cardType() == CARD_NONE) {
    diagnosticReply("ERROR SD unavailable");
    return;
  }
  // No existing file (including recovery sidecars) can be used as the test target.
  String path;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    char name[96];
    snprintf(name, sizeof(name), "/journal/misc/tj-diagnostic-%08lx-%08lx.txt",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    String candidate(name);
    if (!SD.exists(candidate) && !SD.exists(candidate + ".tmp") && !SD.exists(candidate + ".bak")) {
      path = candidate;
      break;
    }
  }
  if (!path.length()) {
    diagnosticReply("ERROR cannot allocate unique test filename");
    return;
  }
  const char payload[] = "Tiny Journal storage check\nUTF-8: £ € é Ж ё\n";
  bool ok = writeFileSafely(path, (const uint8_t*)payload, sizeof(payload) - 1);
  if (ok) {
    File file = SD.open(path, FILE_READ);
    uint8_t actual[sizeof(payload)]{};
    ok = file && file.size() == sizeof(payload) - 1;
    if (ok) ok = file.read(actual, sizeof(payload) - 1) == sizeof(payload) - 1;
    if (file) file.close();
    if (ok) ok = memcmp(actual, payload, sizeof(payload) - 1) == 0;
  }
  // This namespace was exclusively allocated above. Remove only those own paths.
  bool cleaned = true;
  const String ownPaths[] = {path, path + ".tmp", path + ".bak"};
  for (const auto& own : ownPaths)
    if (SD.exists(own) && !SD.remove(own)) cleaned = false;
  diagnosticReply(ok && cleaned ? "STORAGECHECK PASS"
                  : !cleaned    ? "STORAGECHECK ERROR cleanup failed"
                                : "STORAGECHECK FAIL");
}

static void diagnosticCommand(const String& line) {
  int split = line.indexOf(' ');
  String command = split < 0 ? line : line.substring(0, split);
  String argument = split < 0 ? String() : line.substring(split + 1);
  command.toUpperCase();
  argument.trim();
  if (command == "STATUS") {
    if (argument.length()) {
      diagnosticReply("ERROR STATUS takes no argument");
      return;
    }
    Serial.printf(
        "TJ STATUS heap=%lu sd=%u btState=%u btConnected=%u btBusy=%u pairKind=%u printerBusy=%u\n",
        (unsigned long)ESP.getFreeHeap(), SD.cardType() != CARD_NONE, unsigned(btSetupState),
        btKeyboardConnected, unsigned(btConnectBusy.load()), unsigned(btPairKind.load()),
        JournalPrinter::busy());
    Serial.printf("TJ BTPOWER enabled=%u preference=%u auto=%u attempts=%u retryActive=%u shutdown=%u\n",
                  btKeyboardEnabled, btEnabledPreference, btPowerOnAutoConnect, btRetryPolicy.attempts(),
                  btRetryPolicy.active(), btShutdownPending);
  } else if (command == "BTRADIO") {
    if (argument != "ON" && argument != "OFF") { diagnosticReply("ERROR BTRADIO ON/OFF"); return; }
    if (argument == "OFF") btRequestPowerOff(true); else btEnableKeyboard();
    diagnosticReply("BTRADIO REQUESTED");
  } else if (command == "BTAUTO") {
    if (argument != "ON" && argument != "OFF") { diagnosticReply("ERROR BTAUTO ON/OFF"); return; }
    bool value = argument == "ON";
    if (value != btPowerOnAutoConnect) {
      btMenuIndex = 2; btSetupState = BT_STATE_IDLE;
      Keyboard_Class::KeysState keys; keys.enter = true;
      handleBluetoothKeyboardSetupKeys(keys);
    }
    diagnosticReply("BTAUTO SAVED");
  } else if (command == "BTSCAN") {
    if (argument.length() || btConnectBusy || btKeyboardConnected) {
      diagnosticReply("ERROR BTSCAN requires disconnected idle keyboard");
      return;
    }
    btAutoScan = false;
    btWanted = false;
    btKeyboardStartScan();
    appMode = MODE_MENU;
    currentMenu = MENU_SETTINGS_BT;
    diagnosticReply("BTSCAN STARTED");
  } else if (command == "BTLIST") {
    if (argument.length()) {
      diagnosticReply("ERROR BTLIST takes no argument");
      return;
    }
    if (btSetupState == BT_STATE_SCANNING) {
      diagnosticReply("BTLIST SCANNING");
      return;
    }
    Serial.printf("TJ BTLIST count=%u\n", unsigned(btScanDevices.size()));
    for (size_t i = 0; i < btScanDevices.size(); ++i) {
      Serial.printf("TJ BTDEVICE %u ", unsigned(i + 1));
      auto name = btScanDevices[i]->getName();
      for (unsigned char c : name) Serial.write(c >= 32 && c < 127 ? c : '?');
      Serial.println();
    }
  } else if (command == "BTSELECT") {
    char* end = nullptr;
    long index = strtol(argument.c_str(), &end, 10);
    if (!argument.length() || *end || index < 1 || index > long(btScanDevices.size()) ||
        btSetupState != BT_STATE_PICKER || btConnectBusy) {
      diagnosticReply("ERROR BTSELECT requires a valid 1-based picker index");
      return;
    }
    btSelectedDevice = int(index - 1);
    Keyboard_Class::KeysState keys;
    keys.enter = true;
    handleBluetoothKeyboardSetupKeys(keys);
    diagnosticReply("BTSELECT CONNECTING; normal pairing required");
  } else if (command == "BTPIN") {
    if (btPairKind != 1 || !btPairInfo || !diagnosticDigits(argument, 6)) {
      diagnosticReply("ERROR BTPIN requires active passkey entry and six digits");
      return;
    }
    // Feed the exact normal UI entry route. Never log or echo the passkey.
    btPairDigits = "";
    Keyboard_Class::KeysState keys;
    for (char c : argument) keys.word.push_back(c);
    handleBluetoothKeyboardSetupKeys(keys);
    keys.reset();
    keys.enter = true;
    handleBluetoothKeyboardSetupKeys(keys);
    diagnosticReply("BTPIN SUBMITTED");
  } else if (command == "BTCONFIRM") {
    // Explicit matching number is required; there is no blind auto-accept command.
    if (btPairKind != 2 || !btPairInfo || !diagnosticDigits(argument, 6) ||
        uint32_t(argument.toInt()) != btPairPin) {
      diagnosticReply("ERROR BTCONFIRM requires active comparison and displayed six-digit number");
      return;
    }
    Keyboard_Class::KeysState keys;
    keys.enter = true;
    handleBluetoothKeyboardSetupKeys(keys);
    diagnosticReply("BTCONFIRM SUBMITTED");
  } else if (command == "PRINTERHOST") {
    IPAddress address;
    if (!address.fromString(argument) || address == IPAddress(0, 0, 0, 0) || address[0] == 0 ||
        address[0] >= 224) {
      diagnosticReply("ERROR PRINTERHOST requires a unicast IPv4 address");
      return;
    }
    String previous = JournalPrinter::config.host;
    JournalPrinter::config.host = argument;
    if (savePrinterConfig())
      diagnosticReply("PRINTERHOST SAVED");
    else {
      JournalPrinter::config.host = previous;
      diagnosticReply("ERROR printer configuration save failed");
    }
  } else if (command == "PRINTERSTATUS") {
    if (argument.length()) { diagnosticReply("ERROR PRINTERSTATUS takes no argument"); return; }
    Serial.printf("TJ PRINTERSTATUS configured=%u busy=%u phase=%d result=%d wifiStatus=%d sent=%lu queued=%lu cut=%u\n",
        JournalPrinter::config.host.length()!=0, JournalPrinter::busy(), JournalPrinter::phase.load(),
        JournalPrinter::lastResult.load(), JournalPrinter::lastWiFiStatus.load(),
        (unsigned long)JournalPrinter::lastBytesSent.load(), (unsigned long)JournalPrinter::queuedBytes.load(),
        JournalPrinter::config.cut);
  } else if (command == "PRINTTEST") {
    if (argument.length()) {
      diagnosticReply("ERROR PRINTTEST takes no argument");
      return;
    }
    bool queued = JournalPrinter::enqueue("Tiny Journal diagnostic\nABC xyz 123\nUK pound: £\n",
                                          wifiSsid, wifiPassword);
    diagnosticReply(queued ? "PRINTTEST QUEUED; delivery is not physical-print confirmation"
                           : "ERROR printer busy/unconfigured");
  } else if (command == "STORAGECHECK") {
    if (argument.length()) {
      diagnosticReply("ERROR STORAGECHECK takes no argument");
      return;
    }
    diagnosticStorageCheck();
  } else if (command == "MSC") {
    if (argument.length()) { diagnosticReply("ERROR MSC takes no argument"); return; }
    diagnosticReply("MSC RESTART"); Serial.flush(); enterUsbStorageMode();
  } else if (command == "HELP") {
    diagnosticReply(
        "COMMANDS STATUS BTRADIO ON/OFF BTAUTO ON/OFF BTSCAN BTLIST BTSELECT N BTPIN dddddd BTCONFIRM dddddd PRINTERHOST ipv4 "
        "PRINTTEST PRINTERSTATUS STORAGECHECK MSC");
  } else {
    diagnosticReply("ERROR unknown command");
  }
}

void beginJournalDiagnostics() {
  Serial.begin(115200);
  diagnosticReply("DIAGNOSTICS READY; local USB only; no note/credential commands");
}

void pollJournalDiagnostics() {
  static char buffer[129];
  static size_t length = 0;
  static bool overflow = false;
  unsigned budget = 64;
  while (budget-- && Serial.available()) {
    int value = Serial.read();
    if (value < 0) break;
    if (value == '\r') continue;
    if (value == '\n') {
      if (overflow)
        diagnosticReply("ERROR command exceeds 128 bytes");
      else if (length) {
        buffer[length] = 0;
        diagnosticCommand(String(buffer));
      }
      memset(buffer, 0, sizeof(buffer));
      length = 0;
      overflow = false;
    } else if (value >= 32 && value < 127) {
      if (length < 128 && !overflow)
        buffer[length++] = char(value);
      else
        overflow = true;
    } else {
      overflow = true;
    }
  }
}
#endif
