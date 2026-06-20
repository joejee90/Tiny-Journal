#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <vector>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Ring buffer for text/command events generated from HID reports.
// This replaces the old single-byte char queue so Bluetooth keyboards can
// emit text events and editor commands cleanly. It can also carry display-font
// single-byte glyphs such as £.
enum BtInputEventType : uint8_t {
  BT_EVENT_NONE = 0,
  BT_EVENT_TEXT,
  BT_EVENT_COMMAND
};

enum BtEditorCommand : uint8_t {
  BT_CMD_NONE = 0,
  BT_CMD_BACKSPACE,
  BT_CMD_ENTER,
  BT_CMD_TAB,
  BT_CMD_UP,
  BT_CMD_DOWN,
  BT_CMD_LEFT,
  BT_CMD_RIGHT,
  BT_CMD_DELETE_WORD_BACK,
  BT_CMD_DELETE_WORD_FORWARD,
  BT_CMD_WORD_LEFT,
  BT_CMD_WORD_RIGHT,
  BT_CMD_PAGE_UP,
  BT_CMD_PAGE_DOWN,
  BT_CMD_HOME,
  BT_CMD_END,
  BT_CMD_DOC_HOME,
  BT_CMD_DOC_END,
  BT_CMD_DELETE_FORWARD
};

struct BtInputEvent {
  BtInputEventType type;
  char text[5];  // UTF-8: max 4 bytes + NUL
  uint8_t command;
  uint8_t hidUsage;
  uint8_t modifiers;
};

const int BT_INPUT_EVENT_QUEUE_SIZE = 64;
volatile int btEventHead = 0;
volatile int btEventTail = 0;
BtInputEvent btEventQueue[BT_INPUT_EVENT_QUEUE_SIZE];


// ---------- SD card pins for Cardputer (SPI mode) ----------
#define SD_SPI_SCK_PIN 40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN 12

// ---------- USB mass storage ----------
// Requires Arduino IDE Tools -> USB Mode to be set to USB-OTG / TinyUSB on ESP32-S3.
static const uint16_t USB_MSC_BLOCK_SIZE = 512;
USBMSC *usbMsc = nullptr;
bool usbMscConfigured = false;
uint32_t usbMscBlockCount = 0;


// ---------- Editor settings ----------
static const int TAB_SIZE = 4;
static const int EDITOR_LEFT_MARGIN = 2;  // pixels of left padding in editor
// Idle autosave delay: configurable from the Editor behaviour menu
static unsigned long idleAutosaveDelayMs = 5000;  // default 5s

// Key repeat settings (editor only)
static const unsigned long KEY_REPEAT_DELAY_MS = 500;  // 0.5s before first repeat
static const unsigned long KEY_REPEAT_RATE_MS = 100;   // 0.1s between repeats

// Green “IBM-style” text colour for editor
static const uint16_t EDITOR_FG_COLOR = 0x07E0;  // pure green
static const uint16_t EDITOR_BG_COLOR = BLACK;

// Current theme colours (these are what the UI actually uses)
static uint16_t editorFgColor = EDITOR_FG_COLOR;
static uint16_t editorBgColor = EDITOR_BG_COLOR;

// 0 = green on black, 1 = amber, 2 = white on black, 3 = black on white
static int editorThemeIndex = 0;

// 0 = standard, 1 = large, 2 = cataracts mode
static int editorFontSizeIndex = 0;

// ---------- App mode ----------
enum AppMode {
  MODE_EDITOR,
  MODE_MENU
};

AppMode appMode = MODE_EDITOR;

// ---------- Menu IDs ----------
enum MenuId {
  MENU_NONE,
  MENU_MAIN,
  MENU_WRITING,
  MENU_TEXTDOCS,
  MENU_TEMPLATES,
  MENU_AUDIO,
  MENU_SETTINGS,
  MENU_SETTINGS_STORAGE,
  MENU_SETTINGS_EDITOR,
  MENU_SETTINGS_APPEARANCE,
  MENU_SETTINGS_DATETIME,
  MENU_SETTINGS_SYNC,
  MENU_SETTINGS_BT,
  MENU_SETTINGS_WIFI,
  MENU_SETTINGS_ABOUT
};


MenuId currentMenu = MENU_NONE;
int menuSelectedIndex = 0;
int menuScrollOffset = 0;

// ---------- Editor state ----------
String textBuffer;       // Entire document
size_t cursorIndex = 0;  // Position in textBuffer [0..length]

int preferredCol = 0;  // Column we try to maintain when going up/down

// Current file path for this document
String currentFilePath = "/journal/misc/note-boot.txt";

// Visual line representation: [start, end) indices in textBuffer
struct VisualLine {
  size_t start;
  size_t end;
};

std::vector<VisualLine> visualLines;

int maxCols = 0;
int lineHeight = 0;
int topBarHeight = 0;
// Editor-visible lines (used by editor layout/scrolling)
int visibleLines = 0;
// Menu/list-visible lines (used by menus, Wi-Fi list, etc.)
int listVisibleLines = 0;

// Base font metrics for textSize = 1 (used to scale editor font)
int baseCharWidth = 0;
int baseLineHeight = 0;

// Cataracts mode used to choose the absolute largest possible text scale.
// That can create one visual line per character and can exhaust RAM on longer notes.
// Keep it very large, but bounded, so saved cataracts mode cannot boot-loop the device.
static const int CATARACTS_TEXT_SCALE = 6;

int firstVisibleLine = 0;  // Index into visualLines for top of screen

bool bufferDirty = false;
unsigned long lastKeyPressTime = 0;  // For idle autosave
bool savedAfterIdle = false;         // So we only save once per idle period
bool needsRedraw = true;             // To avoid constant redraw/flicker

// Track the last rendered screen so menus can avoid full-screen clears during simple cursor moves.
AppMode lastRenderedAppMode = MODE_EDITOR;
MenuId lastRenderedMenu = MENU_NONE;

// Cursor anchoring mode: true = keep cursor ~2/3 down; false = classic behaviour
bool cursorAnchored = true;

// Idle autosave toggle (controlled from Editor behaviour menu)
bool idleAutosaveEnabled = true;

// ---------- Battery / power management ----------
static const unsigned long BATTERY_POLL_INTERVAL_MS = 30000;  // poll every 30 seconds
static const int LOW_BATTERY_LEVEL = 15;                      // autosave + warning
static const int CRITICAL_BATTERY_LEVEL = 5;                  // autosave + urgent warning

int batteryLevelPercent = -1;
int batteryVoltageMv = -1;
unsigned long lastBatteryPollMs = 0;
bool lowBatteryWarningShown = false;
bool criticalBatteryWarningShown = false;

// Status message (for audio notes, sync messages, etc.)
String statusMessage;
unsigned long statusMessageUntil = 0;

// ---------- Key repeat tracking (editor only) ----------
enum ActionType {
  ACTION_NONE,
  ACTION_INSERT_CHAR,
  ACTION_BACKSPACE,
  ACTION_DELETE_FORWARD,
  ACTION_DELETE_WORD_BACK,
  ACTION_DELETE_WORD_FORWARD,
  ACTION_MOVE_LEFT,
  ACTION_MOVE_RIGHT,
  ACTION_MOVE_UP,
  ACTION_MOVE_DOWN,
  ACTION_WORD_LEFT,
  ACTION_WORD_RIGHT,
  ACTION_PAGE_UP,
  ACTION_PAGE_DOWN,
  ACTION_HOME,
  ACTION_END,
  ACTION_DOC_HOME,
  ACTION_DOC_END
};


struct KeyAction {
  ActionType type;
  char ch;  // used for ACTION_INSERT_CHAR
};

KeyAction lastAction = { ACTION_NONE, 0 };
bool haveLastAction = false;

unsigned long keyPressStartTime = 0;  // when current key was first pressed
unsigned long lastKeyActionTime = 0;  // last time we repeated the action
bool keyHeld = false;                 // have we already started repeating?

// ---------- Template info ----------
struct TemplateInfo {
  String name;
  String filePath;
  String targetDir;
  String filenamePattern;
};

// ---------- Audio notes browser data ----------

const int MAX_AUDIO_NOTES = 64;

struct AudioNoteEntry {
  String path;    // full path, e.g. "/journal/audio/2025-11-29_1842.wav"
  String name;    // display name, e.g. "2025-11-29_1842.wav"
  uint32_t size;  // bytes
};

// ---------- Document browser data ----------

const int MAX_DOCUMENTS = 128;

struct DocumentEntry {
  String path;         // full path, e.g. "/journal/daily/2025-11-29.txt"
  String displayName;  // relative label, e.g. "daily/2025-11-29.txt"
  uint32_t size;       // bytes
};

std::vector<TemplateInfo> templates;

// ---------- Wi-Fi & Google Drive sync ----------
const char *WIFI_CFG_PATH = "/journal/config/wifi.cfg";
const char *GDRIVE_CFG_PATH = "/journal/config/gdrive.cfg";
const char *APPEARANCE_CFG_PATH = "/journal/config/theme.cfg";
const char *BT_KEYBOARD_CFG_PATH = "/journal/config/btkeyboard.cfg";
const char *RTC_CFG_PATH = "/journal/config/rtc.txt";
const char *CONFIG_README_PATH = "/journal/config/CONFIG-README.txt";
const char *LAST_FILE_PATH = "/journal/config/lastfile.txt";
const char *USB_STORAGE_FLAG_PATH = "/journal/config/usb_storage_mode.flag";


// Placeholder values for auto-generated config templates
const char *WIFI_PLACEHOLDER_SSID = "YourSSIDHere";
const char *WIFI_PLACEHOLDER_PASS = "YourPasswordHere";

const char *GD_PLACEHOLDER_REFRESH = "REFRESH_TOKEN_HERE";
const char *GD_PLACEHOLDER_CLIENT = "CLIENT_ID_HERE";
const char *GD_PLACEHOLDER_SECRET = "CLIENT_SECRET_HERE";
const char *GD_PLACEHOLDER_FOLDER = "FOLDER_ID_HERE";

String wifiSsid;
String wifiPassword;
bool wifiConfigured = false;
bool wifiConnected = false;

// gdrive config
String gdriveRefreshToken;
String gdriveClientId;
String gdriveClientSecret;
String gdriveFolderId;

// ---------- WiFi setup UI ----------
enum WiFiSetupState {
  WIFI_STATE_LIST,
  WIFI_STATE_PWENTRY
};

WiFiSetupState wifiSetupState = WIFI_STATE_LIST;

struct WiFiNetwork {
  String ssid;
  int32_t rssi;
  bool secure;
};

std::vector<WiFiNetwork> wifiNetworks;
int wifiSelectedIndex = 0;
int wifiScrollOffset = 0;
String wifiPasswordInput;


// ---------- RTC / Date-time ----------
struct DateTime {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

DateTime gDateTime = { 2025, 1, 1, 0, 0, 0 };
bool rtcInitialised = false;
unsigned long rtcLastMillis = 0;

// Date/time editor state
int dtEditField = 0;  // 0=year,1=month,2=day,3=hour,4=minute

// ---------- Forward declarations ----------
void initDisplay();
void initSD();
void ensureJournalDirs();
void ensureConfigTemplates();
void ensureDefaultTemplates();
void loadTemplates();
void migrateLegacyRootJournalFile();
bool isDocumentPathUsable(const String &path);
void loadLastEditedFilePath();
void saveLastEditedFilePath();

void loadFromFile();
void saveToFile();
void rebuildLayout();
void computeCursorLineCol(int &outLine, int &outCol);
void ensureCursorVisible();
void applyEditorFontSize();
void handleKeyboard();
void handleKeyRepeat();
void handleBtKeyRepeat();
void setLastAction(ActionType t, char ch = 0);
void performLastAction();
void setCursorAnchorMode(bool anchored);
void applyEditorTheme(int themeIndex);
void insertChar(char c);
void insertText(const char *text);
void insertNewline();
void insertTab();
void backspaceChar();
void deleteForwardChar();
void deleteWordBackwards();
void deleteWordForwards();
void moveCursorLeft();
void moveCursorRight();
void moveCursorUp();
void moveCursorDown();
void moveCursorWordLeft();
void moveCursorWordRight();
void moveCursorHome();
void moveCursorEnd();
void moveCursorDocHome();
void moveCursorDocEnd();
void scrollPageUp();
void scrollPageDown();
void drawEditor();

void drawMenu();
size_t clampCursorIndex(size_t idx);
size_t colToIndexInLine(const VisualLine &line, int targetCol);

// Menu helpers
void openMainMenu();
void openMenu(MenuId menu);
void menuMoveUp();
void menuMoveDown();
void menuBack();
void menuSelect();
int getMenuItemCount(MenuId menu);
String getMenuItemLabel(MenuId menu, int index);
MenuId getMenuParent(MenuId menu);

// Templates
void createTemplateFileIfMissing(const char *path, const char *content);
void parseTemplateFile(const String &path);
String getCurrentDateString();
String getCurrentTimeString();
String getCurrentDateLongString();
String expandTemplateTokens(const String &in);
void createDocumentFromTemplateIndex(int idx);
int findTemplateIndexByName(const String &name);
void createNewBlankNote();
void createNewDailyEntry();

// Wi-Fi & sync
void loadWifiConfigFromSD();
bool connectWiFi();
void disconnectWiFi();
bool loadGDriveConfigFromSD();
bool getGoogleAccessToken(String &accessToken);
String jsonEscape(const String &value);
bool uploadFileToDriveMultipart(const String &accessToken, const String &path, const String &mimeType, bool showResultMessage);
bool syncGoogleDrive();
void runManualSync();
void saveWiFiConfig(const String &ssid, const String &password);
void runTimeSync();

// Appearance persistence
void loadAppearanceFromSD();
void saveAppearanceToSD();


// WiFi setup UI
void startWiFiScan();
void openWiFiSetup();
void handleWiFiSetupKeys(const Keyboard_Class::KeysState &ks);
void drawWiFiSetup();

// Bluetooth keyboard setup UI
void loadBluetoothKeyboardConfigFromSD();
void saveBluetoothKeyboardConfigToSD();
void forgetBluetoothKeyboardConfig();
void openBluetoothKeyboardSetup();
void handleBluetoothKeyboardSetupKeys(const Keyboard_Class::KeysState &ks);
void drawBluetoothKeyboardSetup();

// RTC
void initRTC();
void updateRTC();

// Battery / power management
void initPowerManagement();
void updateBatteryStatus(bool force = false);
String getBatteryStatusString();
void handleLowBattery();
void loadRTCFromSD();
void saveRTCToSD();
void advanceDateTime(unsigned long secs);
int daysInMonth(int year, int month);
bool isLeapYear(int year);

// Audio & document tools
void startAudioNote();
void startAudioNoteBrowser();
void openDocumentBrowser();

// Storage & SD tools
void showJournalStorageSummary();
void deleteAllJournalDocuments();
void deleteAllAudioNotes();
void runFactoryReset();

// Status message
void showStatusMessage(const String &msg, unsigned long durationMs);

// Bluetooth keyboard
void initBluetoothKeyboard();
void handleBluetoothInput();
void btKeyboardStartScan();
bool btKeyboardConnect();
void btKeyboardDisconnect();
void btProcessHidReports();

// Date-time menu helpers
void openDateTimeEditor();
void handleDateTimeEditorKeys(const Keyboard_Class::KeysState &ks);
void drawDateTimeEditor();

// About screen
void handleAboutScreenKeys(const Keyboard_Class::KeysState &ks);
void drawAboutScreen();

// USB mass storage
void initUsbStorageDevice(bool presentOnStart);
void enterUsbStorageMode();
void runUsbStorageBootMode();
void drawUsbStorageModeScreen();
static int32_t usbMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
static int32_t usbMscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
static bool usbMscStartStop(uint8_t power_condition, bool start, bool load_eject);


// ---------- Setup ----------

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // enableKeyboard = true
  initPowerManagement();

  initDisplay();
  initSD();
  ensureJournalDirs();

  // USB storage mode must be entered by rebooting into a dedicated mode so the host sees
  // the mass-storage interface during USB enumeration. Do this before normal app startup.
  if (SD.exists(USB_STORAGE_FLAG_PATH)) {
    runUsbStorageBootMode();
  }
  loadWifiConfigFromSD();
  loadAppearanceFromSD();  // <-- load saved theme, if any
  ensureDefaultTemplates();
  loadTemplates();
  initRTC();
  initBluetoothKeyboard();  // set up BLE stack for external keyboard (disabled by default)
  loadLastEditedFilePath();

  // Load the last edited/default document if it exists. On a fresh SD card, behave like
  // "New blank note" and create the first note in /journal/misc rather than /journal root.
  if (SD.exists(currentFilePath.c_str())) {
    loadFromFile();
  } else {
    createNewBlankNote();
  }

  cursorIndex = textBuffer.length();  // start at end of file
  rebuildLayout();
  ensureCursorVisible();

  lastKeyPressTime = millis();
  savedAfterIdle = false;
  needsRedraw = true;
}

// ---------- Main loop ----------

void loop() {
  updateRTC();
  M5Cardputer.update();
  updateBatteryStatus();
  handleLowBattery();
  handleKeyboard();
  handleBluetoothInput();  // process queued chars from BT keyboard
  handleKeyRepeat();       // built-in keyboard repeat
  handleBtKeyRepeat();     // Bluetooth keyboard repeat


  unsigned long now = millis();
  if (idleAutosaveEnabled && bufferDirty && !savedAfterIdle && (now - lastKeyPressTime) >= idleAutosaveDelayMs) {
    saveToFile();
    bufferDirty = false;
    savedAfterIdle = true;
    needsRedraw = true;
  }


  if (needsRedraw) {
    if (appMode == MODE_EDITOR) {
      drawEditor();
    } else {
      drawMenu();
    }
    needsRedraw = false;
  }

  delay(10);
}

// ---------- Initialisation helpers ----------

void initDisplay() {
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(BLACK);

  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int screenW = M5Cardputer.Display.width();
  int screenH = M5Cardputer.Display.height();

  int charWidth = M5Cardputer.Display.textWidth("M");
  lineHeight = M5Cardputer.Display.fontHeight();

  topBarHeight = lineHeight + 4;

  // Store base metrics for editor scaling (textSize = 1)
  baseCharWidth = charWidth;
  baseLineHeight = lineHeight;

  // Menus / lists (Wi-Fi, etc.) always use the small font height
  listVisibleLines = (screenH - topBarHeight) / lineHeight;
  if (listVisibleLines < 1) listVisibleLines = 1;

  // Set initial editor layout for current font size (standard)
  applyEditorFontSize();
}

void applyEditorFontSize() {
  // Scale mapping for editor layout:
  //   0 = standard (1×)
  //   1 = large (2×)
  //   2 = cataracts mode (special: one giant char per screen)
  if (baseCharWidth <= 0 || baseLineHeight <= 0) {
    // Fallback if somehow not initialised
    baseCharWidth = M5Cardputer.Display.textWidth("M");
    baseLineHeight = M5Cardputer.Display.fontHeight();
    if (baseCharWidth <= 0) baseCharWidth = 6;
    if (baseLineHeight <= 0) baseLineHeight = 8;
  }

  int screenW = M5Cardputer.Display.width();
  int screenH = M5Cardputer.Display.height();

  if (editorFontSizeIndex == 2) {
    // Cataracts mode: huge editor text, but bounded so wrapping does not create
    // one visual line per character on longer documents. It deliberately uses
    // a simple two-line viewport so text wraps while writing.
    int charWidth = baseCharWidth * CATARACTS_TEXT_SCALE;
    int editorLineH = baseLineHeight * CATARACTS_TEXT_SCALE;
    if (charWidth < 1) charWidth = 1;
    if (editorLineH < 1) editorLineH = 1;

    maxCols = (screenW - EDITOR_LEFT_MARGIN) / charWidth;
    if (maxCols < 1) maxCols = 1;

    visibleLines = (screenH - topBarHeight) / editorLineH;
    if (visibleLines < 1) visibleLines = 1;
    if (visibleLines > 2) visibleLines = 2;
    return;
  }

  float scale = (editorFontSizeIndex == 0) ? 1.0f : 1.4f;

  int charWidth = (int)(baseCharWidth * scale);
  int editorLineH = (int)(baseLineHeight * scale);

  if (charWidth < 1) charWidth = 1;
  if (editorLineH < 1) editorLineH = 1;

  // These are used by the editor layout/wrapping logic
  maxCols = (screenW - EDITOR_LEFT_MARGIN) / charWidth;
  if (maxCols < 1) maxCols = 1;

  visibleLines = (screenH - topBarHeight) / editorLineH;
  if (visibleLines < 1) visibleLines = 1;
}


void initPowerManagement() {
  // Keep the original battery-management behaviour. The audio issue was traced to
  // M5Stack board package 3.3.x; with board package 3.2.2 the mic works normally.
  M5Cardputer.Power.setBatteryCharge(true);
  updateBatteryStatus(true);
}

void updateBatteryStatus(bool force) {
  unsigned long now = millis();

  if (!force && (now - lastBatteryPollMs) < BATTERY_POLL_INTERVAL_MS) {
    return;
  }

  lastBatteryPollMs = now;

  int oldLevel = batteryLevelPercent;

  batteryLevelPercent = M5Cardputer.Power.getBatteryLevel();
  batteryVoltageMv = M5Cardputer.Power.getBatteryVoltage();

  if (batteryLevelPercent < 0) batteryLevelPercent = -1;
  if (batteryLevelPercent > 100) batteryLevelPercent = 100;

  // Refresh the editor top bar if the visible battery value changed.
  if (oldLevel != batteryLevelPercent && appMode == MODE_EDITOR) {
    needsRedraw = true;
  }
}

String getBatteryStatusString() {
  if (batteryLevelPercent < 0) {
    return "bat:?";
  }

  String s = "bat:";
  s += String(batteryLevelPercent);
  s += "%";

  if (batteryLevelPercent <= LOW_BATTERY_LEVEL) {
    s += "!";
  }

  return s;
}

void handleLowBattery() {
  if (batteryLevelPercent < 0) {
    return;
  }

  if (batteryLevelPercent <= CRITICAL_BATTERY_LEVEL && !criticalBatteryWarningShown) {
    if (bufferDirty) {
      saveToFile();
      bufferDirty = false;
      savedAfterIdle = true;
    }

    showStatusMessage("Critical battery - saved note", 4000);
    criticalBatteryWarningShown = true;
    lowBatteryWarningShown = true;
    needsRedraw = true;
    return;
  }

  if (batteryLevelPercent <= LOW_BATTERY_LEVEL && !lowBatteryWarningShown) {
    if (bufferDirty) {
      saveToFile();
      bufferDirty = false;
      savedAfterIdle = true;
    }

    showStatusMessage("Low battery - saved note", 3000);
    lowBatteryWarningShown = true;
    needsRedraw = true;
  }

  if (batteryLevelPercent > LOW_BATTERY_LEVEL) {
    lowBatteryWarningShown = false;
    criticalBatteryWarningShown = false;
  }
}


void initSD() {
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(0, 0);
    M5Cardputer.Display.println("SD init failed.");
    M5Cardputer.Display.println("Editing in RAM only.");
    delay(1500);
  }
}

void ensureJournalDirs() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  if (!SD.exists("/journal")) SD.mkdir("/journal");
  if (!SD.exists("/journal/config")) SD.mkdir("/journal/config");
  if (!SD.exists("/journal/templates")) SD.mkdir("/journal/templates");
  if (!SD.exists("/journal/daily")) SD.mkdir("/journal/daily");
  if (!SD.exists("/journal/notes")) SD.mkdir("/journal/notes");
  if (!SD.exists("/journal/meetings")) SD.mkdir("/journal/meetings");
  if (!SD.exists("/journal/projects")) SD.mkdir("/journal/projects");
  if (!SD.exists("/journal/travel")) SD.mkdir("/journal/travel");
  if (!SD.exists("/journal/misc")) SD.mkdir("/journal/misc");
  if (!SD.exists("/journal/archive")) SD.mkdir("/journal/archive");
  if (!SD.exists("/journal/audio")) SD.mkdir("/journal/audio");
  if (!SD.exists("/journal/audio/export")) SD.mkdir("/journal/audio/export");

  // After dirs exist, ensure config templates + README exist
  ensureConfigTemplates();
}

// Move legacy root-level config files into /journal/config so old SD cards keep working.
void migrateConfigFileIfNeeded(const char *oldPath, const char *newPath) {
  if (SD.exists(newPath)) return;
  if (!SD.exists(oldPath)) return;

  File oldFile = SD.open(oldPath, FILE_READ);
  if (!oldFile) return;

  File newFile = SD.open(newPath, FILE_WRITE);
  if (!newFile) {
    oldFile.close();
    return;
  }

  while (oldFile.available()) {
    newFile.write((uint8_t)oldFile.read());
  }

  oldFile.close();
  newFile.close();
  SD.remove(oldPath);
}

// Move the original root-level first note into /journal/misc for a tidy journal root.
void migrateLegacyRootJournalFile() {
  const char *oldPath = "/journal/journal.txt";
  if (!SD.exists(oldPath)) return;
  if (!SD.exists("/journal/misc")) SD.mkdir("/journal/misc");

  String newPath = "/journal/misc/journal.txt";
  if (SD.exists(newPath.c_str())) {
    int suffix = 1;
    do {
      newPath = "/journal/misc/journal-" + String(suffix) + ".txt";
      suffix++;
    } while (SD.exists(newPath.c_str()) && suffix < 1000);
  }

  File oldFile = SD.open(oldPath, FILE_READ);
  if (!oldFile) return;

  File newFile = SD.open(newPath.c_str(), FILE_WRITE);
  if (!newFile) {
    oldFile.close();
    return;
  }

  while (oldFile.available()) {
    newFile.write((uint8_t)oldFile.read());
  }

  oldFile.close();
  newFile.close();
  SD.remove(oldPath);

  // If this was the saved last-edited document, point the app at its new home.
  if (currentFilePath == oldPath) {
    currentFilePath = newPath;
    saveLastEditedFilePath();
  }
}

// Only accept document paths inside real journal document folders, not config/templates/root.
bool isDocumentPathUsable(const String &path) {
  if (path.length() == 0) return false;
  if (!path.startsWith("/journal/")) return false;
  if (!path.endsWith(".txt")) return false;
  if (path.startsWith("/journal/config/")) return false;
  if (path.startsWith("/journal/templates/")) return false;
  if (path.startsWith("/journal/audio/")) return false;
  if (path == "/journal/journal.txt") return false;
  return SD.exists(path.c_str());
}

void loadLastEditedFilePath() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) return;
  if (!SD.exists(LAST_FILE_PATH)) return;

  File f = SD.open(LAST_FILE_PATH, FILE_READ);
  if (!f) return;

  String path = f.readStringUntil('\n');
  f.close();
  path.trim();

  if (isDocumentPathUsable(path)) {
    currentFilePath = path;
  }
}

void saveLastEditedFilePath() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) return;
  if (!currentFilePath.startsWith("/journal/")) return;
  if (currentFilePath.startsWith("/journal/config/")) return;
  if (currentFilePath.startsWith("/journal/templates/")) return;

  SD.remove(LAST_FILE_PATH);
  File f = SD.open(LAST_FILE_PATH, FILE_WRITE);
  if (!f) return;
  f.println(currentFilePath);
  f.close();
}

// Create config files and CONFIG-README.txt if missing
void ensureConfigTemplates() {
  if (!SD.exists("/journal/config")) SD.mkdir("/journal/config");

  migrateConfigFileIfNeeded("/journal/wifi.cfg", WIFI_CFG_PATH);
  migrateConfigFileIfNeeded("/journal/gdrive.cfg", GDRIVE_CFG_PATH);
  migrateConfigFileIfNeeded("/journal/theme.cfg", APPEARANCE_CFG_PATH);
  migrateConfigFileIfNeeded("/journal/btkeyboard.cfg", BT_KEYBOARD_CFG_PATH);
  migrateConfigFileIfNeeded("/journal/rtc.txt", RTC_CFG_PATH);
  migrateConfigFileIfNeeded("/journal/CONFIG-README.txt", CONFIG_README_PATH);
  migrateConfigFileIfNeeded("/journal/lastfile.txt", LAST_FILE_PATH);
  migrateLegacyRootJournalFile();

  // wifi.cfg template
  if (!SD.exists(WIFI_CFG_PATH)) {
    File f = SD.open(WIFI_CFG_PATH, FILE_WRITE);
    if (f) {
      f.println(WIFI_PLACEHOLDER_SSID);
      f.println(WIFI_PLACEHOLDER_PASS);
      f.close();
    }
  }

  // gdrive.cfg template
  if (!SD.exists(GDRIVE_CFG_PATH)) {
    File f = SD.open(GDRIVE_CFG_PATH, FILE_WRITE);
    if (f) {
      f.println(GD_PLACEHOLDER_REFRESH);
      f.println(GD_PLACEHOLDER_CLIENT);
      f.println(GD_PLACEHOLDER_SECRET);
      f.println(GD_PLACEHOLDER_FOLDER);
      f.close();
    }
  }

  // CONFIG-README.txt with basic instructions
  if (!SD.exists(CONFIG_README_PATH)) {
    File f = SD.open(CONFIG_README_PATH, FILE_WRITE);
    if (f) {
      f.println("Cardputer Journal configuration");
      f.println("--------------------------------");
      f.println();
      f.println("This folder contains Tiny Journal config files.");
      f.println();
      f.println("1) WiFi config: wifi.cfg");
      f.println("   Path: /journal/config/wifi.cfg");
      f.println("   Format: two lines:");
      f.println("     Line 1: WiFi SSID");
      f.println("     Line 2: WiFi password");
      f.println();
      f.println("   Example:");
      f.println("     MyHomeWiFi");
      f.println("     supersecretpassword123");
      f.println();
      f.println("   The firmware creates wifi.cfg with placeholder values:");
      f.println("     ");
      f.println(String("     ") + WIFI_PLACEHOLDER_SSID);
      f.println(String("     ") + WIFI_PLACEHOLDER_PASS);
      f.println("   Replace both lines with your real SSID and password.");
      f.println("   Alternatively, you can configure WiFi on the device via:");
      f.println("     Fn + `  -> Settings -> Sync & connectivity -> WiFi setup");
      f.println();
      f.println("2) Google Drive config: gdrive.cfg");
      f.println("   Path: /journal/config/gdrive.cfg");
      f.println("   Format: four lines:");
      f.println("     Line 1: refresh_token");
      f.println("     Line 2: client_id");
      f.println("     Line 3: client_secret");
      f.println("     Line 4: folder_id (Drive folder to sync to)");
      f.println();
      f.println("   The firmware creates gdrive.cfg with placeholder values:");
      f.println(String("     ") + GD_PLACEHOLDER_REFRESH);
      f.println(String("     ") + GD_PLACEHOLDER_CLIENT);
      f.println(String("     ") + GD_PLACEHOLDER_SECRET);
      f.println(String("     ") + GD_PLACEHOLDER_FOLDER);
      f.println();
      f.println("   Replace these with real values from your Google Cloud project.");
      f.println("   The current firmware uses them to test connectivity to Google Drive.");
      f.println();
      f.println("3) Testing from the device");
      f.println("   On the Cardputer:");
      f.println("     Fn + `");
      f.println("     -> Settings");
      f.println("     -> Sync & connectivity");
      f.println("     -> Sync now");
      f.println();
      f.println("   Status messages will show if WiFi/auth succeed or fail.");
      f.close();
    }
  }
}

// ---------- Default templates on SD ----------

const char *TEMPLATE_DAILY =
  R"(#@name: Daily Journal
#@targetDir: /journal/daily
#@filename: {{DATE}}.txt

{{DATE_LONG}}


)";

const char *TEMPLATE_QUICK =
  R"(#@name: Quick Note
#@targetDir: /journal/notes
#@filename: note-{{DATE}}-{{TIME}}.txt

{{DATE}} {{TIME}}


)";

const char *TEMPLATE_MEETING =
  R"(#@name: Meeting Notes
#@targetDir: /journal/meetings
#@filename: meeting-{{DATE}}-{{TIME}}.txt

{{DATE_LONG}}
Time: {{TIME}}

Attendees:
-

Notes:
-

Action points:
-
)";

const char *TEMPLATE_PROJECT =
  R"(#@name: Project Log
#@targetDir: /journal/projects
#@filename: project-{{DATE}}.txt

{{DATE_LONG}}

Project:
Today:
-

Next:
-
)";

const char *TEMPLATE_TRAVEL =
  R"(#@name: Travel Log
#@targetDir: /journal/travel
#@filename: travel-{{DATE}}.txt

{{DATE_LONG}}

Location:
Weather:
Highlights:
-
)";

void createTemplateFileIfMissing(const char *path, const char *content) {
  if (!SD.exists(path)) {
    File f = SD.open(path, FILE_WRITE);
    if (f) {
      f.print(content);
      f.close();
    }
  }
}

void ensureDefaultTemplates() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }
  if (!SD.exists("/journal/templates")) SD.mkdir("/journal/templates");

  createTemplateFileIfMissing("/journal/templates/daily.tpl", TEMPLATE_DAILY);
  createTemplateFileIfMissing("/journal/templates/quick.tpl", TEMPLATE_QUICK);
  createTemplateFileIfMissing("/journal/templates/meeting.tpl", TEMPLATE_MEETING);
  createTemplateFileIfMissing("/journal/templates/project.tpl", TEMPLATE_PROJECT);
  createTemplateFileIfMissing("/journal/templates/travel.tpl", TEMPLATE_TRAVEL);
}

void loadTemplates() {
  templates.clear();

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  File dir = SD.open("/journal/templates");
  if (!dir) return;

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String path = String("/journal/templates/") + String(f.name());
      parseTemplateFile(path);
    }
    f.close();
  }
  dir.close();
}

// ---------- File I/O ----------

void loadFromFile() {
  textBuffer = "";

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  File f = SD.open(currentFilePath.c_str(), FILE_READ);
  if (!f) {
    return;
  }

  while (f.available()) {
    textBuffer += (char)f.read();
  }
  f.close();
}

void saveToFile() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  SD.remove(currentFilePath.c_str());
  File f = SD.open(currentFilePath.c_str(), FILE_WRITE);
  if (!f) {
    return;
  }

  for (size_t i = 0; i < (size_t)textBuffer.length(); ++i) {
    f.write((uint8_t)textBuffer[i]);
  }
  f.close();

  // Also persist current RTC time and current document path whenever we save
  saveRTCToSD();
  saveLastEditedFilePath();
}

// ---------- Template parsing & instantiation ----------

void parseTemplateFile(const String &path) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return;

  String content;
  while (f.available()) {
    content += (char)f.read();
  }
  f.close();

  TemplateInfo info;
  info.filePath = path;
  info.name = "";
  info.targetDir = "";
  info.filenamePattern = "";

  int pos = 0;
  int len = content.length();

  while (pos < len) {
    int eol = content.indexOf('\n', pos);
    if (eol < 0) eol = len;
    String line = content.substring(pos, eol);
    line.trim();
    if (!line.startsWith("#@")) {
      break;
    }

    if (line.startsWith("#@name:")) {
      String v = line.substring(7);
      v.trim();
      info.name = v;
    } else if (line.startsWith("#@targetDir:")) {
      String v = line.substring(12);
      v.trim();
      info.targetDir = v;
    } else if (line.startsWith("#@filename:")) {
      String v = line.substring(11);
      v.trim();
      info.filenamePattern = v;
    }

    pos = eol + 1;
  }

  if (info.name.length() == 0) {
    int slash = path.lastIndexOf('/');
    String base = (slash >= 0) ? path.substring(slash + 1) : path;
    info.name = base;
  }
  if (info.targetDir.length() == 0) {
    info.targetDir = "/journal/misc";
  }
  if (info.filenamePattern.length() == 0) {
    info.filenamePattern = "note-{{DATE}}-{{TIME}}.txt";
  }

  templates.push_back(info);
}

// Date/time tokens from RTC
String twoDigits(int v) {
  if (v < 10) return "0" + String(v);
  return String(v);
}

String getCurrentDateString() {
  if (!rtcInitialised) return "0000-00-00";
  return String(gDateTime.year) + "-" + twoDigits(gDateTime.month) + "-" + twoDigits(gDateTime.day);
}

// This is used in filenames AND text; keep it colon-free for FAT safety
String getCurrentTimeString() {
  if (!rtcInitialised) return "000000";
  return twoDigits(gDateTime.hour) + twoDigits(gDateTime.minute) + twoDigits(gDateTime.second);
}


String getCurrentDateLongString() {
  if (!rtcInitialised) return "Date unknown";
  return getCurrentDateString();
}

String expandTemplateTokens(const String &in) {
  String out = in;
  out.replace("{{DATE}}", getCurrentDateString());
  out.replace("{{TIME}}", getCurrentTimeString());
  out.replace("{{DATE_LONG}}", getCurrentDateLongString());
  return out;
}

void createDocumentFromTemplateIndex(int idx) {
  if (idx < 0 || idx >= (int)templates.size()) return;
  TemplateInfo &info = templates[idx];

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  if (!SD.exists(info.targetDir.c_str())) {
    SD.mkdir(info.targetDir.c_str());
  }

  String filename = expandTemplateTokens(info.filenamePattern);
  String dir = info.targetDir;
  if (!dir.endsWith("/")) dir += "/";

  // Build a slug from the template name, e.g. "Daily journal" -> "daily-journal"
  String tplName = info.name;
  String slug = "";
  for (int i = 0; i < tplName.length(); ++i) {
    char ch = tplName[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      // normalise to lowercase letters/numbers
      if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
      slug += ch;
    } else {
      // treat any non-alnum as separator
      if (slug.length() > 0 && slug[slug.length() - 1] != '-') {
        slug += '-';
      }
    }
  }
  // Trim trailing '-' if any
  while (slug.length() > 0 && slug[slug.length() - 1] == '-') {
    slug.remove(slug.length() - 1);
  }
  if (slug.length() == 0) {
    slug = "template";
  }

  // Split expanded pattern into base and extension, e.g. "2025-11-29" + ".txt"
  String baseName = filename;
  String ext = "";
  int dotPos = filename.lastIndexOf('.');
  if (dotPos > 0) {
    baseName = filename.substring(0, dotPos);
    ext = filename.substring(dotPos);
  }

  // Prefix with template slug, e.g. "daily-journal-2025-11-29"
  String baseWithPrefix = slug + "-" + baseName;

  // Always suffix with -1, -2, ... to avoid overwrite
  int suffix = 1;
  String candidate = baseWithPrefix + "-" + String(suffix) + ext;
  String path = dir + candidate;

  while (SD.exists(path.c_str()) && suffix < 1000) {
    suffix++;
    candidate = baseWithPrefix + "-" + String(suffix) + ext;
    path = dir + candidate;
  }


  File f = SD.open(info.filePath.c_str(), FILE_READ);
  if (!f) return;

  String content;
  while (f.available()) {
    content += (char)f.read();
  }
  f.close();

  int pos = 0;
  int len = content.length();
  while (pos < len) {
    int eol = content.indexOf('\n', pos);
    if (eol < 0) eol = len;
    String line = content.substring(pos, eol);
    String trimmed = line;
    trimmed.trim();
    if (!trimmed.startsWith("#@")) {
      break;
    }
    pos = eol + 1;
  }

  String body = content.substring(pos);
  body = expandTemplateTokens(body);

  currentFilePath = path;
  saveLastEditedFilePath();
  textBuffer = body;
  cursorIndex = textBuffer.length();
  bufferDirty = true;
  savedAfterIdle = false;

  rebuildLayout();
  ensureCursorVisible();
  appMode = MODE_EDITOR;
  needsRedraw = true;
}

int findTemplateIndexByName(const String &name) {
  for (int i = 0; i < (int)templates.size(); ++i) {
    if (templates[i].name == name) return i;
  }
  return -1;
}

void createNewBlankNote() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }
  String dir = "/journal/misc";
  if (!SD.exists(dir.c_str())) {
    SD.mkdir(dir.c_str());
  }
  unsigned long t = millis();
  String filename = "note-" + String(t) + ".txt";
  String path = dir + "/" + filename;

  currentFilePath = path;
  saveLastEditedFilePath();
  textBuffer = "";
  cursorIndex = 0;
  bufferDirty = true;
  savedAfterIdle = false;

  rebuildLayout();
  ensureCursorVisible();
  appMode = MODE_EDITOR;
  needsRedraw = true;
}

void createNewDailyEntry() {
  int idx = findTemplateIndexByName("Daily Journal");
  if (idx < 0) {
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
      return;
    }
    String dir = "/journal/daily";
    if (!SD.exists(dir.c_str())) SD.mkdir(dir.c_str());
    unsigned long t = millis();
    String filename = "daily-" + String(t) + ".txt";
    String path = dir + "/" + filename;
    currentFilePath = path;
    saveLastEditedFilePath();
    textBuffer = "";
    cursorIndex = 0;
    bufferDirty = true;
    savedAfterIdle = false;
    rebuildLayout();
    ensureCursorVisible();
    appMode = MODE_EDITOR;
    needsRedraw = true;
  } else {
    createDocumentFromTemplateIndex(idx);
  }
}

// ---------- RTC implementation ----------

bool isLeapYear(int year) {
  if (year % 400 == 0) return true;
  if (year % 100 == 0) return false;
  return (year % 4 == 0);
}

int daysInMonth(int year, int month) {
  switch (month) {
    case 1: return 31;
    case 2: return isLeapYear(year) ? 29 : 28;
    case 3: return 31;
    case 4: return 30;
    case 5: return 31;
    case 6: return 30;
    case 7: return 31;
    case 8: return 31;
    case 9: return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 30;
  }
}

void loadRTCFromSD() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    rtcInitialised = false;
    return;
  }

  if (!SD.exists(RTC_CFG_PATH)) {
    rtcInitialised = true;  // start from default
    rtcLastMillis = millis();
    return;
  }

  File f = SD.open(RTC_CFG_PATH, FILE_READ);
  if (!f) {
    rtcInitialised = true;
    rtcLastMillis = millis();
    return;
  }

  String line = f.readStringUntil('\n');
  f.close();
  line.trim();

  // Expect "YYYY-MM-DD HH:MM:SS"
  if (line.length() < 19) {
    rtcInitialised = true;
    rtcLastMillis = millis();
    return;
  }

  int y = line.substring(0, 4).toInt();
  int mo = line.substring(5, 7).toInt();
  int d = line.substring(8, 10).toInt();
  int h = line.substring(11, 13).toInt();
  int mi = line.substring(14, 16).toInt();
  int s = line.substring(17, 19).toInt();

  if (y < 2000 || y > 2099) y = 2025;
  if (mo < 1 || mo > 12) mo = 1;
  int dim = daysInMonth(y, mo);
  if (d < 1 || d > dim) d = 1;
  if (h < 0 || h > 23) h = 0;
  if (mi < 0 || mi > 59) mi = 0;
  if (s < 0 || s > 59) s = 0;

  gDateTime.year = y;
  gDateTime.month = mo;
  gDateTime.day = d;
  gDateTime.hour = h;
  gDateTime.minute = mi;
  gDateTime.second = s;

  rtcInitialised = true;
  rtcLastMillis = millis();
}

void saveRTCToSD() {
  if (!rtcInitialised) return;
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  SD.remove(RTC_CFG_PATH);
  File f = SD.open(RTC_CFG_PATH, FILE_WRITE);
  if (!f) return;

  String line = String(gDateTime.year) + "-" + twoDigits(gDateTime.month) + "-" + twoDigits(gDateTime.day) + " " + twoDigits(gDateTime.hour) + ":" + twoDigits(gDateTime.minute) + ":" + twoDigits(gDateTime.second);

  f.println(line);
  f.close();
}

void advanceDateTime(unsigned long secs) {
  while (secs > 0) {
    secs--;
    gDateTime.second++;
    if (gDateTime.second >= 60) {
      gDateTime.second = 0;
      gDateTime.minute++;
      if (gDateTime.minute >= 60) {
        gDateTime.minute = 0;
        gDateTime.hour++;
        if (gDateTime.hour >= 24) {
          gDateTime.hour = 0;
          gDateTime.day++;
          int dim = daysInMonth(gDateTime.year, gDateTime.month);
          if (gDateTime.day > dim) {
            gDateTime.day = 1;
            gDateTime.month++;
            if (gDateTime.month > 12) {
              gDateTime.month = 1;
              gDateTime.year++;
              if (gDateTime.year > 2099) gDateTime.year = 2000;
            }
          }
        }
      }
    }
  }
}

void initRTC() {
  loadRTCFromSD();
}

void updateRTC() {
  if (!rtcInitialised) return;
  unsigned long now = millis();
  unsigned long diff = now - rtcLastMillis;
  if (diff >= 1000) {
    unsigned long secs = diff / 1000;
    rtcLastMillis += secs * 1000;
    advanceDateTime(secs);
  }
}

// ---------- Wi-Fi & Google Drive sync implementation ----------

void loadWifiConfigFromSD() {
  wifiSsid = "";
  wifiPassword = "";
  wifiConfigured = false;

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  if (!SD.exists(WIFI_CFG_PATH)) {
    return;
  }

  File f = SD.open(WIFI_CFG_PATH, FILE_READ);
  if (!f) {
    return;
  }

  wifiSsid = f.readStringUntil('\n');
  wifiSsid.trim();
  wifiPassword = f.readStringUntil('\n');
  wifiPassword.trim();
  f.close();

  // Treat placeholder content as "not configured"
  if (wifiSsid.length() == 0 || wifiSsid == WIFI_PLACEHOLDER_SSID) {
    wifiConfigured = false;
    return;
  }

  wifiConfigured = true;
}

void saveWiFiConfig(const String &ssid, const String &password) {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  SD.remove(WIFI_CFG_PATH);
  File f = SD.open(WIFI_CFG_PATH, FILE_WRITE);
  if (!f) return;

  f.println(ssid);
  f.println(password);
  f.close();
}

bool connectWiFi() {
  if (wifiConnected) return true;
  if (!wifiConfigured) {
    showStatusMessage("WiFi not configured", 2000);
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

  const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return true;
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
    return false;
  }
}

void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
}

void loadAppearanceFromSD() {
  // Default theme (0) and font size (0) are already set by globals;
  // this only overrides if the config file exists.
  // Assumes SD has already been initialised by initSD().

  if (!SD.exists(APPEARANCE_CFG_PATH)) {
    return;
  }

  File f = SD.open(APPEARANCE_CFG_PATH, FILE_READ);
  if (!f) return;

  // Line 1: theme index (0–3), kept for backwards compatibility.
  String line1 = f.readStringUntil('\n');
  line1.trim();

  // Line 2: font size index (0–2), optional for older files.
  String line2 = f.readStringUntil('\n');
  line2.trim();

  f.close();

  if (line1.length() > 0) {
    int themeIdx = line1.toInt();
    if (themeIdx >= 0 && themeIdx <= 3) {
      applyEditorTheme(themeIdx);
    }
  }

  if (line2.length() > 0) {
    int fontIdx = line2.toInt();
    if (fontIdx >= 0 && fontIdx <= 2) {
      editorFontSizeIndex = fontIdx;
      applyEditorFontSize();
    }
  }
}

void saveAppearanceToSD() {
  // Assumes SD has already been initialised by initSD().

  SD.remove(APPEARANCE_CFG_PATH);
  File f = SD.open(APPEARANCE_CFG_PATH, FILE_WRITE);
  if (!f) return;

  // Store both theme and font size so appearance persists between power cycles.
  f.println(editorThemeIndex);
  f.println(editorFontSizeIndex);
  f.close();
}


// Load /journal/config/gdrive.cfg (refresh_token, client_id, client_secret, folder_id)
bool loadGDriveConfigFromSD() {
  gdriveRefreshToken = "";
  gdriveClientId = "";
  gdriveClientSecret = "";
  gdriveFolderId = "";

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return false;
  }

  if (!SD.exists(GDRIVE_CFG_PATH)) {
    return false;
  }

  File f = SD.open(GDRIVE_CFG_PATH, FILE_READ);
  if (!f) {
    return false;
  }

  gdriveRefreshToken = f.readStringUntil('\n');
  gdriveRefreshToken.trim();
  gdriveClientId = f.readStringUntil('\n');
  gdriveClientId.trim();
  gdriveClientSecret = f.readStringUntil('\n');
  gdriveClientSecret.trim();
  gdriveFolderId = f.readStringUntil('\n');
  gdriveFolderId.trim();
  f.close();

  // Detect placeholder entries as "not configured"
  if (gdriveRefreshToken == GD_PLACEHOLDER_REFRESH || gdriveClientId == GD_PLACEHOLDER_CLIENT || gdriveClientSecret == GD_PLACEHOLDER_SECRET) {
    return false;
  }

  if (gdriveRefreshToken.length() == 0 || gdriveClientId.length() == 0 || gdriveClientSecret.length() == 0) {
    return false;
  }

  return true;
}

// Get access token from refresh token using Google's OAuth endpoint
bool getGoogleAccessToken(String &accessToken) {
  accessToken = "";

  if (!loadGDriveConfigFromSD()) {
    showStatusMessage("gdrive cfg missing", 3000);
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();  // rely on WiFi + local device; avoids cert hassle

  HTTPClient https;
  if (!https.begin(client, "https://oauth2.googleapis.com/token")) {
    showStatusMessage("Auth: begin failed", 3000);
    return false;
  }

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "client_id=" + gdriveClientId + "&client_secret=" + gdriveClientSecret + "&refresh_token=" + gdriveRefreshToken + "&grant_type=refresh_token";

  int httpCode = https.POST(body);
  if (httpCode != 200) {
    https.end();
    showStatusMessage("Auth HTTP err " + String(httpCode), 3000);
    return false;
  }

  String payload = https.getString();
  https.end();

  int idx = payload.indexOf("\"access_token\"");
  if (idx < 0) {
    showStatusMessage("Auth parse error", 3000);
    return false;
  }
  idx = payload.indexOf(':', idx);
  if (idx < 0) {
    showStatusMessage("Auth parse error", 3000);
    return false;
  }
  idx = payload.indexOf('"', idx);
  if (idx < 0) {
    showStatusMessage("Auth parse error", 3000);
    return false;
  }
  int end = payload.indexOf('"', idx + 1);
  if (end < 0) {
    showStatusMessage("Auth parse error", 3000);
    return false;
  }

  accessToken = payload.substring(idx + 1, end);
  accessToken.trim();

  if (accessToken.length() == 0) {
    showStatusMessage("Auth token empty", 3000);
    return false;
  }

  return true;
}


String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }

  return out;
}

bool uploadFileToDriveMultipart(const String &accessToken, const String &path, const String &mimeType, bool showResultMessage) {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    if (showResultMessage) showStatusMessage("Upload: SD error", 3000);
    return false;
  }

  if (!SD.exists(path.c_str())) {
    if (showResultMessage) showStatusMessage("Upload: file missing", 3000);
    return false;
  }

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) {
    if (showResultMessage) showStatusMessage("Upload: open failed", 3000);
    return false;
  }

  String filename = path;
  int slashPos = filename.lastIndexOf('/');
  if (slashPos >= 0 && slashPos < (int)filename.length() - 1) {
    filename = filename.substring(slashPos + 1);
  }

  String boundary = "----cardputer_journal_boundary";
  String meta = "{\"name\":\"" + jsonEscape(filename) + "\"";
  if (gdriveFolderId.length() > 0) {
    meta += ",\"parents\":[\"" + jsonEscape(gdriveFolderId) + "\"]";
  }
  meta += "}";

  String bodyStart;
  bodyStart += "--" + boundary + "\r\n";
  bodyStart += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
  bodyStart += meta + "\r\n";
  bodyStart += "--" + boundary + "\r\n";
  bodyStart += "Content-Type: " + mimeType + "\r\n\r\n";

  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  uint32_t fileSize = f.size();
  uint32_t contentLength = bodyStart.length() + fileSize + bodyEnd.length();

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("www.googleapis.com", 443)) {
    f.close();
    if (showResultMessage) showStatusMessage("Upload: connect failed", 3000);
    return false;
  }

  client.print("POST /upload/drive/v3/files?uploadType=multipart HTTP/1.1\r\n");
  client.print("Host: www.googleapis.com\r\n");
  client.print("Authorization: Bearer ");
  client.print(accessToken);
  client.print("\r\n");
  client.print("Content-Type: multipart/related; boundary=");
  client.print(boundary);
  client.print("\r\n");
  client.print("Content-Length: ");
  client.print(contentLength);
  client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(bodyStart);

  uint8_t buf[1024];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    client.write(buf, n);
    delay(0);
  }
  f.close();

  client.print(bodyEnd);

  unsigned long startWait = millis();
  while (client.connected() && !client.available() && millis() - startWait < 10000) {
    delay(10);
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();

  int httpCode = 0;
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace >= 0 && firstSpace + 4 <= (int)statusLine.length()) {
    httpCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
  }

  // Drain a little of the response so TLS/socket cleanup is tidy, but don't waste RAM storing it.
  unsigned long drainStart = millis();
  while ((client.connected() || client.available()) && millis() - drainStart < 3000) {
    while (client.available()) {
      client.read();
    }
    delay(1);
  }
  client.stop();

  if (httpCode == 200 || httpCode == 201) {
    if (showResultMessage) showStatusMessage("Uploaded " + filename, 3000);
    return true;
  }

  if (showResultMessage) showStatusMessage("Upload err " + String(httpCode), 3000);
  return false;
}

bool uploadCurrentFileToDrive(const String &accessToken) {
  return uploadFileToDriveMultipart(accessToken, currentFilePath, "text/plain; charset=UTF-8", true);
}

// Upload all .txt docs in a given directory (non-recursive)
static void syncDocsInDir(const char *dirPath, const String &accessToken, bool &allOk, int &docCount) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      String name = String(f.name());
      String lower = name;
      lower.toLowerCase();

      bool isTxt = lower.endsWith(".txt");
      bool isRtc = (lower == "rtc.txt");
      bool isCfgRead = (lower == "config-readme.txt");
      bool isExcluded = isRtc || isCfgRead;

      if (isTxt && !isExcluded) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += name;

        if (!uploadFileToDriveMultipart(accessToken, fullPath, "text/plain; charset=UTF-8", true)) {
          allOk = false;
        }

        docCount++;
        // Give WiFi / TLS stack a moment
        delay(10);
      }
    }

    f.close();
  }

  dir.close();
}

// Upload all .wav audio notes in a given directory (non-recursive)
static void syncAudioInDir(const char *dirPath, const String &accessToken, bool &allOk, int &audioCount) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      String name = String(f.name());
      String lower = name;
      lower.toLowerCase();

      if (lower.endsWith(".wav")) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += name;

        if (!uploadFileToDriveMultipart(accessToken, fullPath, "audio/wav", true)) {
          allOk = false;
        }

        audioCount++;
        delay(10);
      }
    }

    f.close();
  }

  dir.close();
}


// Simple Drive check: call /drive/v3/about with access token
bool syncGoogleDrive() {
  String accessToken;
  if (!getGoogleAccessToken(accessToken)) {
    // getGoogleAccessToken already shows an error
    return false;
  }

  // Make sure SD is available
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 3000);
    return false;
  }

  // Remember current editor file so we can restore it afterwards
  String originalPath = currentFilePath;

  bool allOk = true;
  int docCount = 0;
  int audioCount = 0;

  syncDocsInDir("/journal/daily", accessToken, allOk, docCount);
  syncDocsInDir("/journal/notes", accessToken, allOk, docCount);
  syncDocsInDir("/journal/meetings", accessToken, allOk, docCount);
  syncDocsInDir("/journal/projects", accessToken, allOk, docCount);
  syncDocsInDir("/journal/travel", accessToken, allOk, docCount);
  syncDocsInDir("/journal/misc", accessToken, allOk, docCount);
  syncDocsInDir("/journal/archive", accessToken, allOk, docCount);
  syncAudioInDir("/journal/audio", accessToken, allOk, audioCount);

  // Restore editor state path
  currentFilePath = originalPath;

  if (docCount == 0 && audioCount == 0) {
    showStatusMessage("No files to sync", 2000);
    return true;  // nothing to do, but not an error
  }

  return allOk;
}


void runManualSync() {
  showStatusMessage("Sync: connecting WiFi...", 2000);
  needsRedraw = true;

  if (!connectWiFi()) {
    showStatusMessage("Sync failed: WiFi error", 3000);
    return;
  }

  bool ok = syncGoogleDrive();

  disconnectWiFi();

  if (ok) {
    showStatusMessage("Sync complete, WiFi off", 3000);
  } else {
    showStatusMessage("Sync failed, WiFi off", 3000);
  }
}

// WiFi time sync
void runTimeSync() {
  showStatusMessage("Time sync: connecting...", 2000);
  needsRedraw = true;

  if (!connectWiFi()) {
    showStatusMessage("Unable to connect", 3000);
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  bool got = false;
  const unsigned long TIME_SYNC_TIMEOUT_MS = 15000;
  unsigned long start = millis();

  while (millis() - start < TIME_SYNC_TIMEOUT_MS) {
    if (getLocalTime(&timeinfo, 1000)) {
      got = true;
      break;
    }
  }

  if (!got) {
    disconnectWiFi();
    showStatusMessage("Time sync failed", 3000);
    return;
  }

  gDateTime.year = timeinfo.tm_year + 1900;
  gDateTime.month = timeinfo.tm_mon + 1;
  gDateTime.day = timeinfo.tm_mday;
  gDateTime.hour = timeinfo.tm_hour;
  gDateTime.minute = timeinfo.tm_min;
  gDateTime.second = timeinfo.tm_sec;
  rtcInitialised = true;
  rtcLastMillis = millis();
  saveRTCToSD();

  disconnectWiFi();
  showStatusMessage("Time synced, WiFi off", 3000);
}

// ---------- Bluetooth keyboard (experimental, BLE HID host) ----------

// State flags and metadata
bool btKeyboardEnabled = false;
bool btKeyboardConnected = false;
String btKeyboardName = "";
String btKeyboardAddress = "";
String btKeyboardSavedAddress = "";
String btKeyboardSavedName = "";
bool btKeyboardConfigLoaded = false;
String btKeyboardLastError = "";

// BLE state shown on the Bluetooth keyboard setup screen.
static bool btInitDone = false;
String btAuthDebug = "";

// Simple state for the Bluetooth keyboard setup UI
enum BtSetupState {
  BT_STATE_IDLE,
  BT_STATE_SCANNING,
  BT_STATE_CONNECTED
};

BtSetupState btSetupState = BT_STATE_IDLE;

// Layout for external Bluetooth keyboard
enum BtKeyboardLayout {
  BT_LAYOUT_US = 0,
  BT_LAYOUT_UK = 1
};

// Default to UK layout for external keyboards
BtKeyboardLayout btKeyboardLayout = BT_LAYOUT_UK;


void loadBluetoothKeyboardConfigFromSD() {
  if (btKeyboardConfigLoaded) return;
  btKeyboardConfigLoaded = true;

  btKeyboardSavedAddress = "";
  btKeyboardSavedName = "";

  if (!SD.exists(BT_KEYBOARD_CFG_PATH)) {
    return;
  }

  File f = SD.open(BT_KEYBOARD_CFG_PATH, FILE_READ);
  if (!f) return;

  // Line 1: saved keyboard BLE address
  btKeyboardSavedAddress = f.readStringUntil('\n');
  btKeyboardSavedAddress.trim();

  // Line 2: saved keyboard name
  btKeyboardSavedName = f.readStringUntil('\n');
  btKeyboardSavedName.trim();

  // Line 3: layout index
  String layoutLine = f.readStringUntil('\n');
  layoutLine.trim();

  f.close();

  if (layoutLine.length() > 0) {
    int layoutIdx = layoutLine.toInt();
    if (layoutIdx == BT_LAYOUT_US || layoutIdx == BT_LAYOUT_UK) {
      btKeyboardLayout = (BtKeyboardLayout)layoutIdx;
    }
  }
}

void saveBluetoothKeyboardConfigToSD() {
  SD.remove(BT_KEYBOARD_CFG_PATH);
  File f = SD.open(BT_KEYBOARD_CFG_PATH, FILE_WRITE);
  if (!f) return;

  f.println(btKeyboardSavedAddress);
  f.println(btKeyboardSavedName);
  f.println((int)btKeyboardLayout);
  f.close();
}

void forgetBluetoothKeyboardConfig() {
  // Forget Tiny Journal's saved keyboard preference.
  btKeyboardSavedAddress = "";
  btKeyboardSavedName = "";
  btKeyboardAddress = "";
  SD.remove(BT_KEYBOARD_CFG_PATH);

  // Also clear the ESP32/NimBLE bond store. This is deliberately all-bonds
  // rather than address-specific: some keyboards use resolvable/random BLE
  // addresses, so a saved text address may not match the stored bond exactly.
  if (btInitDone || NimBLEDevice::isInitialized()) {
    int bondCount = NimBLEDevice::getNumBonds();
    bool ok = NimBLEDevice::deleteAllBonds();
    btAuthDebug = ok ? ("bonds cleared " + String(bondCount)) : "bond clear failed";
  } else {
    btAuthDebug = "keyboard forgotten";
  }
}


// List of devices found in the last scan for display
const int BT_MAX_SCAN_DEVICES = 10;
int btScanDeviceCount = 0;
String btScanDeviceNames[BT_MAX_SCAN_DEVICES];
bool btScanDeviceIsHid[BT_MAX_SCAN_DEVICES];


// Internal BLE objects
static NimBLEAdvertisedDevice *btKeyboardAdvDevice = nullptr;
static NimBLEClient *btKeyboardClient = nullptr;
static NimBLERemoteCharacteristic *btKeyboardReportChar = nullptr;
// Debug counters / status
volatile uint32_t btReportCount = 0;  // how many HID notifications we've seen


// BLE HID report queue.
// The notification callback must stay tiny: it copies the incoming report
// into this queue and returns. The main loop does the decoding safely.
struct BtHidReport {
  uint8_t data[8];
  uint8_t length;
};

static const int BT_HID_REPORT_QUEUE_SIZE = 16;
static QueueHandle_t btHidReportQueue = nullptr;
volatile uint32_t btDroppedReportCount = 0;
uint8_t btSubscribedReportCount = 0;
String btReportDebug = "";
// Last normalised HID report so we can detect newly-pressed keys.
// Normalised format used by Tiny Journal:
//   [0]    = modifiers
//   [1..6] = up to 6 key usages
//   [7]    = unused
static uint8_t btLastReport[8] = { 0 };

// True when subscribed to Boot Keyboard Input Report 0x2A22.
// False when subscribed to standard HID Report characteristic 0x2A4D.
static bool btUsingBootReport = false;

// Simple tracking for BT key repeat
bool btAnyKeyDown = false;
unsigned long btKeyPressStartTime = 0;
unsigned long btLastKeyActionTime = 0;
bool btKeyHeld = false;
bool btRepeatEligible = false;

void btClearInputEventQueue() {
  btEventHead = 0;
  btEventTail = 0;
}

bool btQueuePushEvent(const BtInputEvent &event) {
  int next = (btEventHead + 1) % BT_INPUT_EVENT_QUEUE_SIZE;
  if (next == btEventTail) {
    // queue full, drop event
    return false;
  }

  btEventQueue[btEventHead] = event;
  btEventHead = next;
  return true;
}

bool btQueuePopEvent(BtInputEvent &event) {
  if (btEventHead == btEventTail) {
    return false;
  }

  event = btEventQueue[btEventTail];
  btEventTail = (btEventTail + 1) % BT_INPUT_EVENT_QUEUE_SIZE;
  return true;
}

void btMakeEmptyEvent(BtInputEvent &event, uint8_t key, uint8_t mods) {
  event.type = BT_EVENT_NONE;
  event.text[0] = '\0';
  event.command = BT_CMD_NONE;
  event.hidUsage = key;
  event.modifiers = mods;
}

bool btSetTextEvent(BtInputEvent &event, const char *textValue) {
  if (!textValue || textValue[0] == '\0') {
    return false;
  }

  event.type = BT_EVENT_TEXT;
  event.command = BT_CMD_NONE;
  strncpy(event.text, textValue, sizeof(event.text) - 1);
  event.text[sizeof(event.text) - 1] = '\0';
  return true;
}

bool btSetTextCharEvent(BtInputEvent &event, char c) {
  if (c == 0) return false;

  event.type = BT_EVENT_TEXT;
  event.command = BT_CMD_NONE;
  event.text[0] = c;
  event.text[1] = '\0';
  return true;
}

bool btSetTextByteEvent(BtInputEvent &event, uint8_t value) {
  if (value == 0) return false;

  event.type = BT_EVENT_TEXT;
  event.command = BT_CMD_NONE;
  event.text[0] = (char)value;
  event.text[1] = '\0';
  return true;
}

bool btSetCommandEvent(BtInputEvent &event, BtEditorCommand command) {
  if (command == BT_CMD_NONE) {
    return false;
  }

  event.type = BT_EVENT_COMMAND;
  event.command = static_cast<uint8_t>(command);
  event.text[0] = '\0';
  return true;
}

// Map HID usage ID + modifiers to a basic ASCII char or control code.
// Returns 0 if we don't handle that key.
bool btHidKeyToEvent(uint8_t key, uint8_t mods, BtInputEvent &event) {
  btMakeEmptyEvent(event, key, mods);

  bool shift = (mods & 0x22) != 0;  // left or right shift bits
  bool ctrl = (mods & 0x11) != 0;   // left or right Ctrl bits

  // Letters A-Z (usage 0x04-0x1D)
  if (key >= 0x04 && key <= 0x1D) {
    char base = shift ? 'A' : 'a';
    return btSetTextCharEvent(event, base + (key - 0x04));
  }

  const char *base = nullptr;
  const char *shifted = nullptr;

  switch (key) {
    // Number row 1-9, 0
    case 0x1E:
      base = "1";
      shifted = "!";
      break;

    case 0x1F:
      base = "2";
      shifted = (btKeyboardLayout == BT_LAYOUT_UK) ? "\"" : "@";
      break;

    case 0x20:
      if (shift && btKeyboardLayout == BT_LAYOUT_UK) {
        // M5Stack Font0 follows a CP437-style glyph table: 156 (0x9C) displays as £.
        return btSetTextByteEvent(event, 156);
      }
      base = "3";
      shifted = "#";
      break;

    case 0x21:
      base = "4";
      shifted = "$";
      break;
    case 0x22:
      base = "5";
      shifted = "%";
      break;
    case 0x23:
      base = "6";
      shifted = "^";
      break;
    case 0x24:
      base = "7";
      shifted = "&";
      break;
    case 0x25:
      base = "8";
      shifted = "*";
      break;
    case 0x26:
      base = "9";
      shifted = "(";
      break;
    case 0x27:
      base = "0";
      shifted = ")";
      break;

    // Enter, backspace, tab, space
    case 0x28:
      return btSetCommandEvent(event, BT_CMD_ENTER);

    case 0x2A:
      return btSetCommandEvent(event, ctrl ? BT_CMD_DELETE_WORD_BACK : BT_CMD_BACKSPACE);

    case 0x2B:
      return btSetCommandEvent(event, BT_CMD_TAB);

    case 0x2C:
      base = " ";
      shifted = " ";
      break;

    // Symbols on main section
    case 0x2D:
      base = "-";
      shifted = "_";
      break;
    case 0x2E:
      base = "=";
      shifted = "+";
      break;
    case 0x2F:
      base = "[";
      shifted = "{";
      break;
    case 0x30:
      base = "]";
      shifted = "}";
      break;
    case 0x31:
      base = "\\";
      shifted = "|";
      break;

    // Non-US / ISO backslash key. Many UK Bluetooth keyboards send this
    // instead of usage 0x31 for the physical backslash/pipe key.
    case 0x64:
      base = "\\";
      shifted = "|";
      break;

    // ISO hash/tilde key (near Enter or left Shift on many UK boards)
    case 0x32:
      base = "#";
      shifted = "~";
      break;

    case 0x33:
      base = ";";
      shifted = ":";
      break;

    case 0x34:
      base = "'";
      shifted = (btKeyboardLayout == BT_LAYOUT_UK) ? "@" : "\"";
      break;

    case 0x35:
      base = "`";
      shifted = "~";
      break;

    case 0x36:
      base = ",";
      shifted = "<";
      break;
    case 0x37:
      base = ".";
      shifted = ">";
      break;
    case 0x38:
      base = "/";
      shifted = "?";
      break;

    default:
      break;
  }

  if (base) {
    return btSetTextEvent(event, shift ? shifted : base);
  }

  // Dedicated handling for Delete key so Ctrl+Delete can be distinguished
  if (key == 0x4C) {
    return btSetCommandEvent(event, ctrl ? BT_CMD_DELETE_WORD_FORWARD : BT_CMD_DELETE_FORWARD);
  }

  // Page Up / Page Down / Home / End
  if (key == 0x4B) return btSetCommandEvent(event, BT_CMD_PAGE_UP);
  if (key == 0x4E) return btSetCommandEvent(event, BT_CMD_PAGE_DOWN);

  if (key == 0x4A) {
    return btSetCommandEvent(event, ctrl ? BT_CMD_DOC_HOME : BT_CMD_HOME);
  }

  if (key == 0x4D) {
    return btSetCommandEvent(event, ctrl ? BT_CMD_DOC_END : BT_CMD_END);
  }

  // Arrow keys -> editor commands. Ctrl+Left / Ctrl+Right move by word.
  if (ctrl) {
    switch (key) {
      case 0x4F: return btSetCommandEvent(event, BT_CMD_WORD_RIGHT);
      case 0x50: return btSetCommandEvent(event, BT_CMD_WORD_LEFT);
      default: break;
    }
  }

  switch (key) {
    case 0x4F: return btSetCommandEvent(event, BT_CMD_RIGHT);
    case 0x50: return btSetCommandEvent(event, BT_CMD_LEFT);
    case 0x51: return btSetCommandEvent(event, BT_CMD_DOWN);
    case 0x52: return btSetCommandEvent(event, BT_CMD_UP);
    default:
      return false;
  }
}


// Decode one normalised HID keyboard report into Tiny Journal key events.
// Normalised report format:
//   report[0]    = modifiers
//   report[1..6] = key usages
//   report[7]    = unused
void btHandleNormalisedKeyboardReport(const uint8_t report[8]) {
  uint8_t mods = report[0];

  bool anyDown = false;
  bool sawNewKey = false;
  bool mappedNewKey = false;

  for (int i = 1; i <= 6; ++i) {
    if (report[i] != 0) {
      anyDown = true;
      break;
    }
  }

  // Generate one character/control event for each newly pressed key.
  for (int i = 1; i <= 6; ++i) {
    uint8_t key = report[i];
    if (key == 0) continue;

    bool alreadyDown = false;
    for (int j = 1; j <= 6; ++j) {
      if (btLastReport[j] == key) {
        alreadyDown = true;
        break;
      }
    }
    if (alreadyDown) continue;

    sawNewKey = true;

    BtInputEvent event;
    if (btHidKeyToEvent(key, mods, event)) {
      mappedNewKey = true;
      btQueuePushEvent(event);

      unsigned long now = millis();
      btKeyPressStartTime = now;
      btKeyHeld = false;
      btRepeatEligible = true;
    }
  }

  btAnyKeyDown = anyDown;

  if (!btAnyKeyDown) {
    btKeyHeld = false;
    btRepeatEligible = false;
  } else if (sawNewKey && !mappedNewKey) {
    // Important: unmapped keys such as Esc/Caps Lock must not repeat the
    // previous editor action. This was causing "phantom repeat" behaviour.
    btKeyHeld = false;
    btRepeatEligible = false;
  }

  for (int i = 0; i < 8; ++i) {
    btLastReport[i] = report[i];
  }
}

// Convert a raw BLE HID notification into the normalised Tiny Journal
// keyboard report format. This deliberately supports the two common shapes:
//
//   Standard Report 0x2A4D, Micro Journal-style:
//     [mods][key1][key2][key3][key4][key5][key6]
//
//   Boot Keyboard Input Report 0x2A22:
//     [mods][reserved][key1][key2][key3][key4][key5][key6]
//
// Some keyboards send a Report ID before the standard report:
//     [reportId][mods][key1][key2][key3][key4][key5][key6]
bool btNormaliseHidReport(const uint8_t *rawData, uint8_t rawLength, uint8_t outReport[8]) {
  for (int i = 0; i < 8; ++i) outReport[i] = 0;

  if (!rawData || rawLength < 2) {
    return false;
  }

  if (btUsingBootReport) {
    // Classic boot report: [mods][reserved][key1..6]
    if (rawLength < 3) return false;
    outReport[0] = rawData[0];
    for (int i = 0; i < 6; ++i) {
      int src = i + 2;
      if (src < rawLength) outReport[i + 1] = rawData[src];
    }
    return true;
  }

  // Micro Journal-style standard HID report:
  // [mods][key1][key2][key3][key4][key5][key6]
  if (rawLength == 7) {
    outReport[0] = rawData[0];
    for (int i = 0; i < 6; ++i) {
      outReport[i + 1] = rawData[i + 1];
    }
    return true;
  }

  // Classic boot-like report:
  // [mods][reserved][key1][key2][key3][key4][key5][key6]
  if (rawLength == 8) {
    outReport[0] = rawData[0];
    for (int i = 0; i < 6; ++i) {
      outReport[i + 1] = rawData[i + 2];
    }
    return true;
  }

  // Report-ID-prefixed fallback:
  // [reportId][mods][key1][key2][key3][key4][key5][key6]
  if (rawLength >= 8) {
    outReport[0] = rawData[1];
    for (int i = 0; i < 6; ++i) {
      int src = i + 2;
      if (src < rawLength) outReport[i + 1] = rawData[src];
    }
    return true;
  }

  return false;
}


// Process queued BLE HID reports from the main loop.
void btProcessHidReports() {
  if (!btHidReportQueue) return;

  BtHidReport raw;
  uint8_t normalised[8];

  while (xQueueReceive(btHidReportQueue, &raw, 0) == pdTRUE) {
    if (btNormaliseHidReport(raw.data, raw.length, normalised)) {
      btHandleNormalisedKeyboardReport(normalised);
    }
  }
}

// Notification callback for HID input reports.
// Keep this deliberately small: copy bytes into a FreeRTOS queue, then return.
// Decoding in the callback is risky because NimBLE callbacks run inside BLE task
// context, not inside Tiny Journal's normal editor loop.
void btNotifyCallback(NimBLERemoteCharacteristic *pRemoteCharacteristic,
                      uint8_t *pData, size_t length, bool isNotify) {
  if (!pData || length < 2 || !btHidReportQueue) {
    return;
  }

  BtHidReport report;
  report.length = length > 8 ? 8 : length;
  for (uint8_t i = 0; i < 8; ++i) {
    report.data[i] = 0;
  }
  for (uint8_t i = 0; i < report.length; ++i) {
    report.data[i] = pData[i];
  }

  btReportCount++;

  if (xQueueSend(btHidReportQueue, &report, 0) != pdTRUE) {
    btDroppedReportCount++;
  }
}


// Scan callback: remember first HID keyboard we see and stop scan
class BtScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    // HID service UUID 0x1812
    if (advertisedDevice->isAdvertisingService(NimBLEUUID((uint16_t)0x1812))) {
      // drop const so we can store the pointer
      btKeyboardAdvDevice = (NimBLEAdvertisedDevice *)advertisedDevice;
      btKeyboardName = advertisedDevice->getName().c_str();
      NimBLEDevice::getScan()->stop();
    }
  }
};

// Client callbacks so we can see security / auth status
class BtClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *pClient) override {
    btAuthDebug = "connected";
  }

  void onConnectFail(NimBLEClient *pClient, int reason) override {
    btAuthDebug = "connectFail=";
    btAuthDebug += String(reason);
  }

  void onDisconnect(NimBLEClient *pClient, int reason) override {
    btAuthDebug = "disc=";
    btAuthDebug += String(reason);
  }

  void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
    // Micro Journal uses this same fallback PIN for keyboards that ask
    // the central/client to provide a passkey.
    btAuthDebug = "passkey 123456";
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }

  void onConfirmPasskey(NimBLEConnInfo &connInfo, uint32_t pin) override {
    // Numeric comparison pairing: accept automatically for now.
    // Later we can show the number on screen if needed.
    btAuthDebug = "confirm ";
    btAuthDebug += String(pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    bool enc = connInfo.isEncrypted();
    bool auth = connInfo.isAuthenticated();
    bool bond = connInfo.isBonded();

    btAuthDebug = "auth=";
    btAuthDebug += auth ? "1" : "0";
    btAuthDebug += " enc=";
    btAuthDebug += enc ? "1" : "0";
    btAuthDebug += " bond=";
    btAuthDebug += bond ? "1" : "0";
  }
};

static BtClientCallbacks btClientCallbacks;


void initBluetoothKeyboard() {
  loadBluetoothKeyboardConfigFromSD();
  if (btInitDone) return;

  NimBLEDevice::init("Cardputer Journal");

  if (!btHidReportQueue) {
    btHidReportQueue = xQueueCreate(BT_HID_REPORT_QUEUE_SIZE, sizeof(BtHidReport));
  }

  // Match Micro Journal's BLE keyboard security approach:
  // bonding + MITM + LE Secure Connections.
  // This is important because some BLE keyboards will bond/encrypt but still
  // not send input reports unless the pairing is authenticated.
  NimBLEDevice::setSecurityAuth(true, true, true);

  btInitDone = true;
}

// Start a short active scan for HID keyboards
void btKeyboardStartScan() {
  if (!btInitDone) {
    initBluetoothKeyboard();
  }

  btKeyboardAdvDevice = nullptr;
  btKeyboardConnected = false;
  btKeyboardReportChar = nullptr;
  btKeyboardLastError = "";
  btKeyboardName = "";
  btKeyboardAddress = "";
  btScanDeviceCount = 0;
  btSetupState = BT_STATE_SCANNING;

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);

  // Blocking scan for up to 15 seconds
  NimBLEScanResults results = scan->getResults(15000, false);

  const int count = results.getCount();
  if (count <= 0) {
    btSetupState = BT_STATE_IDLE;
    btKeyboardEnabled = false;
    btKeyboardLastError = "No devices found";
    showStatusMessage("No BT devices found", 2000);
    return;
  }

  const NimBLEUUID hidUuid((uint16_t)0x1812);
  const NimBLEAdvertisedDevice *found = nullptr;
  const NimBLEAdvertisedDevice *savedMatch = nullptr;

  for (int i = 0; i < count && btScanDeviceCount < BT_MAX_SCAN_DEVICES; ++i) {
    const NimBLEAdvertisedDevice *dev = results.getDevice(i);
    if (!dev) continue;

    bool isHid = dev->isAdvertisingService(hidUuid);
    String address = dev->getAddress().toString().c_str();
    String name = dev->getName().c_str();
    if (name.length() == 0) {
      // Fall back to MAC address if the device has no name
      name = address;
    }

    btScanDeviceNames[btScanDeviceCount] = name;
    btScanDeviceIsHid[btScanDeviceCount] = isHid;
    btScanDeviceCount++;

    // Prefer the previously paired keyboard if it appears in the scan results.
    if (isHid && btKeyboardSavedAddress.length() > 0 && address.equalsIgnoreCase(btKeyboardSavedAddress)) {
      savedMatch = dev;
    }

    // Remember the first HID device as a fallback.
    if (!found && isHid) {
      found = dev;
    }
  }

  if (savedMatch) {
    found = savedMatch;
  }

  if (!found) {
    btSetupState = BT_STATE_IDLE;
    btKeyboardEnabled = false;
    btKeyboardLastError = "No HID keyboard";
    showStatusMessage("No HID keyboard found", 2000);
    return;
  }

  btKeyboardAdvDevice = (NimBLEAdvertisedDevice *)found;
  btKeyboardName = found->getName().c_str();
  btKeyboardAddress = found->getAddress().toString().c_str();

  if (btKeyboardConnect()) {
    String msg = "BT keyboard: ";
    if (btKeyboardName.length()) {
      msg += btKeyboardName;
    } else {
      msg += "connected";
    }
    showStatusMessage(msg, 2000);
  } else {
    String msg = "BT keyboard error: " + btKeyboardLastError;
    showStatusMessage(msg, 2000);
    btKeyboardEnabled = false;
  }
}


// Connect to the last advertised HID keyboard, subscribe to reports
bool btKeyboardConnect() {
  if (!btKeyboardAdvDevice) {
    btKeyboardLastError = "No keyboard found";
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  btKeyboardClient = NimBLEDevice::createClient();
  btKeyboardClient->setClientCallbacks(&btClientCallbacks, false);
  btKeyboardClient->setConnectionParams(12, 12, 0, 51);
  btKeyboardClient->setConnectTimeout(5000);

  // Reset debug counters for this connection attempt
  btReportCount = 0;
  btDroppedReportCount = 0;
  btSubscribedReportCount = 0;
  btReportDebug = "";
  btAuthDebug = "";

  if (!btKeyboardClient->connect(btKeyboardAdvDevice)) {
    btKeyboardLastError = "Connect failed";
    btKeyboardClient = nullptr;
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  // Run the security / bonding handshake (required by many keyboards).
  if (!btKeyboardClient->secureConnection()) {
    btKeyboardLastError = "Secure connect failed";
    btKeyboardClient->disconnect();
    btKeyboardClient = nullptr;
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  // Force service/characteristic discovery before selecting the report
  // characteristic. Micro Journal does this explicitly, and it helps avoid
  // connecting successfully but never receiving HID reports.
  if (!btKeyboardClient->discoverAttributes()) {
    btKeyboardLastError = "Discover failed";
    btKeyboardClient->disconnect();
    btKeyboardClient = nullptr;
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  NimBLERemoteService *hidService = btKeyboardClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hidService) {
    btKeyboardLastError = "No HID service";
    btKeyboardClient->disconnect();
    btKeyboardClient = nullptr;
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  NimBLERemoteCharacteristic *primaryReportChar = nullptr;
  btUsingBootReport = false;

  // Start fresh for this connection.
  if (btHidReportQueue) {
    xQueueReset(btHidReportQueue);
  }
  for (int i = 0; i < 8; ++i) {
    btLastReport[i] = 0;
  }
  btAnyKeyDown = false;
  btKeyHeld = false;
  btRepeatEligible = false;
  btClearInputEventQueue();

  // Prefer the same path as Micro Journal: HID Report characteristic 0x2A4D.
  // Important: some keyboards expose more than one 0x2A4D report characteristic.
  // getCharacteristic() may return the wrong one, so subscribe to every notifiable
  // 0x2A4D characteristic we find.
  uint8_t subscriptions = 0;
  const NimBLEUUID reportUuid((uint16_t)0x2A4D);
  const NimBLEUUID bootKeyboardUuid((uint16_t)0x2A22);

  const std::vector<NimBLERemoteCharacteristic *> &chars = hidService->getCharacteristics(true);

  for (auto *chr : chars) {
    if (!chr) continue;
    if (!chr->getUUID().equals(reportUuid)) continue;
    if (!(chr->canNotify() || chr->canIndicate())) continue;

    bool notify = chr->canNotify();
    if (chr->subscribe(notify, btNotifyCallback)) {
      if (!primaryReportChar) primaryReportChar = chr;
      subscriptions++;
    }
  }

  // Fallback: Boot Keyboard Input Report 0x2A22.
  // If we use this, request Boot Protocol via Protocol Mode 0x2A4E first.
  if (subscriptions == 0) {
    NimBLERemoteCharacteristic *protocolMode =
      hidService->getCharacteristic(NimBLEUUID((uint16_t)0x2A4E));
    if (protocolMode && protocolMode->canWrite()) {
      uint8_t bootMode = 0x00;  // 0 = Boot Protocol, 1 = Report Protocol
      protocolMode->writeValue(&bootMode, 1, true);
    }

    for (auto *chr : chars) {
      if (!chr) continue;
      if (!chr->getUUID().equals(bootKeyboardUuid)) continue;
      if (!(chr->canNotify() || chr->canIndicate())) continue;

      bool notify = chr->canNotify();
      if (chr->subscribe(notify, btNotifyCallback)) {
        if (!primaryReportChar) primaryReportChar = chr;
        subscriptions++;
        btUsingBootReport = true;
      }
    }
  }

  // Last-resort fallback: subscribe to the first notifiable HID characteristic.
  // This is not ideal, but it gives us a way to test keyboards that do not expose
  // the expected 0x2A4D/0x2A22 path.
  if (subscriptions == 0) {
    for (auto *chr : chars) {
      if (chr && (chr->canNotify() || chr->canIndicate())) {
        bool notify = chr->canNotify();
        if (chr->subscribe(notify, btNotifyCallback)) {
          primaryReportChar = chr;
          subscriptions++;
          btUsingBootReport = false;
          break;
        }
      }
    }
  }

  if (subscriptions == 0 || !primaryReportChar) {
    btKeyboardLastError = "Subscribe failed";
    btKeyboardClient->disconnect();
    btKeyboardClient = nullptr;
    btKeyboardReportChar = nullptr;
    btSetupState = BT_STATE_IDLE;
    return false;
  }

  btKeyboardReportChar = primaryReportChar;
  btSubscribedReportCount = subscriptions;
  btReportDebug = "subs=";
  btReportDebug += String(subscriptions);
  btReportDebug += btUsingBootReport ? " boot" : " report";

  btKeyboardConnected = true;
  btKeyboardLastError = "";
  if (btKeyboardAddress.length() > 0) {
    btKeyboardSavedAddress = btKeyboardAddress;
    btKeyboardSavedName = btKeyboardName;
    saveBluetoothKeyboardConfigToSD();
  }
  btSetupState = BT_STATE_CONNECTED;
  btKeyboardEnabled = true;
  return true;
}


void btKeyboardDisconnect() {
  if (btKeyboardClient) {
    if (btKeyboardClient->isConnected()) {
      btKeyboardClient->disconnect();
    }
    btKeyboardClient = nullptr;
  }
  btKeyboardConnected = false;
  btKeyboardReportChar = nullptr;
  btKeyboardAdvDevice = nullptr;
  btKeyboardEnabled = false;
  btSetupState = BT_STATE_IDLE;

  // Clear BT repeat state and queued reports
  btAnyKeyDown = false;
  btKeyHeld = false;
  btRepeatEligible = false;
  btUsingBootReport = false;
  btClearInputEventQueue();
  for (int i = 0; i < 8; ++i) {
    btLastReport[i] = 0;
  }
  if (btHidReportQueue) {
    xQueueReset(btHidReportQueue);
  }
}


// Poll the BLE stack for connection progress. Call frequently from loop().
void btKeyboardPollConnection() {
  if (!btKeyboardEnabled) return;
  if (!btKeyboardConnected) return;

  // If link has dropped, clean up state and show a message
  if (btKeyboardClient && !btKeyboardClient->isConnected()) {
    btKeyboardDisconnect();
    btKeyboardLastError = "BT keyboard disconnected";
    showStatusMessage("BT keyboard disconnected", 2000);
  }
}

// Consume text/command events from the BT queue and feed them into the app
void handleBluetoothInput() {
  if (!btKeyboardEnabled) {
    return;
  }

  btKeyboardPollConnection();

  if (!btKeyboardConnected) {
    return;
  }

  btProcessHidReports();

  BtInputEvent event;
  bool any = false;

  while (btQueuePopEvent(event)) {
    any = true;

    unsigned long now = millis();
    lastKeyPressTime = now;
    savedAfterIdle = false;

    if (appMode == MODE_EDITOR) {
      if (event.type == BT_EVENT_TEXT) {
        size_t len = strlen(event.text);
        insertText(event.text);

        if (len == 1) {
          setLastAction(ACTION_INSERT_CHAR, event.text[0]);
        } else {
          // Multi-byte text cannot be represented by the
          // old single-char repeat system. Insert it once and disable repeat.
          setLastAction(ACTION_NONE, 0);
          btRepeatEligible = false;
        }
      } else if (event.type == BT_EVENT_COMMAND) {
        switch (event.command) {
          case BT_CMD_BACKSPACE:
            backspaceChar();
            setLastAction(ACTION_BACKSPACE, 0);
            break;

          case BT_CMD_ENTER:
            insertNewline();
            setLastAction(ACTION_INSERT_CHAR, '\n');
            break;

          case BT_CMD_TAB:
            insertTab();
            setLastAction(ACTION_INSERT_CHAR, '\t');
            break;

          case BT_CMD_UP:
            moveCursorUp();
            setLastAction(ACTION_MOVE_UP, 0);
            break;

          case BT_CMD_DOWN:
            moveCursorDown();
            setLastAction(ACTION_MOVE_DOWN, 0);
            break;

          case BT_CMD_LEFT:
            moveCursorLeft();
            setLastAction(ACTION_MOVE_LEFT, 0);
            break;

          case BT_CMD_RIGHT:
            moveCursorRight();
            setLastAction(ACTION_MOVE_RIGHT, 0);
            break;

          case BT_CMD_DELETE_WORD_BACK:
            deleteWordBackwards();
            setLastAction(ACTION_DELETE_WORD_BACK, 0);
            break;

          case BT_CMD_DELETE_WORD_FORWARD:
            deleteWordForwards();
            setLastAction(ACTION_DELETE_WORD_FORWARD, 0);
            break;

          case BT_CMD_WORD_LEFT:
            moveCursorWordLeft();
            setLastAction(ACTION_WORD_LEFT, 0);
            break;

          case BT_CMD_WORD_RIGHT:
            moveCursorWordRight();
            setLastAction(ACTION_WORD_RIGHT, 0);
            break;

          case BT_CMD_PAGE_UP:
            scrollPageUp();
            setLastAction(ACTION_PAGE_UP, 0);
            break;

          case BT_CMD_PAGE_DOWN:
            scrollPageDown();
            setLastAction(ACTION_PAGE_DOWN, 0);
            break;

          case BT_CMD_HOME:
            moveCursorHome();
            setLastAction(ACTION_HOME, 0);
            break;

          case BT_CMD_END:
            moveCursorEnd();
            setLastAction(ACTION_END, 0);
            break;

          case BT_CMD_DOC_HOME:
            moveCursorDocHome();
            setLastAction(ACTION_DOC_HOME, 0);
            break;

          case BT_CMD_DOC_END:
            moveCursorDocEnd();
            setLastAction(ACTION_DOC_END, 0);
            break;

          case BT_CMD_DELETE_FORWARD:
            deleteForwardChar();
            setLastAction(ACTION_DELETE_FORWARD, 0);
            break;

          default:
            break;
        }
      }

      needsRedraw = true;

    } else if (appMode == MODE_MENU) {
      // Basic menu navigation with BT keyboard
      if (event.type == BT_EVENT_COMMAND) {
        if (event.command == BT_CMD_ENTER) {
          menuSelect();
        } else if (event.command == BT_CMD_BACKSPACE) {
          menuBack();
        } else if (event.command == BT_CMD_UP) {
          menuMoveUp();
          needsRedraw = true;
        } else if (event.command == BT_CMD_DOWN) {
          menuMoveDown();
          needsRedraw = true;
        }
      }
    }
  }

  if (any) {
    needsRedraw = true;
  }
}

// ---------- Bluetooth keyboard setup UI (pairing screen) ----------

void openBluetoothKeyboardSetup() {
  currentMenu = MENU_SETTINGS_BT;
  // Reflect current connection state in the UI
  if (btKeyboardConnected) {
    btSetupState = BT_STATE_CONNECTED;
  } else if (btKeyboardEnabled) {
    btSetupState = BT_STATE_SCANNING;
  } else {
    btSetupState = BT_STATE_IDLE;
  }
  needsRedraw = true;
}

void handleBluetoothKeyboardSetupKeys(const Keyboard_Class::KeysState &ks) {
  // Back / delete: return to Sync & connectivity menu
  if (ks.del) {
    openMenu(MENU_SETTINGS_SYNC);
    needsRedraw = true;
    return;
  }

  // Opt+Enter forgets the saved keyboard and clears BLE bond records.
  if (ks.opt && ks.enter) {
    if (btKeyboardConnected || (btKeyboardClient && btKeyboardClient->isConnected())) {
      btKeyboardDisconnect();
    }
    forgetBluetoothKeyboardConfig();
    showStatusMessage("BT keyboard + bond forgotten", 2200);
    needsRedraw = true;
    return;
  }

  // Opt key: toggle layout (US <-> UK)
  if (ks.opt) {
    if (btKeyboardLayout == BT_LAYOUT_US) {
      btKeyboardLayout = BT_LAYOUT_UK;
      showStatusMessage("BT layout: UK", 1500);
    } else {
      btKeyboardLayout = BT_LAYOUT_US;
      showStatusMessage("BT layout: US", 1500);
    }
    saveBluetoothKeyboardConfigToSD();
    needsRedraw = true;
    return;
  }


  // Connected: Enter disconnects
  if (btKeyboardConnected) {
    if (ks.enter) {
      btKeyboardDisconnect();
      showStatusMessage("BT keyboard: disconnected", 2000);
      needsRedraw = true;
    }
    return;
  }

  // Not connected yet
  if (btSetupState == BT_STATE_IDLE) {
    // Enter from idle: start a scan + auto-connect
    if (ks.enter) {
      btKeyboardEnabled = true;
      btKeyboardLastError = "";
      btKeyboardName = "";
      btSetupState = BT_STATE_SCANNING;
      showStatusMessage("Scanning for keyboards...", 20000);
      drawBluetoothKeyboardSetup();  // update UI before blocking scan
      btKeyboardStartScan();
      needsRedraw = true;
    }
    return;
  }


  if (btSetupState == BT_STATE_SCANNING) {
    // While scanning, Enter cancels
    if (ks.enter) {
      NimBLEScan *scan = NimBLEDevice::getScan();
      scan->stop();
      btSetupState = BT_STATE_IDLE;
      btKeyboardEnabled = false;
      showStatusMessage("BT scan cancelled", 1500);
      needsRedraw = true;
    }
    return;
  }
}

void drawBluetoothKeyboardSetup() {
  M5Cardputer.Display.startWrite();

  int screenW = M5Cardputer.Display.width();
  bool fullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (fullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;

  int startY = topBarHeight;

  String title = "Bluetooth keyboard";
  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(title);

  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int y = startY;

  if (btKeyboardConnected) {
    // Show current connection
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    String label = "Connected to: ";
    if (btKeyboardName.length()) {
      label += btKeyboardName;
    } else {
      label += "(unnamed device)";
    }
    M5Cardputer.Display.print(label);
    y += lineHeight;

    // Show security / auth status
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    if (btAuthDebug.length() > 0) {
      M5Cardputer.Display.print("Sec: " + btAuthDebug);
    } else {
      M5Cardputer.Display.print("Sec: (no data)");
    }
    y += lineHeight;

    // Show how many HID reports we've seen
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Reports: " + String(btReportCount));
    y += lineHeight;

    // Report subscription debug
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    String reportLabel = btReportDebug.length() ? btReportDebug : "subs=0";
    reportLabel += " drop=";
    reportLabel += String(btDroppedReportCount);
    M5Cardputer.Display.print(reportLabel);
    y += lineHeight;

    // Layout info
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    String layoutLabel = "Layout: ";
    layoutLabel += (btKeyboardLayout == BT_LAYOUT_UK) ? "UK" : "US";
    layoutLabel += "  (Opt to change)";
    M5Cardputer.Display.print(layoutLabel);
    y += lineHeight;

    // Saved keyboard info
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    if (btKeyboardSavedAddress.length()) {
      String savedLabel = "Saved: ";
      savedLabel += btKeyboardSavedName.length() ? btKeyboardSavedName : btKeyboardSavedAddress;
      M5Cardputer.Display.print(savedLabel);
    } else {
      M5Cardputer.Display.print("Saved: none");
    }
    y += lineHeight;

    // Hint for controls
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter: disconnect Back: exit");
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Opt+Enter: forget keyboard");
    y += lineHeight;

  } else if (btSetupState == BT_STATE_SCANNING) {
    // Scanning state
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Scanning for keyboards...");
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter: cancel        Back: exit");
    y += lineHeight;

  } else {
    // Idle / not connected
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    if (btKeyboardLastError.length() > 0) {
      M5Cardputer.Display.print("Last error: " + btKeyboardLastError);
    } else {
      M5Cardputer.Display.print("No BT keyboard connected.");
    }
    y += lineHeight;

    if (btScanDeviceCount > 0) {
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
      M5Cardputer.Display.setCursor(3, y);
      M5Cardputer.Display.print("Last scan:");
      y += lineHeight;

      int showCount = btScanDeviceCount;
      if (showCount > 5) showCount = 5;  // don’t overflow the screen
      for (int i = 0; i < showCount; ++i) {
        M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
        M5Cardputer.Display.setCursor(3, y);
        String devLabel = btScanDeviceNames[i];
        bool looksLikeMac = devLabel.indexOf(':') >= 0;  // crude check

        String row = String(i + 1) + ". ";
        if (looksLikeMac) {
          row += "Unknown device (" + devLabel + ")";
        } else {
          row += devLabel;
        }
        if (btScanDeviceIsHid[i]) {
          row += " [HID]";
        }
        M5Cardputer.Display.print(row);
        y += lineHeight;
      }
    }

    // Report subscription debug
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    String reportLabel = btReportDebug.length() ? btReportDebug : "subs=0";
    reportLabel += " drop=";
    reportLabel += String(btDroppedReportCount);
    M5Cardputer.Display.print(reportLabel);
    y += lineHeight;

    // Layout info
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    String layoutLabel = "Layout: ";
    layoutLabel += (btKeyboardLayout == BT_LAYOUT_UK) ? "UK" : "US";
    layoutLabel += "  (Opt to change)";
    M5Cardputer.Display.print(layoutLabel);
    y += lineHeight;

    // Controls hint
    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter: scan & pair   Back: exit");
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Opt+Enter: forget keyboard");
    y += lineHeight;
  }


  // Status message at the bottom, same style as WiFi setup
  if (statusMessageUntil > millis() && statusMessage.length() > 0) {
    int by = M5Cardputer.Display.height() - lineHeight;
    M5Cardputer.Display.fillRect(0, by, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(0, by);
    M5Cardputer.Display.print(statusMessage);
  }

  M5Cardputer.Display.endWrite();
}


// ---------- WiFi setup (scan + connect) ----------

void startWiFiScan() {
  wifiNetworks.clear();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; ++i) {
    WiFiNetwork net;
    net.ssid = WiFi.SSID(i);
    net.rssi = WiFi.RSSI(i);
    wifi_auth_mode_t enc = WiFi.encryptionType(i);
    net.secure = (enc != WIFI_AUTH_OPEN);
    wifiNetworks.push_back(net);
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);

  wifiSelectedIndex = 0;
  wifiScrollOffset = 0;
}

void openWiFiSetup() {
  currentMenu = MENU_SETTINGS_WIFI;
  wifiSetupState = WIFI_STATE_LIST;
  wifiSelectedIndex = 0;
  wifiScrollOffset = 0;
  wifiPasswordInput = "";

  M5Cardputer.Display.startWrite();
  bool menuFullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (menuFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;
  int screenW = M5Cardputer.Display.width();
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print("WiFi setup");

  int y = topBarHeight + lineHeight;
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Scanning networks...");
  M5Cardputer.Display.endWrite();

  startWiFiScan();
  needsRedraw = true;
}

void handleWiFiSetupKeys(const Keyboard_Class::KeysState &ks) {
  if (wifiSetupState == WIFI_STATE_LIST) {
    if (ks.del) {
      openMenu(MENU_SETTINGS_SYNC);
      return;
    }

    int count = (int)wifiNetworks.size();

    if (count == 0) {
      if (ks.enter) {
        openWiFiSetup();
        return;
      }
      return;
    }

    for (auto c : ks.word) {
      if (c == ';') {
        if (wifiSelectedIndex > 0) wifiSelectedIndex--;
        if (wifiSelectedIndex < wifiScrollOffset) wifiScrollOffset = wifiSelectedIndex;
        needsRedraw = true;
      } else if (c == '.') {
        if (wifiSelectedIndex < count - 1) wifiSelectedIndex++;
        if (wifiSelectedIndex >= wifiScrollOffset + listVisibleLines) {
          wifiScrollOffset = wifiSelectedIndex - listVisibleLines + 1;
        }
        needsRedraw = true;
      }
    }

    if (ks.enter && count > 0) {
      wifiSetupState = WIFI_STATE_PWENTRY;
      wifiPasswordInput = "";
      needsRedraw = true;
    }

  } else if (wifiSetupState == WIFI_STATE_PWENTRY) {
    if (ks.del) {
      if (wifiPasswordInput.length() > 0) {
        wifiPasswordInput.remove(wifiPasswordInput.length() - 1);
        needsRedraw = true;
      } else {
        wifiSetupState = WIFI_STATE_LIST;
        needsRedraw = true;
      }
      return;
    }

    if (ks.enter) {
      if (wifiSelectedIndex < 0 || wifiSelectedIndex >= (int)wifiNetworks.size()) {
        wifiSetupState = WIFI_STATE_LIST;
        needsRedraw = true;
        return;
      }

      WiFiNetwork &net = wifiNetworks[wifiSelectedIndex];

      wifiSsid = net.ssid;
      wifiPassword = wifiPasswordInput;
      wifiConfigured = true;

      showStatusMessage("Connecting...", 2000);
      needsRedraw = true;

      bool ok = connectWiFi();
      if (ok) {
        saveWiFiConfig(wifiSsid, wifiPassword);
        disconnectWiFi();
        showStatusMessage("WiFi config saved", 3000);
        openMenu(MENU_SETTINGS_SYNC);
      } else {
        showStatusMessage("WiFi connect failed", 3000);
        wifiSetupState = WIFI_STATE_LIST;
        needsRedraw = true;
      }
      return;
    }

    for (auto c : ks.word) {
      if (c == '\r' || c == '\n') continue;
      wifiPasswordInput += c;
      needsRedraw = true;
    }
  }
}

// ---------- Audio note recording ----------

static void writeLE16(File &file, uint16_t value) {
  uint8_t b[2];
  b[0] = value & 0xFF;
  b[1] = (value >> 8) & 0xFF;
  file.write(b, 2);
}

static void writeLE32(File &file, uint32_t value) {
  uint8_t b[4];
  b[0] = value & 0xFF;
  b[1] = (value >> 8) & 0xFF;
  b[2] = (value >> 16) & 0xFF;
  b[3] = (value >> 24) & 0xFF;
  file.write(b, 4);
}

static void writeWavHeader(File &file, uint32_t sampleRate, uint32_t totalSamples) {
  const uint16_t numChannels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  const uint32_t dataChunkSize = totalSamples * numChannels * (bitsPerSample / 8);
  const uint32_t riffChunkSize = 36 + dataChunkSize;

  file.seek(0);

  // RIFF header
  file.write((const uint8_t *)"RIFF", 4);
  writeLE32(file, riffChunkSize);
  file.write((const uint8_t *)"WAVE", 4);

  // fmt chunk
  file.write((const uint8_t *)"fmt ", 4);
  writeLE32(file, 16);  // Subchunk1Size for PCM
  writeLE16(file, 1);   // AudioFormat = PCM
  writeLE16(file, numChannels);
  writeLE32(file, sampleRate);
  writeLE32(file, byteRate);
  writeLE16(file, blockAlign);
  writeLE16(file, bitsPerSample);

  // data chunk
  file.write((const uint8_t *)"data", 4);
  writeLE32(file, dataChunkSize);
}


void startAudioNote() {
  // Open-ended blocking audio note recorder.
  //
  // Important build note:
  // M5Stack ESP32 board package 3.2.2 is required for working Cardputer mic capture
  // in Arduino IDE on this hardware/library combination. Later 3.3.x builds can
  // produce valid-length silent WAV files or constant bogus sample values.

  if (!SD.exists("/journal/audio")) {
    SD.mkdir("/journal/audio");
  }

  String dateStr = getCurrentDateString();  // e.g. 2025-11-29
  String timeStr = getCurrentTimeString();  // e.g. 184259
  String baseName = dateStr + "_" + timeStr;
  String fileName = baseName + ".wav";
  String path = String("/journal/audio/") + fileName;

  // Ensure we don't overwrite an existing file; add _1, _2, ... suffix if needed.
  int suffix = 1;
  while (SD.exists(path.c_str()) && suffix < 100) {
    fileName = baseName + "_" + String(suffix) + ".wav";
    path = String("/journal/audio/") + fileName;
    suffix++;
  }

  File audioFile = SD.open(path.c_str(), FILE_WRITE);
  if (!audioFile) {
    showStatusMessage("Audio: can't open file", 2000);
    return;
  }

  uint8_t blankHeader[44] = { 0 };
  audioFile.write(blankHeader, sizeof(blankHeader));

  const uint32_t sampleRate = 17000;
  const size_t chunkSamples = 256;

  int16_t sampleBuf[chunkSamples];
  uint32_t samplesWritten = 0;
  const int warmupChunksToDiscard = 8;  // discard initial mic/I2S settling click
  int warmupChunksDiscarded = 0;
  unsigned long startedMs = millis();
  unsigned long lastScreenUpdateMs = 0;

  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.print("Recording audio note");
  M5Cardputer.Display.setCursor(0, lineHeight * 2);
  M5Cardputer.Display.print(fileName);
  M5Cardputer.Display.setCursor(0, lineHeight * 4);
  M5Cardputer.Display.print("Press any key to stop");

  // The Cardputer mic and speaker cannot be used at the same time.
  M5Cardputer.Speaker.end();
  delay(50);
  M5Cardputer.Mic.begin();

  while (true) {
    M5Cardputer.update();

    // Ignore the keypress that opened the recorder; after that, any key stops.
    if ((millis() - startedMs) > 500 && M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      break;
    }

    if (M5Cardputer.Mic.record(sampleBuf, chunkSamples, sampleRate)) {
      if (warmupChunksDiscarded < warmupChunksToDiscard) {
        warmupChunksDiscarded++;
      } else {
        audioFile.write((uint8_t *)sampleBuf, chunkSamples * sizeof(int16_t));
        samplesWritten += chunkSamples;
      }
    }

    unsigned long now = millis();
    if (now - lastScreenUpdateMs >= 500) {
      lastScreenUpdateMs = now;
      unsigned long elapsedSec = (now - startedMs) / 1000;
      int screenW = M5Cardputer.Display.width();

      M5Cardputer.Display.fillRect(0, lineHeight * 6, screenW, lineHeight * 2, editorBgColor);
      M5Cardputer.Display.setCursor(0, lineHeight * 6);
      M5Cardputer.Display.print("Time: ");
      M5Cardputer.Display.print(elapsedSec);
      M5Cardputer.Display.print("s");
    }

    delay(1);
  }

  M5Cardputer.Mic.end();
  delay(50);
  M5Cardputer.Speaker.begin();

  writeWavHeader(audioFile, sampleRate, samplesWritten);
  audioFile.close();

  showStatusMessage("Saved audio: " + fileName, 2000);

  needsRedraw = true;
}
void drawAudioNotesList(AudioNoteEntry *entries, int count, int selectedIndex, int scrollOffset) {
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.print("Audio notes");

  int topY = lineHeight * 2;
  int screenH = M5Cardputer.Display.height();
  int screenW = M5Cardputer.Display.width();
  int rows = (screenH - topY - lineHeight * 2) / lineHeight;
  if (rows < 1) rows = 1;

  for (int i = 0; i < rows; ++i) {
    int idx = scrollOffset + i;
    if (idx >= count) break;

    int y = topY + i * lineHeight;

    // Highlight selected line
    if (idx == selectedIndex) {
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorFgColor);
      M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
    } else {
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
      M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    }

    String baseName = entries[idx].name;
    int slashPos = baseName.lastIndexOf('/');
    if (slashPos >= 0) {
      baseName = baseName.substring(slashPos + 1);
    }

    // File name
    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print(baseName);
  }

  // Footer instructions
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  int footerY = screenH - lineHeight;
  M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
  M5Cardputer.Display.setCursor(0, footerY);
  M5Cardputer.Display.print("W/S, ;/. : move   d: del   D: del all   `: back");
}


void startAudioNoteBrowser() {
  AudioNoteEntry entries[MAX_AUDIO_NOTES];
  int count = 0;

  File dir = SD.open("/journal/audio");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    showStatusMessage("No /journal/audio folder", 2000);
    return;
  }

  // Collect .wav files
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory() && count < MAX_AUDIO_NOTES) {
      String fileName = String(f.name());
      if (fileName.endsWith(".wav") || fileName.endsWith(".WAV")) {
        entries[count].name = fileName;
        entries[count].path = String("/journal/audio/") + fileName;
        entries[count].size = f.size();
        count++;
      }
    }
    f.close();
  }
  dir.close();

  if (count == 0) {
    showStatusMessage("No audio notes found", 2000);
    return;
  }

  // Sort newest first by filename (YYYY-MM-DD_HHMM.wav => lexicographic = chronological)
  for (int i = 0; i < count - 1; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (entries[j].name > entries[i].name) {
        AudioNoteEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }
    }
  }

  int selectedIndex = 0;
  int scrollOffset = 0;

  int screenH = M5Cardputer.Display.height();
  int rows = (screenH - lineHeight * 4) / lineHeight;
  if (rows < 1) rows = 1;

  drawAudioNotesList(entries, count, selectedIndex, scrollOffset);

  bool browsing = true;
  while (browsing) {
    M5Cardputer.update();
    updateRTC();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

      for (auto c : ks.word) {
        // Back out on ` or ESC (Fn+`)
        if (c == '`' || c == 27) {
          browsing = false;
          break;
        }

        // Move selection up (W or ; as arrow-up)
        if (c == 'w' || c == 'W' || c == ';') {
          if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
              scrollOffset = selectedIndex;
            }
            drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
          }
        }

        // Move selection down (S or . as arrow-down)
        if (c == 's' || c == 'S' || c == '.') {
          if (selectedIndex < count - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + rows) {
              scrollOffset = selectedIndex - rows + 1;
            }
            drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
          }
        }

        // Delete selected note with 'd'
        if (c == 'd') {
          if (count > 0) {
            String delPath = entries[selectedIndex].path;
            SD.remove(delPath.c_str());

            // Compact the list
            for (int i = selectedIndex; i < count - 1; ++i) {
              entries[i] = entries[i + 1];
            }
            count--;

            if (count == 0) {
              showStatusMessage("No audio notes left", 2000);
              browsing = false;
              break;
            }

            if (selectedIndex >= count) {
              selectedIndex = count - 1;
            }
            if (selectedIndex < scrollOffset) {
              scrollOffset = selectedIndex;
            }
            if (selectedIndex >= scrollOffset + rows) {
              scrollOffset = selectedIndex - rows + 1;
            }
            drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
          }
        }

        // Delete ALL notes with 'D'
        if (c == 'D') {
          for (int i = 0; i < count; ++i) {
            SD.remove(entries[i].path.c_str());
          }
          count = 0;
          showStatusMessage("All audio notes deleted", 2000);
          browsing = false;
          break;
        }
      }
    }

    delay(5);
  }

  // Return to whatever screen we were on before
  needsRedraw = true;
}

static void collectDocsFromDir(const char *dirPath, DocumentEntry *entries, int &count, int maxCount) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory() && count < maxCount) {
      String name = String(f.name());
      String lower = name;
      lower.toLowerCase();

      bool isTxt = lower.endsWith(".txt");
      bool isRtc = (lower == "rtc.txt");
      bool isCfgRead = (lower == "config-readme.txt");
      bool isExcluded = isRtc || isCfgRead;

      if (isTxt && !isExcluded) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += name;

        String rel = fullPath;
        const String journalPrefix = "/journal/";
        if (rel.startsWith(journalPrefix)) {
          rel = rel.substring(journalPrefix.length());
        } else if (rel.length() > 0 && rel[0] == '/') {
          rel = rel.substring(1);
        }

        entries[count].path = fullPath;
        entries[count].displayName = rel;  // e.g. "daily/2025-11-29.txt"
        entries[count].size = f.size();
        count++;
      }
    }

    f.close();
  }

  dir.close();
}


void drawDocumentList(DocumentEntry *entries, int count, int selectedIndex, int scrollOffset) {
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.print("Open document");

  int topY = lineHeight * 2;
  int screenH = M5Cardputer.Display.height();
  int screenW = M5Cardputer.Display.width();
  int rows = (screenH - topY - lineHeight * 2) / lineHeight;
  if (rows < 1) rows = 1;

  for (int i = 0; i < rows; ++i) {
    int idx = scrollOffset + i;
    if (idx >= count) break;

    int y = topY + i * lineHeight;

    // Highlight selected line
    if (idx == selectedIndex) {
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorFgColor);
      M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
    } else {
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
      M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    }

    String name = entries[idx].displayName;

    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print(name);
  }

  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  int footerY = screenH - lineHeight;
  M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
  M5Cardputer.Display.setCursor(0, footerY);
  M5Cardputer.Display.print("ENTER: open   d: rename   `: back");
}

static bool renameDocumentEntry(DocumentEntry *entries, int count, int &selectedIndex, int &scrollOffset) {
  if (count <= 0 || selectedIndex < 0 || selectedIndex >= count) return false;

  String fullPath = entries[selectedIndex].path;
  int lastSlash = fullPath.lastIndexOf('/');
  if (lastSlash < 0) return false;

  String dirPath = fullPath.substring(0, lastSlash);
  String oldName = fullPath.substring(lastSlash + 1);

  String rel = entries[selectedIndex].displayName;
  int slashRel = rel.lastIndexOf('/');
  String relFolder = "";
  if (slashRel >= 0) {
    relFolder = rel.substring(0, slashRel);  // e.g. "daily"
  }

  String newName = oldName;
  bool renaming = true;
  bool confirmed = false;

  while (renaming) {
    // Draw rename screen
    M5Cardputer.Display.fillScreen(editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

    int y = 0;
    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print("Rename document");
    y += lineHeight * 2;

    if (relFolder.length() > 0) {
      M5Cardputer.Display.setCursor(0, y);
      M5Cardputer.Display.print("Folder: " + relFolder);
      y += lineHeight;
    }

    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print("Name:");
    y += lineHeight;

    M5Cardputer.Display.setCursor(0, y);
    M5Cardputer.Display.print(newName);

    int screenH = M5Cardputer.Display.height();
    int screenW = M5Cardputer.Display.width();
    int footerY = screenH - lineHeight;
    M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(0, footerY);
    M5Cardputer.Display.print("ENTER: save   DEL: back   `: cancel");

    M5Cardputer.update();
    updateRTC();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

      // Confirm with ENTER
      if (ks.enter) {
        confirmed = true;
        renaming = false;
        break;
      }

      // Backspace with DEL
      if (ks.del) {
        if (newName.length() > 0) {
          newName.remove(newName.length() - 1);
        }
        continue;
      }

      for (auto c : ks.word) {
        // Cancel with ` or ESC
        if (c == '`' || c == 27) {
          confirmed = false;
          renaming = false;
          break;
        }
        if (c == '\r' || c == '\n') continue;

        // Basic printable ASCII
        if (c >= 32 && c <= 126) {
          newName += c;
        }
      }
    }

    delay(5);
  }

  if (!confirmed) {
    showStatusMessage("Rename cancelled", 1500);
    return false;
  }

  newName.trim();
  if (newName.length() == 0) {
    showStatusMessage("Name cannot be empty", 2000);
    return false;
  }

  if (newName.indexOf('/') >= 0) {
    showStatusMessage("Name can't contain /", 2000);
    return false;
  }

  // Ensure .txt extension
  String lower = newName;
  lower.toLowerCase();
  if (!lower.endsWith(".txt")) {
    newName += ".txt";
  }

  String newFullPath = dirPath + "/" + newName;
  if (newFullPath == fullPath) {
    showStatusMessage("Name unchanged", 1500);
    return false;
  }

  if (SD.exists(newFullPath.c_str())) {
    showStatusMessage("File already exists", 2000);
    return false;
  }

  if (!SD.rename(fullPath.c_str(), newFullPath.c_str())) {
    showStatusMessage("Rename failed", 2000);
    return false;
  }

  // Update the entry's path + display name
  entries[selectedIndex].path = newFullPath;

  String newRel = newFullPath;
  const String journalPrefix = "/journal/";
  if (newRel.startsWith(journalPrefix)) {
    newRel = newRel.substring(journalPrefix.length());
  } else if (newRel.length() > 0 && newRel[0] == '/') {
    newRel = newRel.substring(1);
  }
  entries[selectedIndex].displayName = newRel;

  showStatusMessage("Renamed", 1500);
  return true;
}


void openDocumentBrowser() {
  DocumentEntry entries[MAX_DOCUMENTS];
  int count = 0;

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 2000);
    return;
  }

  // Collect .txt files from the main journal dirs
  collectDocsFromDir("/journal/daily", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/notes", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/meetings", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/projects", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/travel", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/misc", entries, count, MAX_DOCUMENTS);
  collectDocsFromDir("/journal/archive", entries, count, MAX_DOCUMENTS);

  if (count == 0) {
    showStatusMessage("No documents found", 2000);
    return;
  }

  // Sort alphabetically by display name
  for (int i = 0; i < count - 1; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (entries[j].displayName < entries[i].displayName) {
        DocumentEntry tmp = entries[i];
        entries[i] = entries[j];
        entries[j] = tmp;
      }
    }
  }

  int selectedIndex = 0;
  int scrollOffset = 0;

  int screenH = M5Cardputer.Display.height();
  int rows = (screenH - lineHeight * 4) / lineHeight;
  if (rows < 1) rows = 1;

  drawDocumentList(entries, count, selectedIndex, scrollOffset);

  bool browsing = true;
  while (browsing) {
    M5Cardputer.update();
    updateRTC();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

      // Open on ENTER
      if (ks.enter && count > 0) {
        currentFilePath = entries[selectedIndex].path;
        saveLastEditedFilePath();
        appMode = MODE_EDITOR;
        currentMenu = MENU_NONE;

        loadFromFile();
        cursorIndex = textBuffer.length();
        rebuildLayout();
        ensureCursorVisible();

        bufferDirty = false;
        savedAfterIdle = false;
        lastKeyPressTime = millis();
        needsRedraw = true;

        browsing = false;
        continue;
      }

      for (auto c : ks.word) {
        // Back / escape
        if (c == '`' || c == 27) {
          browsing = false;
          break;
        }

        // Rename current document with 'd' / 'D'
        if (c == 'd' || c == 'D') {
          // Run rename UI; if successful, re-sort and keep selection on renamed file
          if (renameDocumentEntry(entries, count, selectedIndex, scrollOffset)) {
            String targetPath = entries[selectedIndex].path;

            // Re-run the same alphabetical sort as above
            for (int i = 0; i < count - 1; ++i) {
              for (int j = i + 1; j < count; ++j) {
                if (entries[j].displayName < entries[i].displayName) {
                  DocumentEntry tmp = entries[i];
                  entries[i] = entries[j];
                  entries[j] = tmp;
                }
              }
            }

            // Find the renamed item in the sorted list
            for (int i = 0; i < count; ++i) {
              if (entries[i].path == targetPath) {
                selectedIndex = i;
                break;
              }
            }

            // Keep it in view
            if (selectedIndex < scrollOffset) {
              scrollOffset = selectedIndex;
            }
            if (selectedIndex >= scrollOffset + rows) {
              scrollOffset = selectedIndex - rows + 1;
            }

            drawDocumentList(entries, count, selectedIndex, scrollOffset);
          }
          // Go to next key
          continue;
        }

        // Move selection up (W or ; as arrow-up)
        if (c == 'w' || c == 'W' || c == ';') {
          if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
              scrollOffset = selectedIndex;
            }
            drawDocumentList(entries, count, selectedIndex, scrollOffset);
          }
        }

        // Move selection down (S or . as arrow-down)
        if (c == 's' || c == 'S' || c == '.') {
          if (selectedIndex < count - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + rows) {
              scrollOffset = selectedIndex - rows + 1;
            }
            drawDocumentList(entries, count, selectedIndex, scrollOffset);
          }
        }
      }
    }

    delay(5);
  }

  // When we drop out (either open or back), ask main loop to redraw
  needsRedraw = true;
}

// ---------- Storage & SD helpers ----------

// Non-recursive size of a single folder (files only, no subfolders)
static unsigned long getFolderSize(const char *dirPath, unsigned long &fileCount) {
  fileCount = 0;
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  unsigned long total = 0;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      total += f.size();
      fileCount++;
    }
    f.close();
  }
  dir.close();
  return total;
}

// Delete .txt files in a folder.
// If excludeSystem is true, config/readme files are NOT deleted. Root docs are no longer used.
static unsigned long deleteTxtInDir(const char *dirPath, bool excludeSystem) {
  unsigned long deleted = 0;
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      String name = String(f.name());
      String lower = name;
      lower.toLowerCase();

      bool isTxt = lower.endsWith(".txt");
      bool isRtc = (lower == "rtc.txt");
      bool isCfg = (lower == "config-readme.txt");

      if (isTxt && !(excludeSystem && (isRtc || isCfg))) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";

        int slashPos = name.lastIndexOf('/');
        if (slashPos >= 0) {
          fullPath += name.substring(slashPos + 1);
        } else {
          fullPath += name;
        }

        if (SD.remove(fullPath.c_str())) {
          deleted++;
        }
      }
    }

    f.close();
  }

  dir.close();
  return deleted;
}

// Delete .wav files in a folder (audio notes).
static unsigned long deleteWavInDir(const char *dirPath) {
  unsigned long deleted = 0;
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      String name = String(f.name());
      String lower = name;
      lower.toLowerCase();

      if (lower.endsWith(".wav")) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";

        int slashPos = name.lastIndexOf('/');
        if (slashPos >= 0) {
          fullPath += name.substring(slashPos + 1);
        } else {
          fullPath += name;
        }

        if (SD.remove(fullPath.c_str())) {
          deleted++;
        }
      }
    }

    f.close();
  }

  dir.close();
  return deleted;
}

// Delete all files (and contents of subdirectories) starting at dirPath.
// We leave empty directories; they will be recreated by ensureJournalDirs().
static void deleteAllFilesRecursively(const char *dirPath, bool preserveSyncCfg) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    String name = String(f.name());
    if (name.length() == 0) {
      f.close();
      continue;
    }

    if (f.isDirectory()) {
      // Build sub-path
      String subPath;
      if (strcmp(dirPath, "/") == 0) {
        subPath = String("/") + name;
      } else {
        subPath = String(dirPath);
        if (!subPath.endsWith("/")) subPath += "/";
        subPath += name;
      }
      f.close();

      // Recurse into subdirectory
      deleteAllFilesRecursively(subPath.c_str(), preserveSyncCfg);

      // We deliberately do *not* try to remove the directory itself here;
      // empty dirs are harmless and /journal will be recreated cleanly.
    } else {
      // Build full file path
      String fullPath;
      if (strcmp(dirPath, "/") == 0) {
        fullPath = String("/") + name;
      } else {
        fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += name;
      }
      f.close();

      // Optionally preserve wifi.cfg and gdrive.cfg
      if (preserveSyncCfg) {
        if (fullPath == WIFI_CFG_PATH || fullPath == GDRIVE_CFG_PATH) {
          continue;  // skip deleting these files
        }
      }

      SD.remove(fullPath.c_str());
    }
  }

  dir.close();
}


// Show a simple storage summary for /journal
void showJournalStorageSummary() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 2000);
    return;
  }

  unsigned long filesRoot = 0, filesDaily = 0, filesNotes = 0, filesMeet = 0;
  unsigned long filesProj = 0, filesTravel = 0, filesMisc = 0, filesArch = 0;
  unsigned long filesAudio = 0, filesAudioExport = 0, filesConfig = 0;

  unsigned long bytesRoot = getFolderSize("/journal", filesRoot);
  unsigned long bytesConfig = getFolderSize("/journal/config", filesConfig);
  unsigned long bytesDaily = getFolderSize("/journal/daily", filesDaily);
  unsigned long bytesNotes = getFolderSize("/journal/notes", filesNotes);
  unsigned long bytesMeet = getFolderSize("/journal/meetings", filesMeet);
  unsigned long bytesProj = getFolderSize("/journal/projects", filesProj);
  unsigned long bytesTravel = getFolderSize("/journal/travel", filesTravel);
  unsigned long bytesMisc = getFolderSize("/journal/misc", filesMisc);
  unsigned long bytesArch = getFolderSize("/journal/archive", filesArch);
  unsigned long bytesAudio = getFolderSize("/journal/audio", filesAudio);
  unsigned long bytesAudioExport = getFolderSize("/journal/audio/export", filesAudioExport);

  unsigned long totalBytes = bytesRoot + bytesConfig + bytesDaily + bytesNotes + bytesMeet + bytesProj + bytesTravel + bytesMisc + bytesArch + bytesAudio + bytesAudioExport;
  unsigned long totalFiles = filesRoot + filesConfig + filesDaily + filesNotes + filesMeet + filesProj + filesTravel + filesMisc + filesArch + filesAudio + filesAudioExport;

  unsigned long audioBytes = bytesAudio + bytesAudioExport;
  unsigned long audioFiles = filesAudio + filesAudioExport;

  unsigned long textBytes = (totalBytes > audioBytes) ? (totalBytes - audioBytes) : 0;
  unsigned long textFiles = (totalFiles > audioFiles) ? (totalFiles - audioFiles) : 0;

  unsigned long totalKB = totalBytes / 1024UL;
  unsigned long totalMB = totalKB / 1024UL;
  unsigned long remKB = totalKB % 1024UL;

  unsigned long textKB = textBytes / 1024UL;
  unsigned long textMB = textKB / 1024UL;
  unsigned long textRem = textKB % 1024UL;

  unsigned long audioKB = audioBytes / 1024UL;
  unsigned long audioMB = audioKB / 1024UL;
  unsigned long audioRem = audioKB % 1024UL;

  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.print("Storage & SD tools");

  int y = lineHeight * 2;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.printf("Journal total: %lu MB %lu KB (%lu files)",
                             totalMB, remKB, totalFiles);

  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.printf("Text/config:  %lu MB %lu KB (%lu files)",
                             textMB, textRem, textFiles);

  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.printf("Audio notes:  %lu MB %lu KB (%lu files)",
                             audioMB, audioRem, audioFiles);

  int screenH = M5Cardputer.Display.height();
  int screenW = M5Cardputer.Display.width();
  int footerY = screenH - lineHeight;

  M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
  M5Cardputer.Display.setCursor(0, footerY);
  M5Cardputer.Display.print("` / ESC: back");

  // Wait for ` or ESC
  bool done = false;
  while (!done) {
    M5Cardputer.update();
    updateRTC();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
      for (auto c : ks.word) {
        if (c == '`' || c == 27) {
          done = true;
          break;
        }
      }
    }
    delay(5);
  }

  needsRedraw = true;
}

// Delete all user text documents (keeps rtc/config/etc.)
void deleteAllJournalDocuments() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 2000);
    return;
  }

  unsigned long deleted = 0;
  deleted += deleteTxtInDir("/journal/daily", false);
  deleted += deleteTxtInDir("/journal/notes", false);
  deleted += deleteTxtInDir("/journal/meetings", false);
  deleted += deleteTxtInDir("/journal/projects", false);
  deleted += deleteTxtInDir("/journal/travel", false);
  deleted += deleteTxtInDir("/journal/misc", false);
  deleted += deleteTxtInDir("/journal/archive", false);

  if (deleted == 0) {
    showStatusMessage("No text docs to delete", 2000);
  } else {
    String msg = "Deleted " + String(deleted) + " text docs";
    showStatusMessage(msg, 2000);
  }
}

// Delete all audio notes (.wav in /journal/audio)
void deleteAllAudioNotes() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 2000);
    return;
  }

  unsigned long deleted = deleteWavInDir("/journal/audio");

  if (deleted == 0) {
    showStatusMessage("No audio notes to delete", 2000);
  } else {
    String msg = "Deleted " + String(deleted) + " audio notes";
    showStatusMessage(msg, 2000);
  }
}

void runFactoryReset() {
  // Warning screen
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int y = 0;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("FACTORY RESET");

  y += lineHeight * 2;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("This will erase ALL files");
  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("on the SD card, including");
  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("all notes and audio.");

  y += lineHeight * 2;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("Press D to confirm reset.");
  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("Press ` or ESC to cancel.");

  bool waiting = true;
  bool confirmed = false;

  while (waiting) {
    M5Cardputer.update();
    updateRTC();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
      for (auto c : ks.word) {
        if (c == '`' || c == 27) {
          // Cancel
          waiting = false;
          confirmed = false;
          break;
        }
        if (c == 'd' || c == 'D') {
          // Confirm
          waiting = false;
          confirmed = true;
          break;
        }
      }
    }
    delay(5);
  }

  if (!confirmed) {
    showStatusMessage("Factory reset cancelled", 2000);
    needsRedraw = true;
    return;
  }

  // Ask whether to keep WiFi / Google Drive config files
  bool keepSyncCfg = false;
  {
    M5Cardputer.Display.fillScreen(editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

    int y2 = 0;
    M5Cardputer.Display.setCursor(0, y2);
    M5Cardputer.Display.println("KEEP WIFI / GDRIVE CFG?");
    y2 += lineHeight * 2;
    M5Cardputer.Display.setCursor(0, y2);
    M5Cardputer.Display.println("Keep wifi.cfg & gdrive.cfg?");
    y2 += lineHeight;
    M5Cardputer.Display.setCursor(0, y2);
    M5Cardputer.Display.println("Y = keep them");
    y2 += lineHeight;
    M5Cardputer.Display.setCursor(0, y2);
    M5Cardputer.Display.println("N = wipe everything");
    y2 += lineHeight * 2;
    M5Cardputer.Display.setCursor(0, y2);
    M5Cardputer.Display.println("` / ESC = cancel reset");

    bool deciding = true;
    while (deciding) {
      M5Cardputer.update();
      updateRTC();

      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState ks2 = M5Cardputer.Keyboard.keysState();
        for (auto c : ks2.word) {
          if (c == '`' || c == 27) {
            showStatusMessage("Factory reset cancelled", 2000);
            needsRedraw = true;
            return;
          }
          if (c == 'y' || c == 'Y') {
            keepSyncCfg = true;
            deciding = false;
            break;
          }
          if (c == 'n' || c == 'N') {
            keepSyncCfg = false;
            deciding = false;
            break;
          }
        }
      }
      delay(5);
    }
  }


  // Perform the actual reset
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    showStatusMessage("SD error", 2000);
    return;
  }

  // Wipe everything on the SD card
  deleteAllFilesRecursively("/", keepSyncCfg);


  // Rebuild journal structure and default config/template files
  ensureJournalDirs();       // recreates /journal tree + /journal/config files/README
  ensureDefaultTemplates();  // recreates template .tpl files
  loadTemplates();           // refresh in-memory template list

  // Reset editor state to a clean default: create a standard blank note in /journal/misc
  createNewBlankNote();
  lastKeyPressTime = millis();

  showStatusMessage("Factory reset complete", 2000);
  needsRedraw = true;
}


// ---------- USB mass storage mode ----------

static int32_t usbMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (bufsize == 0 || offset >= USB_MSC_BLOCK_SIZE) {
    return -1;
  }

  uint8_t *dst = (uint8_t *)buffer;
  uint8_t sectorBuf[USB_MSC_BLOCK_SIZE];
  uint32_t remaining = bufsize;
  uint32_t currentLba = lba;
  uint32_t currentOffset = offset;

  while (remaining > 0) {
    uint32_t chunk = USB_MSC_BLOCK_SIZE - currentOffset;
    if (chunk > remaining) chunk = remaining;

    if (currentOffset == 0 && chunk == USB_MSC_BLOCK_SIZE) {
      if (!SD.readRAW(dst, currentLba)) {
        return -1;
      }
    } else {
      if (!SD.readRAW(sectorBuf, currentLba)) {
        return -1;
      }
      memcpy(dst, sectorBuf + currentOffset, chunk);
    }

    dst += chunk;
    remaining -= chunk;
    currentLba++;
    currentOffset = 0;
  }

  return (int32_t)bufsize;
}

static int32_t usbMscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (bufsize == 0 || offset >= USB_MSC_BLOCK_SIZE) {
    return -1;
  }

  uint8_t sectorBuf[USB_MSC_BLOCK_SIZE];
  uint32_t remaining = bufsize;
  uint32_t currentLba = lba;
  uint32_t currentOffset = offset;
  uint8_t *src = buffer;

  while (remaining > 0) {
    uint32_t chunk = USB_MSC_BLOCK_SIZE - currentOffset;
    if (chunk > remaining) chunk = remaining;

    if (currentOffset == 0 && chunk == USB_MSC_BLOCK_SIZE) {
      if (!SD.writeRAW(src, currentLba)) {
        return -1;
      }
    } else {
      if (!SD.readRAW(sectorBuf, currentLba)) {
        return -1;
      }
      memcpy(sectorBuf + currentOffset, src, chunk);
      if (!SD.writeRAW(sectorBuf, currentLba)) {
        return -1;
      }
    }

    src += chunk;
    remaining -= chunk;
    currentLba++;
    currentOffset = 0;
  }

  return (int32_t)bufsize;
}

static bool usbMscStartStop(uint8_t power_condition, bool start, bool load_eject) {
  (void)power_condition;
  (void)start;
  (void)load_eject;
  return true;
}


void initUsbStorageDevice(bool presentOnStart) {
#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
  if (usbMscConfigured) {
    return;
  }

  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }

  usbMscBlockCount = SD.numSectors();
  if (usbMscBlockCount == 0) {
    return;
  }

  // Create the USB MSC object only inside the dedicated storage boot mode.
  // Normal Tiny Journal mode keeps ownership of the SD card.
  if (usbMsc == nullptr) {
    usbMsc = new USBMSC();
  }
  if (usbMsc == nullptr) {
    return;
  }

  usbMsc->vendorID("TinyJ");
  usbMsc->productID("Journal SD");
  usbMsc->productRevision("1.0");
  usbMsc->onRead(usbMscRead);
  usbMsc->onWrite(usbMscWrite);
  usbMsc->onStartStop(usbMscStartStop);
  usbMsc->mediaPresent(presentOnStart);

  usbMscConfigured = usbMsc->begin(usbMscBlockCount, USB_MSC_BLOCK_SIZE);
  if (usbMscConfigured) {
    USB.begin();
  }
#endif
}

void drawUsbStorageModeScreen() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int y = 14;
  const int lineStep = 24;

  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("Switch power");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("off then");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("connect over");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("USB.");

  M5Cardputer.Display.setTextSize(1);
}

void enterUsbStorageMode() {
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE == 1)
  showStatusMessage("Set USB Mode: USB-OTG", 3000);
  needsRedraw = true;
  return;
#else
  if (bufferDirty) {
    saveToFile();
    bufferDirty = false;
    savedAfterIdle = true;
  }

  ensureJournalDirs();

  File flag = SD.open(USB_STORAGE_FLAG_PATH, FILE_WRITE);
  if (!flag) {
    showStatusMessage("USB flag write failed", 3000);
    needsRedraw = true;
    return;
  }
  flag.println("1");
  flag.close();

  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(8, 32);
  M5Cardputer.Display.println("Restarting");
  M5Cardputer.Display.setCursor(8, 56);
  M5Cardputer.Display.println("USB mode...");

  delay(600);
  ESP.restart();
#endif
}

void runUsbStorageBootMode() {
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE == 1)
  SD.remove(USB_STORAGE_FLAG_PATH);
  return;
#else
  drawUsbStorageModeScreen();

  initUsbStorageDevice(true);
  if (!usbMscConfigured || usbMscBlockCount == 0) {
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setCursor(8, 120);
    M5Cardputer.Display.println("USB MSC failed.");
    delay(2500);
    SD.remove(USB_STORAGE_FLAG_PATH);
    ESP.restart();
  }

  // Dedicated USB storage mode: do not start the editor, BLE, Wi-Fi, autosave, or other SD writers.
  // Eject/unmount from the computer first, then press any Cardputer key to leave this mode.
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      break;
    }
    delay(20);
  }

  usbMsc->mediaPresent(false);
  SD.remove(USB_STORAGE_FLAG_PATH);

  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(8, 44);
  M5Cardputer.Display.println("Restarting");
  M5Cardputer.Display.setCursor(8, 68);
  M5Cardputer.Display.println("normal mode...");
  delay(600);
  ESP.restart();
#endif
}


// ---------- Global menu opener ----------

void openMainMenu() {
  appMode = MODE_MENU;
  currentMenu = MENU_MAIN;
  menuSelectedIndex = 0;
  menuScrollOffset = 0;
  haveLastAction = false;
  keyHeld = false;
  needsRedraw = true;
}

// ---------- Keyboard handling ----------

void handleKeyboard() {
  if (!M5Cardputer.Keyboard.isChange()) {
    return;
  }
  if (!M5Cardputer.Keyboard.isPressed()) {
    return;
  }

  unsigned long now = millis();
  lastKeyPressTime = now;
  savedAfterIdle = false;

  keyPressStartTime = now;
  keyHeld = false;

  Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();
  bool fnDown = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);

#ifdef KEY_OPT
  bool optDown = M5Cardputer.Keyboard.isKeyPressed(KEY_OPT);
#else
  bool optDown = false;
#endif

#ifdef KEY_ESC
  bool escDown = M5Cardputer.Keyboard.isKeyPressed(KEY_ESC);
#else
  bool escDown = false;
#endif

  bool shiftDown = false;
#ifdef KEY_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_SHIFT)) shiftDown = true;
#endif
#ifdef KEY_LEFT_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_SHIFT)) shiftDown = true;
#endif
#ifdef KEY_RIGHT_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT_SHIFT)) shiftDown = true;
#endif

#ifdef KEY_TAB
  bool tabDown = M5Cardputer.Keyboard.isKeyPressed(KEY_TAB);
#else
  bool tabDown = false;
#endif

  bool escCombo = false;

  if (escDown) {
    escCombo = true;
  }

  if (fnDown) {
    for (auto c : ks.word) {
      if (c == '`' || c == 27) {
        escCombo = true;
        break;
      }
    }
  }

  if (fnDown && escCombo) {
    if (appMode == MODE_EDITOR) {
      openMainMenu();
    } else {
      appMode = MODE_EDITOR;
      haveLastAction = false;
      keyHeld = false;
      needsRedraw = true;
    }
    return;
  }

  if (optDown && tabDown) {
    startAudioNote();
    return;
  }

  if (appMode == MODE_MENU && currentMenu == MENU_SETTINGS_DATETIME) {
    handleDateTimeEditorKeys(ks);
    return;
  }

  if (appMode == MODE_MENU && currentMenu == MENU_SETTINGS_ABOUT) {
    handleAboutScreenKeys(ks);
    return;
  }

  if (appMode == MODE_MENU && currentMenu == MENU_SETTINGS_BT) {
    handleBluetoothKeyboardSetupKeys(ks);
    return;
  }

  if (appMode == MODE_MENU && currentMenu == MENU_SETTINGS_WIFI) {
    handleWiFiSetupKeys(ks);
    return;
  }

  if (appMode == MODE_MENU) {
    if (ks.del) {
      menuBack();
      needsRedraw = true;
      return;
    }

    if (ks.enter) {
      menuSelect();
      needsRedraw = true;
      return;
    }

    // Built-in keyboard: ';' = up, '.' = down in menus
    for (auto c : ks.word) {
      if (c == ';') {
        menuMoveUp();
        needsRedraw = true;
      } else if (c == '.') {
        menuMoveDown();
        needsRedraw = true;
      }
    }

    return;
  }

  // ===== MODE_EDITOR from here =====

  if (tabDown && !fnDown && !optDown && !shiftDown) {
    insertTab();
    setLastAction(ACTION_INSERT_CHAR, '\t');
    needsRedraw = true;
    return;
  }


  if (ks.del) {
    if (fnDown) {
      deleteForwardChar();
      setLastAction(ACTION_DELETE_FORWARD, 0);
    } else if (optDown) {
      deleteWordBackwards();
      setLastAction(ACTION_DELETE_WORD_BACK, 0);
    } else {
      backspaceChar();
      setLastAction(ACTION_BACKSPACE, 0);
    }
    needsRedraw = true;
    return;
  }

  if (fnDown && !optDown) {
    for (auto c : ks.word) {
      switch (c) {
        case ';':
          moveCursorUp();
          setLastAction(ACTION_MOVE_UP, 0);
          break;
        case '.':
          moveCursorDown();
          setLastAction(ACTION_MOVE_DOWN, 0);
          break;
        case ',':
          moveCursorLeft();
          setLastAction(ACTION_MOVE_LEFT, 0);
          break;
        case '/':
          moveCursorRight();
          setLastAction(ACTION_MOVE_RIGHT, 0);
          break;
        default: break;
      }
    }
    needsRedraw = true;
    return;
  }

  if (optDown && !fnDown) {
    for (auto c : ks.word) {
      switch (c) {
        case ',':
          moveCursorWordLeft();
          setLastAction(ACTION_WORD_LEFT, 0);
          break;
        case '/':
          moveCursorWordRight();
          setLastAction(ACTION_WORD_RIGHT, 0);
          break;
        case ';':
          scrollPageUp();
          setLastAction(ACTION_PAGE_UP, 0);
          break;
        case '.':
          scrollPageDown();
          setLastAction(ACTION_PAGE_DOWN, 0);
          break;
        default: break;
      }
    }
    needsRedraw = true;
    return;
  }

  bool didNewline = false;

  for (auto c : ks.word) {
    if ((c == '\r' || c == '\n') && !didNewline) {
      insertNewline();
      setLastAction(ACTION_INSERT_CHAR, '\n');
      didNewline = true;
    } else if (c == '\t') {
      insertTab();
      setLastAction(ACTION_INSERT_CHAR, '\t');
    } else {
      insertChar(c);
      setLastAction(ACTION_INSERT_CHAR, c);
    }
  }

  if (ks.enter && !didNewline) {
    insertNewline();
    setLastAction(ACTION_INSERT_CHAR, '\n');
  }
}


// ---------- UTF-8 helpers ----------

bool utf8IsContinuationByte(uint8_t b) {
  return (b & 0xC0) == 0x80;
}

size_t utf8CharLenAt(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx >= len) return 0;

  uint8_t c = (uint8_t)textBuffer[idx];
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0 && idx + 1 < len) return 2;
  if ((c & 0xF0) == 0xE0 && idx + 2 < len) return 3;
  if ((c & 0xF8) == 0xF0 && idx + 3 < len) return 4;

  // Invalid/truncated UTF-8: treat as one byte so we always advance.
  return 1;
}

size_t utf8NextIndex(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx >= len) return len;
  size_t step = utf8CharLenAt(idx);
  if (step < 1) step = 1;
  idx += step;
  if (idx > len) idx = len;
  return idx;
}

size_t utf8PrevIndex(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx > len) idx = len;
  if (idx == 0) return 0;

  idx--;
  while (idx > 0 && utf8IsContinuationByte((uint8_t)textBuffer[idx])) {
    idx--;
  }
  return idx;
}

String utf8CharStringAt(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx >= len) return String("");
  size_t charLen = utf8CharLenAt(idx);
  if (charLen < 1) charLen = 1;
  if (idx + charLen > len) charLen = len - idx;
  return textBuffer.substring(idx, idx + charLen);
}

int visualColsAt(size_t idx, int currentCol) {
  if (idx >= (size_t)textBuffer.length()) return 0;
  char c = textBuffer[idx];
  if (c == '\t') {
    int remaining = TAB_SIZE - (currentCol % TAB_SIZE);
    if (remaining <= 0) remaining = TAB_SIZE;
    return remaining;
  }
  return 1;
}

// ---------- Editing primitives ----------

void insertChar(char c) {
  char text[2] = { c, '\0' };
  insertText(text);
}

void insertText(const char *text) {
  if (!text || text[0] == '\0') return;

  cursorIndex = clampCursorIndex(cursorIndex);
  String insertValue = String(text);
  textBuffer = textBuffer.substring(0, cursorIndex) + insertValue + textBuffer.substring(cursorIndex);
  cursorIndex += insertValue.length();
  bufferDirty = true;
  rebuildLayout();
  ensureCursorVisible();
  needsRedraw = true;
}

void insertNewline() {
  insertChar('\n');
}

void insertTab() {
  insertChar('\t');
}

void backspaceChar() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex == 0) return;

  size_t prevIndex = utf8PrevIndex(cursorIndex);
  textBuffer.remove(prevIndex, cursorIndex - prevIndex);
  cursorIndex = prevIndex;
  bufferDirty = true;
  rebuildLayout();
  ensureCursorVisible();
  needsRedraw = true;
}

void deleteForwardChar() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex >= (size_t)textBuffer.length()) return;

  size_t nextIndex = utf8NextIndex(cursorIndex);
  textBuffer.remove(cursorIndex, nextIndex - cursorIndex);
  bufferDirty = true;
  rebuildLayout();
  ensureCursorVisible();
  needsRedraw = true;
}

void deleteWordBackwards() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex == 0) return;

  size_t i = cursorIndex;

  while (i > 0 && (textBuffer[i - 1] == ' ' || textBuffer[i - 1] == '\t' || textBuffer[i - 1] == '\n')) {
    i--;
  }

  while (i > 0 && !(textBuffer[i - 1] == ' ' || textBuffer[i - 1] == '\n' || textBuffer[i - 1] == '\t')) {
    i--;
  }

  if (i < cursorIndex) {
    textBuffer.remove(i, cursorIndex - i);
    cursorIndex = i;
    bufferDirty = true;
    rebuildLayout();
    ensureCursorVisible();
    needsRedraw = true;
  }
}

void deleteWordForwards() {
  cursorIndex = clampCursorIndex(cursorIndex);
  size_t len = textBuffer.length();
  if (cursorIndex >= len) return;

  size_t i = cursorIndex;

  // Skip initial whitespace
  while (i < len && (textBuffer[i] == ' ' || textBuffer[i] == '\t' || textBuffer[i] == '\n')) {
    i++;
  }

  // Skip the next word
  while (i < len && !(textBuffer[i] == ' ' || textBuffer[i] == '\t' || textBuffer[i] == '\n')) {
    i++;
  }

  if (i > cursorIndex) {
    textBuffer.remove(cursorIndex, i - cursorIndex);
    bufferDirty = true;
    rebuildLayout();
    ensureCursorVisible();
    needsRedraw = true;
  }
}


// ---------- Cursor movement ----------

void moveCursorLeft() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex > 0) {
    cursorIndex = utf8PrevIndex(cursorIndex);
  }
  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorRight() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex < (size_t)textBuffer.length()) {
    cursorIndex = utf8NextIndex(cursorIndex);
  }
  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorUp() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);

  if (line <= 0) {
    line = 0;
  } else {
    line -= 1;
  }

  if (preferredCol < 0) preferredCol = col;
  VisualLine &target = visualLines[line];
  cursorIndex = colToIndexInLine(target, preferredCol);
  cursorIndex = clampCursorIndex(cursorIndex);

  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorDown() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);

  int lastLine = (int)visualLines.size() - 1;
  if (line >= lastLine) {
    line = lastLine;
  } else {
    line += 1;
  }

  if (preferredCol < 0) preferredCol = col;
  VisualLine &target = visualLines[line];
  cursorIndex = colToIndexInLine(target, preferredCol);
  cursorIndex = clampCursorIndex(cursorIndex);

  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorWordLeft() {
  if (textBuffer.length() == 0) return;

  cursorIndex = clampCursorIndex(cursorIndex);
  size_t i = cursorIndex;
  if (i == 0) return;

  while (i > 0 && isspace((unsigned char)textBuffer[i - 1])) {
    i--;
  }
  while (i > 0 && !isspace((unsigned char)textBuffer[i - 1])) {
    i--;
  }

  cursorIndex = i;
  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorWordRight() {
  if (textBuffer.length() == 0) return;

  cursorIndex = clampCursorIndex(cursorIndex);
  size_t len = textBuffer.length();
  size_t i = cursorIndex;
  if (i >= len) return;

  while (i < len && isspace((unsigned char)textBuffer[i])) {
    i++;
  }
  while (i < len && !isspace((unsigned char)textBuffer[i])) {
    i++;
  }

  cursorIndex = i;
  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorHome() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);

  if (line < 0 || line >= (int)visualLines.size()) return;

  VisualLine &vl = visualLines[line];
  cursorIndex = vl.start;
  cursorIndex = clampCursorIndex(cursorIndex);

  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorEnd() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);

  if (line < 0 || line >= (int)visualLines.size()) return;

  VisualLine &vl = visualLines[line];
  cursorIndex = vl.end;
  cursorIndex = clampCursorIndex(cursorIndex);

  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorDocHome() {
  cursorIndex = 0;
  cursorIndex = clampCursorIndex(cursorIndex);

  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}

void moveCursorDocEnd() {
  cursorIndex = textBuffer.length();
  cursorIndex = clampCursorIndex(cursorIndex);

  int line, col;
  computeCursorLineCol(line, col);
  preferredCol = col;
  ensureCursorVisible();
  needsRedraw = true;
}


void scrollPageUp() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);
  if (preferredCol < 0) preferredCol = col;

  line -= visibleLines;
  if (line < 0) line = 0;

  int lastLine = (int)visualLines.size() - 1;
  if (line > lastLine) line = lastLine;

  VisualLine &target = visualLines[line];
  cursorIndex = colToIndexInLine(target, preferredCol);
  cursorIndex = clampCursorIndex(cursorIndex);

  ensureCursorVisible();
  needsRedraw = true;
}

void scrollPageDown() {
  if (visualLines.empty()) return;

  int line, col;
  computeCursorLineCol(line, col);
  if (preferredCol < 0) preferredCol = col;

  int lastLine = (int)visualLines.size() - 1;
  line += visibleLines;
  if (line > lastLine) line = lastLine;

  VisualLine &target = visualLines[line];
  cursorIndex = colToIndexInLine(target, preferredCol);
  cursorIndex = clampCursorIndex(cursorIndex);

  ensureCursorVisible();
  needsRedraw = true;
}

// ---------- Layout / wrapping ----------

void rebuildLayout() {
  visualLines.clear();

  size_t n = textBuffer.length();
  if (n == 0) {
    VisualLine vl;
    vl.start = 0;
    vl.end = 0;
    visualLines.push_back(vl);
    return;
  }

  size_t index = 0;
  size_t lineStart = 0;
  int col = 0;

  size_t lastSpaceIdx = (size_t)-1;

  auto pushLine = [&](size_t start, size_t end) {
    VisualLine vl;
    vl.start = start;
    vl.end = end;
    visualLines.push_back(vl);
  };

  while (index < n) {
    char c = textBuffer[index];

    if (c == '\n') {
      pushLine(lineStart, index);
      index++;
      lineStart = index;
      col = 0;
      lastSpaceIdx = (size_t)-1;
      continue;
    }

    size_t charLen = utf8CharLenAt(index);
    if (charLen < 1) charLen = 1;
    int charCols = visualColsAt(index, col);
    if (charCols < 1) charCols = 1;

    if (c == ' ') {
      lastSpaceIdx = index;
    }

    if (col + charCols > maxCols) {
      // If a single character expands wider than the available line width
      // (for example a tab while maxCols is very small), force it onto its
      // own line and advance. Without this, cataracts mode can loop forever.
      if (col == 0 && charCols > maxCols) {
        size_t nextIndex = index + charLen;
        if (nextIndex > n) nextIndex = n;
        pushLine(index, nextIndex);
        index = nextIndex;
        lineStart = index;
        col = 0;
        lastSpaceIdx = (size_t)-1;
        continue;
      }

      if (lastSpaceIdx != (size_t)-1 && lastSpaceIdx >= lineStart) {
        pushLine(lineStart, lastSpaceIdx);
        index = lastSpaceIdx + 1;
        lineStart = index;
      } else {
        if (index == lineStart) {
          // Safety valve: never push an empty line without advancing.
          size_t nextIndex = index + charLen;
          if (nextIndex > n) nextIndex = n;
          pushLine(index, nextIndex);
          index = nextIndex;
          lineStart = index;
        } else {
          pushLine(lineStart, index);
          lineStart = index;
        }
      }
      col = 0;
      lastSpaceIdx = (size_t)-1;
      continue;
    }

    col += charCols;
    index += charLen;
  }

  if (lineStart <= n) {
    VisualLine vl;
    vl.start = lineStart;
    vl.end = n;
    visualLines.push_back(vl);
  }

  if (visualLines.empty()) {
    VisualLine vl;
    vl.start = 0;
    vl.end = 0;
    visualLines.push_back(vl);
  }
}

void computeCursorLineCol(int &outLine, int &outCol) {
  outLine = 0;
  outCol = 0;

  if (visualLines.empty()) {
    return;
  }

  cursorIndex = clampCursorIndex(cursorIndex);

  int lineCount = (int)visualLines.size();
  for (int i = 0; i < lineCount; ++i) {
    VisualLine &vl = visualLines[i];

    if (cursorIndex < vl.start) {
      outLine = i;
      outCol = 0;
      return;
    }
    if (cursorIndex <= vl.end) {
      // When the cursor is exactly on a soft-wrap boundary, prefer the next
      // visual line. Otherwise the cursor can sit at col == maxCols, off the
      // right edge, which is especially obvious in cataracts mode.
      bool atSoftWrapBoundary = (cursorIndex == vl.end && i + 1 < lineCount && visualLines[i + 1].start == cursorIndex);
      if (atSoftWrapBoundary) {
        continue;
      }

      outLine = i;
      int col = 0;
      for (size_t idx = vl.start; idx < cursorIndex && idx < vl.end; ++idx) {
        char c = textBuffer[idx];
        if (c == '\t') {
          int remaining = TAB_SIZE - (col % TAB_SIZE);
          if (remaining <= 0) remaining = TAB_SIZE;
          col += remaining;
        } else {
          col += 1;
        }
      }
      outCol = col;
      return;
    }
  }

  outLine = lineCount - 1;
  VisualLine &last = visualLines.back();
  int col = 0;
  for (size_t idx = last.start; idx < last.end; ++idx) {
    char c = textBuffer[idx];
    if (c == '\t') {
      int remaining = TAB_SIZE - (col % TAB_SIZE);
      if (remaining <= 0) remaining = TAB_SIZE;
      col += remaining;
    } else {
      col += 1;
    }
  }
  outCol = col;
}

// Toggle between anchored and classic scroll logic (for future menu)
void setCursorAnchorMode(bool anchored) {
  cursorAnchored = anchored;
  ensureCursorVisible();
  needsRedraw = true;
}

void applyEditorTheme(int themeIndex) {
  // Normalise to 0–3
  if (themeIndex < 0) themeIndex = 0;
  themeIndex = themeIndex % 4;
  editorThemeIndex = themeIndex;

  switch (editorThemeIndex) {
    case 0:  // Green on black (default)
      editorFgColor = EDITOR_FG_COLOR;
      editorBgColor = EDITOR_BG_COLOR;
      break;

    case 1:                    // Amber on black
      editorFgColor = 0xFD20;  // amber-ish
      editorBgColor = BLACK;
      break;

    case 2:  // White on black
      editorFgColor = 0xFFFF;
      editorBgColor = BLACK;
      break;

    case 3:  // Black on white (paper mode)
    default:
      editorFgColor = BLACK;
      editorBgColor = 0xFFFF;
      break;
  }

  needsRedraw = true;
}


void ensureCursorVisible() {
  int line, col;
  computeCursorLineCol(line, col);

  if (visualLines.empty()) {
    firstVisibleLine = 0;
    preferredCol = 0;
    return;
  }

  int totalLines = (int)visualLines.size();

  if (editorFontSizeIndex == 2) {
    // Cataracts mode uses a simple two-row writing viewport. Ignore the normal
    // anchored/classic cursor preference so newly wrapped text remains visible.
    if (line < firstVisibleLine) {
      firstVisibleLine = line;
    } else if (line >= firstVisibleLine + visibleLines) {
      firstVisibleLine = line - visibleLines + 1;
    }

    if (firstVisibleLine < 0) firstVisibleLine = 0;
    int maxFirst = totalLines - visibleLines;
    if (maxFirst < 0) maxFirst = 0;
    if (firstVisibleLine > maxFirst) firstVisibleLine = maxFirst;

    preferredCol = col;
    return;
  }

  if (cursorAnchored) {
    int anchorRow = (visibleLines * 2) / 3;
    if (anchorRow < 0) anchorRow = 0;
    if (anchorRow >= visibleLines) anchorRow = visibleLines - 1;

    if (totalLines <= visibleLines) {
      firstVisibleLine = 0;
    } else {
      int desiredFirst = line - anchorRow;

      if (desiredFirst < 0) desiredFirst = 0;
      int maxFirst = totalLines - visibleLines;
      if (maxFirst < 0) maxFirst = 0;
      if (desiredFirst > maxFirst) desiredFirst = maxFirst;

      firstVisibleLine = desiredFirst;
    }
  } else {
    if (line < firstVisibleLine) {
      firstVisibleLine = line;
    } else if (line >= firstVisibleLine + visibleLines) {
      firstVisibleLine = line - visibleLines + 1;
    }

    if (firstVisibleLine < 0) firstVisibleLine = 0;
    if (firstVisibleLine >= totalLines) {
      firstVisibleLine = totalLines - 1;
      if (firstVisibleLine < 0) firstVisibleLine = 0;
    }
  }

  preferredCol = col;
}

size_t clampCursorIndex(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (len == 0) return 0;
  if (idx > len) return len;

  // Never leave the cursor inside a UTF-8 continuation byte.
  while (idx > 0 && idx < len && utf8IsContinuationByte((uint8_t)textBuffer[idx])) {
    idx--;
  }
  return idx;
}

size_t colToIndexInLine(const VisualLine &line, int targetCol) {
  int col = 0;
  size_t idx = line.start;
  while (idx < line.end) {
    char c = textBuffer[idx];
    int charCols = 1;
    if (c == '\t') {
      int remaining = TAB_SIZE - (col % TAB_SIZE);
      if (remaining <= 0) remaining = TAB_SIZE;
      charCols = remaining;
    }
    if (col + charCols > targetCol) {
      break;
    }
    col += charCols;
    idx++;
  }
  return idx;
}

// ---------- Key repeat helpers ----------

void setLastAction(ActionType t, char ch) {
  lastAction.type = t;
  lastAction.ch = ch;
  haveLastAction = (t != ACTION_NONE);
}

void performLastAction() {
  if (!haveLastAction || appMode != MODE_EDITOR) return;

  switch (lastAction.type) {
    case ACTION_INSERT_CHAR: insertChar(lastAction.ch); break;
    case ACTION_BACKSPACE: backspaceChar(); break;
    case ACTION_DELETE_FORWARD: deleteForwardChar(); break;
    case ACTION_DELETE_WORD_BACK: deleteWordBackwards(); break;
    case ACTION_DELETE_WORD_FORWARD: deleteWordForwards(); break;
    case ACTION_MOVE_LEFT: moveCursorLeft(); break;
    case ACTION_MOVE_RIGHT: moveCursorRight(); break;
    case ACTION_MOVE_UP: moveCursorUp(); break;
    case ACTION_MOVE_DOWN: moveCursorDown(); break;
    case ACTION_WORD_LEFT: moveCursorWordLeft(); break;
    case ACTION_WORD_RIGHT: moveCursorWordRight(); break;
    case ACTION_PAGE_UP: scrollPageUp(); break;
    case ACTION_PAGE_DOWN: scrollPageDown(); break;
    case ACTION_HOME: moveCursorHome(); break;
    case ACTION_END: moveCursorEnd(); break;
    case ACTION_DOC_HOME: moveCursorDocHome(); break;
    case ACTION_DOC_END: moveCursorDocEnd(); break;
    default: break;
  }

  lastKeyPressTime = millis();
  savedAfterIdle = false;
}

bool anyModifierPressed() {
  bool mod = false;

#ifdef KEY_FN
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) mod = true;
#endif

#ifdef KEY_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_SHIFT)) mod = true;
#endif
#ifdef KEY_LEFT_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_SHIFT)) mod = true;
#endif
#ifdef KEY_RIGHT_SHIFT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT_SHIFT)) mod = true;
#endif

#ifdef KEY_CTRL
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_CTRL)) mod = true;
#endif
#ifdef KEY_LEFT_CTRL
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_CTRL)) mod = true;
#endif
#ifdef KEY_RIGHT_CTRL
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT_CTRL)) mod = true;
#endif

#ifdef KEY_OPT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_OPT)) mod = true;
#endif

#ifdef KEY_ALT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_ALT)) mod = true;
#endif
#ifdef KEY_LEFT_ALT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_ALT)) mod = true;
#endif
#ifdef KEY_RIGHT_ALT
  if (M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT_ALT)) mod = true;
#endif

  return mod;
}


void handleKeyRepeat() {
  if (appMode != MODE_EDITOR) return;
  if (!haveLastAction) return;

  // Don’t auto-repeat when any modifier key is held (Fn/Shift/Ctrl/Opt/Alt)
  if (anyModifierPressed()) {
    keyHeld = false;
    return;
  }

  if (!M5Cardputer.Keyboard.isPressed()) {
    keyHeld = false;
    return;
  }

  unsigned long now = millis();

  if (!keyHeld) {
    if (now - keyPressStartTime >= KEY_REPEAT_DELAY_MS) {
      performLastAction();
      keyHeld = true;
      lastKeyActionTime = now;
    }
  } else {
    if (now - lastKeyActionTime >= KEY_REPEAT_RATE_MS) {
      performLastAction();
      lastKeyActionTime = now;
    }
  }
}

void handleBtKeyRepeat() {
  if (appMode != MODE_EDITOR) return;
  if (!haveLastAction) return;

  if (!btKeyboardEnabled || !btKeyboardConnected) {
    btKeyHeld = false;
    btRepeatEligible = false;
    return;
  }

  if (!btAnyKeyDown || !btRepeatEligible) {
    btKeyHeld = false;
    return;
  }

  unsigned long now = millis();

  if (!btKeyHeld) {
    if (now - btKeyPressStartTime >= KEY_REPEAT_DELAY_MS) {
      performLastAction();
      btKeyHeld = true;
      btLastKeyActionTime = now;
    }
  } else {
    if (now - btLastKeyActionTime >= KEY_REPEAT_RATE_MS) {
      performLastAction();
      btLastKeyActionTime = now;
    }
  }
}


// ---------- Menu helpers ----------

int getMenuItemCount(MenuId menu) {
  switch (menu) {
    case MENU_MAIN:
      return 4;
    case MENU_WRITING:
      return 2;
    case MENU_TEXTDOCS:
      return 4;
    case MENU_TEMPLATES:
      return (int)templates.size();
    case MENU_AUDIO:
      return 2;
    case MENU_SETTINGS:
      return 6;
    case MENU_SETTINGS_STORAGE:
      return 5;
    case MENU_SETTINGS_EDITOR:
      return 2;
    case MENU_SETTINGS_APPEARANCE:
      return 2;
    case MENU_SETTINGS_SYNC:
      return 4;
    default:
      return 0;
  }
}


String getMenuItemLabel(MenuId menu, int index) {
  switch (menu) {
    case MENU_MAIN:
      switch (index) {
        case 0: return "Return to editor";
        case 1: return "Writing menu";
        case 2: return "Settings";
        case 3: return "Enter USB storage mode";
      }
      break;

    case MENU_WRITING:
      switch (index) {
        case 0: return "Text documents";
        case 1: return "Audio notes";
      }
      break;

    case MENU_TEXTDOCS:
      switch (index) {
        case 0: return "New blank note";
        case 1: return "New daily entry";
        case 2: return "New from template...";
        case 3: return "Open document...";
      }
      break;

    case MENU_TEMPLATES:
      if (index >= 0 && index < (int)templates.size()) {
        return templates[index].name;
      }
      return String("(no templates)");

    case MENU_AUDIO:
      switch (index) {
        case 0: return "Record new note";
        case 1: return "Browse audio notes";
      }
      break;


    case MENU_SETTINGS:
      switch (index) {
        case 0: return "Sync & connectivity";
        case 1: return "Storage & SD tools";
        case 2: return "Editor behaviour";
        case 3: return "Appearance";
        case 4: return "Date & time";
        case 5: return "About Tiny Journal";  // <--- new
      }
      break;


    case MENU_SETTINGS_EDITOR:
      switch (index) {
        case 0:
          return cursorAnchored
                   ? "Cursor scroll: anchored (2/3 down)"
                   : "Cursor scroll: classic";
        case 1:
          {
            if (!idleAutosaveEnabled) {
              return "Idle autosave: OFF";
            }
            unsigned long secs = idleAutosaveDelayMs / 1000UL;
            return "Idle autosave: " + String(secs) + "s";
          }
      }
      break;

    case MENU_SETTINGS_APPEARANCE:
      switch (index) {
        case 0:
          {
            const char *name = "Unknown";
            switch (editorThemeIndex) {
              case 0: name = "Green on black"; break;
              case 1: name = "Amber on black"; break;
              case 2: name = "White on black"; break;
              case 3: name = "Black on white"; break;
            }
            return String("Colour theme: ") + name;
          }

        case 1:
          {
            const char *sizeName;
            switch (editorFontSizeIndex) {
              case 0: sizeName = "standard"; break;
              case 1: sizeName = "large"; break;
              case 2: sizeName = "cataracts mode"; break;
              default: sizeName = "standard"; break;
            }
            return String("Font size: ") + sizeName;
          }
      }
      break;

    case MENU_SETTINGS_STORAGE:
      switch (index) {
        case 0: return "Show journal storage summary";
        case 1: return "Delete all text documents";
        case 2: return "Delete all audio notes";
        case 3: return "Rebuild journal folders";
        case 4: return "Factory reset (wipe SD)";
      }
      break;


    case MENU_SETTINGS_SYNC:
      switch (index) {
        case 0: return "Sync now";
        case 1: return "WiFi setup";
        case 2: return "Sync time via WiFi";
        case 3:
          if (!btKeyboardEnabled) {
            return "Bluetooth keyboard: OFF";
          } else if (btKeyboardConnected) {
            String label = "Bluetooth keyboard: ON";
            if (btKeyboardName.length()) {
              label += " (" + btKeyboardName + ")";
            }
            return label;
          } else {
            return "Bluetooth keyboard: ON (searching...)";
          }
      }
      break;

    default:
      break;
  }
  return String("");
}

MenuId getMenuParent(MenuId menu) {
  switch (menu) {
    case MENU_MAIN: return MENU_NONE;
    case MENU_WRITING: return MENU_MAIN;
    case MENU_TEXTDOCS: return MENU_WRITING;
    case MENU_TEMPLATES: return MENU_TEXTDOCS;
    case MENU_AUDIO: return MENU_WRITING;
    case MENU_SETTINGS: return MENU_MAIN;
    case MENU_SETTINGS_STORAGE: return MENU_SETTINGS;
    case MENU_SETTINGS_EDITOR: return MENU_SETTINGS;
    case MENU_SETTINGS_APPEARANCE: return MENU_SETTINGS;
    case MENU_SETTINGS_SYNC: return MENU_SETTINGS;
    case MENU_SETTINGS_DATETIME: return MENU_SETTINGS;
    case MENU_SETTINGS_BT: return MENU_SETTINGS_SYNC;
    case MENU_SETTINGS_WIFI: return MENU_SETTINGS_SYNC;
    case MENU_SETTINGS_ABOUT: return MENU_SETTINGS;
    default: return MENU_NONE;
  }
}


void openMenu(MenuId menu) {
  currentMenu = menu;
  menuSelectedIndex = 0;
  menuScrollOffset = 0;
  needsRedraw = true;
}

void menuMoveUp() {
  int count = getMenuItemCount(currentMenu);
  if (count <= 0) return;
  if (menuSelectedIndex > 0) {
    menuSelectedIndex--;
  }
  if (menuSelectedIndex < menuScrollOffset) {
    menuScrollOffset = menuSelectedIndex;
  }
}

void menuMoveDown() {
  int count = getMenuItemCount(currentMenu);
  if (count <= 0) return;
  if (menuSelectedIndex < count - 1) {
    menuSelectedIndex++;
  }
  if (menuSelectedIndex >= menuScrollOffset + listVisibleLines) {
    menuScrollOffset = menuSelectedIndex - listVisibleLines + 1;
  }
}

void menuBack() {
  MenuId parent = getMenuParent(currentMenu);
  if (currentMenu == MENU_SETTINGS_DATETIME) {
    openMenu(MENU_SETTINGS);
  } else if (parent == MENU_NONE) {
    appMode = MODE_EDITOR;
  } else {
    openMenu(parent);
  }
  needsRedraw = true;
}

void openDateTimeEditor() {
  currentMenu = MENU_SETTINGS_DATETIME;
  dtEditField = 0;
  needsRedraw = true;
}

void menuSelect() {
  switch (currentMenu) {
    case MENU_MAIN:
      switch (menuSelectedIndex) {
        case 0:
          appMode = MODE_EDITOR;
          needsRedraw = true;
          break;
        case 1:
          openMenu(MENU_WRITING);
          break;
        case 2:
          openMenu(MENU_SETTINGS);
          break;
        case 3:
          enterUsbStorageMode();
          break;
      }
      break;

    case MENU_WRITING:
      switch (menuSelectedIndex) {
        case 0:
          openMenu(MENU_TEXTDOCS);
          break;
        case 1:
          openMenu(MENU_AUDIO);
          break;
      }
      break;

    case MENU_TEXTDOCS:
      switch (menuSelectedIndex) {
        case 0:
          createNewBlankNote();
          break;
        case 1:
          createNewDailyEntry();
          break;
        case 2:
          openMenu(MENU_TEMPLATES);
          break;
        case 3:
          openDocumentBrowser();
          break;
      }
      break;

    case MENU_TEMPLATES:
      if (templates.size() > 0) {
        if (menuSelectedIndex >= 0 && menuSelectedIndex < (int)templates.size()) {
          createDocumentFromTemplateIndex(menuSelectedIndex);
        }
      } else {
        showStatusMessage("No templates found", 2000);
      }
      break;

    case MENU_AUDIO:
      switch (menuSelectedIndex) {
        case 0:
          startAudioNote();
          break;
        case 1:
          startAudioNoteBrowser();
          break;
      }
      break;

    case MENU_SETTINGS:
      switch (menuSelectedIndex) {
        case 0:
          openMenu(MENU_SETTINGS_SYNC);
          break;
        case 1:
          openMenu(MENU_SETTINGS_STORAGE);
          break;
        case 2:
          openMenu(MENU_SETTINGS_EDITOR);
          break;
        case 3:
          openMenu(MENU_SETTINGS_APPEARANCE);
          break;
        case 4:
          openDateTimeEditor();
          break;
        case 5:
          openMenu(MENU_SETTINGS_ABOUT);
          break;
      }
      break;


    case MENU_SETTINGS_STORAGE:
      switch (menuSelectedIndex) {
        case 0:
          showJournalStorageSummary();
          break;
        case 1:
          deleteAllJournalDocuments();
          break;
        case 2:
          deleteAllAudioNotes();
          break;
        case 3:
          ensureJournalDirs();
          showStatusMessage("Folders/config rebuilt", 2000);
          break;
        case 4:
          runFactoryReset();
          break;
      }
      break;
    case MENU_SETTINGS_EDITOR:
      switch (menuSelectedIndex) {
        case 0:
          // Toggle scroll behaviour: anchored vs classic
          setCursorAnchorMode(!cursorAnchored);
          break;
        case 1:
          // Cycle idle autosave through OFF → 1s → 2s → 5s → 10s → OFF ...
          if (!idleAutosaveEnabled) {
            // OFF → 1s
            idleAutosaveEnabled = true;
            idleAutosaveDelayMs = 1000;
          } else {
            if (idleAutosaveDelayMs <= 1000) {
              idleAutosaveDelayMs = 2000;  // 1s → 2s
            } else if (idleAutosaveDelayMs <= 2000) {
              idleAutosaveDelayMs = 5000;  // 2s → 5s
            } else if (idleAutosaveDelayMs <= 5000) {
              idleAutosaveDelayMs = 10000;  // 5s → 10s
            } else {
              // 10s or anything else → OFF
              idleAutosaveEnabled = false;
            }
          }

          if (!idleAutosaveEnabled) {
            showStatusMessage("Idle autosave: OFF", 1500);
          } else {
            unsigned long secs = idleAutosaveDelayMs / 1000UL;
            String msg = "Idle autosave: " + String(secs) + "s";
            showStatusMessage(msg, 1500);
          }
          break;
      }
      break;

    case MENU_SETTINGS_APPEARANCE:
      switch (menuSelectedIndex) {
        case 0:
          {
            // Cycle colour theme
            int next = (editorThemeIndex + 1) % 4;
            applyEditorTheme(next);
            saveAppearanceToSD();

            const char *name = "Unknown";
            switch (editorThemeIndex) {
              case 0: name = "Green on black"; break;
              case 1: name = "Amber on black"; break;
              case 2: name = "White on black"; break;
              case 3: name = "Black on white"; break;
            }

            String msg = String("Theme: ") + name;
            showStatusMessage(msg, 1500);
            break;
          }

        case 1:
          {
            // Cycle font size: 0 = standard, 1 = large, 2 = cataracts mode
            editorFontSizeIndex = (editorFontSizeIndex + 1) % 3;

            // Recalculate editor layout and re-wrap text
            applyEditorFontSize();
            saveAppearanceToSD();
            rebuildLayout();
            ensureCursorVisible();
            needsRedraw = true;

            const char *sizeName;
            switch (editorFontSizeIndex) {
              case 0: sizeName = "standard"; break;
              case 1: sizeName = "large"; break;
              case 2: sizeName = "cataracts mode"; break;
              default: sizeName = "standard"; break;
            }
            String msg = String("Font size: ") + sizeName;
            showStatusMessage(msg, 1500);
            break;
          }
      }
      break;


    case MENU_SETTINGS_SYNC:
      switch (menuSelectedIndex) {
        case 0:
          runManualSync();
          break;
        case 1:
          openWiFiSetup();
          break;
        case 2:
          runTimeSync();
          break;
        case 3:
          openBluetoothKeyboardSetup();
          break;
      }
      break;


    default:
      break;
  }
}

// ---------- Date & time editor (MENU_SETTINGS_DATETIME) ----------

void handleDateTimeEditorKeys(const Keyboard_Class::KeysState &ks) {
  if (ks.del) {
    openMenu(MENU_SETTINGS);
    return;
  }

  if (ks.enter) {
    saveRTCToSD();
    showStatusMessage("Time saved", 2000);
    openMenu(MENU_SETTINGS);
    return;
  }

  for (auto c : ks.word) {
    if (c == ',') {
      if (dtEditField > 0) dtEditField--;
      needsRedraw = true;
    } else if (c == '/') {
      if (dtEditField < 4) dtEditField++;
      needsRedraw = true;
    } else if (c == ';') {
      switch (dtEditField) {
        case 0:
          gDateTime.year++;
          if (gDateTime.year > 2099) gDateTime.year = 2000;
          break;
        case 1:
          gDateTime.month++;
          if (gDateTime.month > 12) gDateTime.month = 1;
          break;
        case 2:
          {
            int dim = daysInMonth(gDateTime.year, gDateTime.month);
            gDateTime.day++;
            if (gDateTime.day > dim) gDateTime.day = 1;
            break;
          }
        case 3:
          gDateTime.hour++;
          if (gDateTime.hour > 23) gDateTime.hour = 0;
          break;
        case 4:
          gDateTime.minute++;
          if (gDateTime.minute > 59) gDateTime.minute = 0;
          break;
      }
      needsRedraw = true;
    } else if (c == '.') {
      switch (dtEditField) {
        case 0:
          gDateTime.year--;
          if (gDateTime.year < 2000) gDateTime.year = 2099;
          break;
        case 1:
          gDateTime.month--;
          if (gDateTime.month < 1) gDateTime.month = 12;
          break;
        case 2:
          {
            int dim = daysInMonth(gDateTime.year, gDateTime.month);
            gDateTime.day--;
            if (gDateTime.day < 1) gDateTime.day = dim;
            break;
          }
        case 3:
          gDateTime.hour--;
          if (gDateTime.hour < 0) gDateTime.hour = 23;
          break;
        case 4:
          gDateTime.minute--;
          if (gDateTime.minute < 0) gDateTime.minute = 59;
          break;
      }
      needsRedraw = true;
    }
  }
}

void handleAboutScreenKeys(const Keyboard_Class::KeysState &ks) {
  // Backspace / Del or Enter -> return to Settings menu
  if (ks.del || ks.enter) {
    openMenu(MENU_SETTINGS);
    needsRedraw = true;
    return;
  }
}

void drawDateTimeEditor() {
  M5Cardputer.Display.startWrite();
  bool menuFullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (menuFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;

  int screenW = M5Cardputer.Display.width();

  String title = "Date & time";
  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(title);

  int startY = topBarHeight;

  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.fillRect(0, startY, screenW, lineHeight, EDITOR_BG_COLOR);
  M5Cardputer.Display.setCursor(3, startY);
  M5Cardputer.Display.print(",/ move field  ;/. change");

  String dtStr = String(gDateTime.year) + "-" + twoDigits(gDateTime.month) + "-" + twoDigits(gDateTime.day) + " " + twoDigits(gDateTime.hour) + ":" + twoDigits(gDateTime.minute);

  int y = startY + lineHeight;
  M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);

  int fieldStart[5];
  int fieldLen[5];

  fieldStart[0] = 0;
  fieldLen[0] = 4;
  fieldStart[1] = 5;
  fieldLen[1] = 2;
  fieldStart[2] = 8;
  fieldLen[2] = 2;
  fieldStart[3] = 11;
  fieldLen[3] = 2;
  fieldStart[4] = 14;
  fieldLen[4] = 2;

  int x = 3;
  for (int i = 0; i < (int)dtStr.length(); ++i) {
    bool inSel = false;
    for (int f = 0; f < 5; ++f) {
      if (f == dtEditField && i >= fieldStart[f] && i < fieldStart[f] + fieldLen[f]) {
        inSel = true;
        break;
      }
    }

    if (inSel) {
      M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
      M5Cardputer.Display.fillRect(x, y, 8, lineHeight, editorFgColor);
    } else {
      M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    }


    M5Cardputer.Display.setCursor(x, y);
    M5Cardputer.Display.print(dtStr[i]);
    x += 6;
  }

  if (statusMessageUntil > millis() && statusMessage.length() > 0) {
    int by = M5Cardputer.Display.height() - lineHeight;
    M5Cardputer.Display.fillRect(0, by, screenW, lineHeight, EDITOR_BG_COLOR);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(0, by);
    M5Cardputer.Display.print(statusMessage);
  }

  M5Cardputer.Display.endWrite();
}

void drawAboutScreen() {
  M5Cardputer.Display.startWrite();
  bool menuFullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (menuFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;

  int screenW = M5Cardputer.Display.width();

  // Title bar
  String title = "About Tiny Journal";
  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(title);

  // Body text
  int y = topBarHeight;
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  // Intro
  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Tiny Journal 2026.06.06");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Author: JoeJee90 (GitHub)");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Plain-text journal for");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("M5Stack Cardputer.");
  y += lineHeight;

  // Native keyboard
  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Native keyboard:");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Fn+Esc - editor/menu");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Opt+Tab - audio note");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Fn+arrows - move cursor");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Opt+arrows - jump cursor");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Opt+Del - delete word");
  y += lineHeight;

  // Bluetooth keyboard
  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Bluetooth keyboard:");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Arrows - move cursor");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Ctrl+arrows - jump word");
  y += lineHeight;

  M5Cardputer.Display.setCursor(3, y);
  M5Cardputer.Display.print("Ctrl+Back/Del - del word");
  y += lineHeight;

  // Bottom hint
  int bottomY = M5Cardputer.Display.height() - lineHeight;
  M5Cardputer.Display.fillRect(0, bottomY, screenW, lineHeight, editorBgColor);
  M5Cardputer.Display.setCursor(0, bottomY);
  M5Cardputer.Display.print("Backspace or Enter: back");

  M5Cardputer.Display.endWrite();
}

// ---------- Status messages ----------

void showStatusMessage(const String &msg, unsigned long durationMs) {
  statusMessage = msg;
  statusMessageUntil = millis() + durationMs;
  needsRedraw = true;
}

// ---------- WiFi setup rendering ----------

void drawWiFiSetup() {
  M5Cardputer.Display.startWrite();
  bool menuFullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (menuFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;

  int screenW = M5Cardputer.Display.width();
  int startY = topBarHeight;

  String title = "WiFi setup";
  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(title);

  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  if (wifiSetupState == WIFI_STATE_LIST) {
    int count = (int)wifiNetworks.size();

    if (count == 0) {
      int y = startY;
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
      M5Cardputer.Display.setCursor(3, y);
      M5Cardputer.Display.print("No networks found");
      y += lineHeight;
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
      M5Cardputer.Display.setCursor(3, y);
      M5Cardputer.Display.print("Enter: rescan   Back: exit");
    } else {
      for (int row = 0; row < listVisibleLines; ++row) {
        int idx = wifiScrollOffset + row;
        int y = startY + row * lineHeight;
        M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);

        if (idx < 0 || idx >= count) continue;

        WiFiNetwork &net = wifiNetworks[idx];
        String label = net.ssid;
        if (net.secure) {
          label += " [lock]";
        } else {
          label += " [open]";
        }

        bool selected = (idx == wifiSelectedIndex);
        if (selected) {
          M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
          M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorFgColor);
        } else {
          M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
        }


        M5Cardputer.Display.setCursor(3, y);
        M5Cardputer.Display.print(label);
      }
    }

  } else if (wifiSetupState == WIFI_STATE_PWENTRY) {
    int y = startY;

    String ssidLabel = "(no network)";
    if (wifiSelectedIndex >= 0 && wifiSelectedIndex < (int)wifiNetworks.size()) {
      ssidLabel = wifiNetworks[wifiSelectedIndex].ssid;
    }

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("SSID: ");
    M5Cardputer.Display.print(ssidLabel);
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter password:");
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print(wifiPasswordInput);
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter: connect  Back: clear/exit");
  }

  int footerY = M5Cardputer.Display.height() - lineHeight;
  M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
  if (statusMessageUntil > millis() && statusMessage.length() > 0) {
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(0, footerY);
    M5Cardputer.Display.print(statusMessage);
  }

  M5Cardputer.Display.endWrite();
}

// ---------- Rendering (editor + menus) ----------

void drawEditor() {
  M5Cardputer.Display.startWrite();

  bool editorFullClear = (lastRenderedAppMode != MODE_EDITOR);
  if (editorFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }

  lastRenderedAppMode = MODE_EDITOR;
  lastRenderedMenu = MENU_NONE;
  // Do not clear the whole screen on every editor redraw.
  // The top bar and each visible editor row are cleared/repainted below.
  // A full clear is only used when returning from menus/special screens,
  // which prevents leftover UI fragments without bringing back typing flicker.
  int screenW = M5Cardputer.Display.width();

  // Always use "small" text settings for status / word count bars
  M5Cardputer.Display.setTextSize(1);

  // ----- Word count + saved status (top bar, unchanged size) -----
  size_t wordCount = 0;
  bool inWord = false;
  for (size_t i = 0; i < (size_t)textBuffer.length();) {
    char c = textBuffer[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (inWord) {
        wordCount++;
        inWord = false;
      }
    } else {
      inWord = true;
    }
    i = utf8NextIndex(i);
  }
  if (inWord) wordCount++;

  String countStr = String((uint32_t)wordCount);

  char savedChar = bufferDirty ? 'x' : 'o';
  String savedStr = String("saved:") + savedChar;
  String batteryStr = getBatteryStatusString();

  // Top bar background
  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);

  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(countStr);

  int batteryWidth = M5Cardputer.Display.textWidth(batteryStr);
  int batteryX = (screenW - batteryWidth) / 2;
  if (batteryX < 0) batteryX = 0;
  M5Cardputer.Display.setCursor(batteryX, textY);
  M5Cardputer.Display.print(batteryStr);

  int savedWidth = M5Cardputer.Display.textWidth(savedStr);
  int savedX = screenW - savedWidth - 3;
  if (savedX < 0) savedX = 0;
  M5Cardputer.Display.setCursor(savedX, textY);
  M5Cardputer.Display.print(savedStr);

  // ----- Editor text (scaled font) -----
  float scale;
  if (editorFontSizeIndex == 0) {
    scale = 1.0f;
  } else if (editorFontSizeIndex == 1) {
    scale = 1.4f;
  } else {
    scale = (float)CATARACTS_TEXT_SCALE;
  }

  int charWidth = (int)(baseCharWidth * scale);
  int editorLineH = (int)(baseLineHeight * scale);
  if (charWidth < 1) charWidth = 1;
  if (editorLineH < 1) editorLineH = lineHeight;

  M5Cardputer.Display.setTextSize(scale);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int cursorLine, cursorCol;
  computeCursorLineCol(cursorLine, cursorCol);

  int startY = topBarHeight;

  for (int row = 0; row < visibleLines; ++row) {
    int lineIndex = firstVisibleLine + row;
    int y = startY + row * editorLineH;

    M5Cardputer.Display.fillRect(0, y, screenW, editorLineH, editorBgColor);

    if (lineIndex < 0 || lineIndex >= (int)visualLines.size()) {
      continue;
    }

    VisualLine &vl = visualLines[lineIndex];
    int col = 0;
    size_t idx = vl.start;

    while (idx < vl.end) {
      char c = textBuffer[idx];
      int charCols = visualColsAt(idx, col);
      if (charCols < 1) charCols = 1;

      if (c == '\t') {
        for (int r = 0; r < charCols; ++r) {
          bool isCursorHere = (lineIndex == cursorLine && col == cursorCol);
          M5Cardputer.Display.setTextColor(isCursorHere ? editorBgColor : editorFgColor,
                                           isCursorHere ? editorFgColor : editorBgColor);
          M5Cardputer.Display.setCursor(EDITOR_LEFT_MARGIN + col * charWidth, y);
          M5Cardputer.Display.print(' ');
          col++;
          if (col >= maxCols) break;
        }
      } else {
        bool isCursorHere = (lineIndex == cursorLine && col == cursorCol);
        M5Cardputer.Display.setTextColor(isCursorHere ? editorBgColor : editorFgColor,
                                         isCursorHere ? editorFgColor : editorBgColor);
        M5Cardputer.Display.setCursor(EDITOR_LEFT_MARGIN + col * charWidth, y);
        M5Cardputer.Display.print(utf8CharStringAt(idx));
        col += charCols;
      }

      if (col >= maxCols) break;
      idx = utf8NextIndex(idx);
    }

    // Draw cursor block if it's beyond the last character on this visual line
    if (lineIndex == cursorLine && cursorCol >= col && cursorCol < maxCols) {
      M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
      int charX = EDITOR_LEFT_MARGIN + cursorCol * charWidth;
      M5Cardputer.Display.setCursor(charX, y);
      M5Cardputer.Display.print(' ');
    }
  }

  // ----- Bottom status message (keep small size) -----
  M5Cardputer.Display.setTextSize(1);

  if (statusMessageUntil > millis() && statusMessage.length() > 0) {
    int by = M5Cardputer.Display.height() - lineHeight;
    M5Cardputer.Display.fillRect(0, by, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(0, by);
    M5Cardputer.Display.print(statusMessage);
  }

  M5Cardputer.Display.endWrite();
}

void drawMenu() {
  if (currentMenu == MENU_SETTINGS_DATETIME) {
    drawDateTimeEditor();
    return;
  }
  if (currentMenu == MENU_SETTINGS_ABOUT) {
    drawAboutScreen();
    return;
  }
  if (currentMenu == MENU_SETTINGS_BT) {
    drawBluetoothKeyboardSetup();
    return;
  }
  if (currentMenu == MENU_SETTINGS_WIFI) {
    drawWiFiSetup();
    return;
  }


  M5Cardputer.Display.startWrite();
  bool menuFullClear = (lastRenderedAppMode != MODE_MENU || lastRenderedMenu != currentMenu);
  if (menuFullClear) {
    M5Cardputer.Display.fillScreen(BLACK);
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;

  int screenW = M5Cardputer.Display.width();

  String title;
  switch (currentMenu) {
    case MENU_MAIN: title = "Main menu"; break;
    case MENU_WRITING: title = "Writing menu"; break;
    case MENU_TEXTDOCS: title = "Text documents"; break;
    case MENU_TEMPLATES: title = "Templates"; break;
    case MENU_AUDIO: title = "Audio notes"; break;
    case MENU_SETTINGS: title = "Settings"; break;
    case MENU_SETTINGS_STORAGE: title = "Storage & SD tools"; break;
    case MENU_SETTINGS_EDITOR: title = "Editor behaviour"; break;
    case MENU_SETTINGS_APPEARANCE: title = "Appearance"; break;
    case MENU_SETTINGS_SYNC: title = "Sync & connectivity"; break;
    default: title = ""; break;
  }


  M5Cardputer.Display.fillRect(0, 0, screenW, topBarHeight, WHITE);
  int textY = (topBarHeight - lineHeight) / 2;
  if (textY < 0) textY = 0;

  M5Cardputer.Display.setTextColor(BLACK, WHITE);
  M5Cardputer.Display.setCursor(3, textY);
  M5Cardputer.Display.print(title);

  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int startY = topBarHeight;
  int count = getMenuItemCount(currentMenu);

  for (int row = 0; row < listVisibleLines; ++row) {
    int itemIndex = menuScrollOffset + row;
    int y = startY + row * lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);

    if (itemIndex < 0 || itemIndex >= count) {
      continue;
    }

    String label = getMenuItemLabel(currentMenu, itemIndex);
    bool selected = (itemIndex == menuSelectedIndex);

    if (selected) {
      M5Cardputer.Display.setTextColor(editorBgColor, editorFgColor);
      M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorFgColor);
    } else {
      M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    }


    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print(label);
  }

  int footerY = M5Cardputer.Display.height() - lineHeight;
  M5Cardputer.Display.fillRect(0, footerY, screenW, lineHeight, editorBgColor);
  if (statusMessageUntil > millis() && statusMessage.length() > 0) {
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(0, footerY);
    M5Cardputer.Display.print(statusMessage);
  }

  M5Cardputer.Display.endWrite();
}
