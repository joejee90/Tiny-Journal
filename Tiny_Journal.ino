#include <M5Cardputer.h>
#include "JournalVersion.h"
#include <SPI.h>
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "JournalNetwork.h"
#include "JournalPrinter.h"
#include <NimBLEDevice.h>
#include "JournalInput.h"
#include <vector>
#include <memory>
#include <new>
#include "JournalLayout.h"
#include "JournalTemplateMetadata.h"
#include "JournalHistory.h"
#include "JournalMenuUi.h"
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
  BT_CMD_ESCAPE,
  BT_CMD_LAYOUT, BT_CMD_UNDO, BT_CMD_REDO, BT_CMD_FIND,
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
  char text[16];  // UTF-8: max 4 bytes + NUL
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
// CDC-on-boot starts TinyUSB before setup(). Its interface set is frozen then,
// so the MSC object must register during static initialization, not in setup.
// Media stays absent in normal journal mode; only the dedicated boot mode
// publishes the SD card after editor/network writers have been excluded.
#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
static USBMSC usbMscInterface;
USBMSC *usbMsc = &usbMscInterface;
#else
USBMSC *usbMsc = nullptr;
#endif
bool usbMscConfigured = false;
volatile bool usbHostEjected = false;
volatile bool usbMediaActive = false;
uint32_t usbMscBlockCount = 0;


// ---------- Editor settings ----------
static const int TAB_SIZE = 4;
static const int EDITOR_LEFT_MARGIN = 2;
static int uiFontSizeIndex = 0;
static int aboutPage = 0;
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
  MENU_SETTINGS_ABOUT,
  MENU_SETTINGS_PRINTER
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
JournalHistory editHistory;
String historyFilePath;
void rememberEdit();
void journalUndo();
void journalRedo();
void journalFind();

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
bool saveLastEditedFilePath();

bool loadFromFile();
bool saveToFile();
bool saveDirtyDocument();
bool writeFileSafely(const String &path, const uint8_t *data, size_t length);
void recoverJournalFiles();
void recoverAudioNotes();
bool readAppKeys(Keyboard_Class::KeysState &ks);
bool anyAppKeyPressed();
void applyUiTextSize();
String fitUiLabel(const String &label, int widthPx);
int uiFooterHeight();
void drawUiFooter(const String &text);
void drawUiScrollbar(int total, int first, int visible, int topY, int height);
int uiListLabelWidth(int total, int visible);
void drawUiListLabel(const String &text, int x, int y, int widthPx, bool selected);
void pollUiAnimation();
static const size_t MAX_DOCUMENT_BYTES = 128 * 1024;
static const size_t MAX_VISUAL_LINES = 4096;
bool documentFitsMemory(const String &candidate);
static String unusedFilePath(const String &base, const String &extension);
void rebuildLayout();
void computeCursorLineCol(int &outLine, int &outCol);
void ensureCursorVisible();
void applyEditorFontSize();
float uiTextScale();
void applyUiTextSize();
String fitUiLabel(const String &label, int widthPx);
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
void ensureUserGuide();
void showUserGuide();
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
bool parseTemplateFile(const String &path);
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
bool saveWiFiConfig(const String &ssid, const String &password);
void runTimeSync();
void loadPrinterConfig();
void openPrinterSetup();
void handlePrinterSetupKeys(const Keyboard_Class::KeysState &ks);
void drawPrinterSetup();
void printCurrentDocument();
void printCompletedLine();
void pollPrinter();

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
bool saveRTCToSD();
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
void rememberEdit();
bool documentFitsMemory(const String &candidate);
bool readAppKeys(Keyboard_Class::KeysState &ks);
bool anyAppKeyPressed();

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


#ifdef TJ_DIAGNOSTICS
void beginJournalDiagnostics();
void pollJournalDiagnostics();
#endif

// ---------- Setup ----------

void setup() {
#ifdef TJ_DIAGNOSTICS
  Serial.begin(115200);
#endif
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // enableKeyboard = true
  initPowerManagement();

  initDisplay();
  initSD();
  ensureJournalDirs();

  // USB storage mode must be entered by rebooting into a dedicated mode so the host sees
  // the mass-storage interface during USB enumeration. Do this before normal app startup.
  Preferences usbPrefs;
  usbPrefs.begin("tinyjournal", false);
  bool usbRequested = usbPrefs.getBool("usb", false) || SD.exists(USB_STORAGE_FLAG_PATH);
  usbPrefs.end();
  if (usbRequested) runUsbStorageBootMode();
  recoverJournalFiles();
  recoverAudioNotes();
  ensureUserGuide();
  loadWifiConfigFromSD();
  loadPrinterConfig();
  loadAppearanceFromSD();  // <-- load saved theme, if any
  ensureDefaultTemplates();
  loadTemplates();
  initRTC();
  initBluetoothKeyboard();  // set up BLE stack for external keyboard (disabled by default)
  loadLastEditedFilePath();

  // Load the last edited/default document if it exists. On a fresh SD card, behave like
  // "New blank note" and create the first note in /journal/misc rather than /journal root.
  if (SD.exists(currentFilePath.c_str())) {
    if (!loadFromFile()) createNewBlankNote();
  } else {
    createNewBlankNote();
  }

  cursorIndex = textBuffer.length();  // start at end of file
  rebuildLayout();
  ensureCursorVisible();

  lastKeyPressTime = millis();
  savedAfterIdle = false;
  needsRedraw = true;
#ifdef TJ_DIAGNOSTICS
  beginJournalDiagnostics();
#endif
}

// ---------- Main loop ----------

void loop() {
#ifdef TJ_DIAGNOSTICS
  pollJournalDiagnostics();
#endif
  updateRTC();
  M5Cardputer.update();
  updateBatteryStatus();
  handleLowBattery();
  handleKeyboard();
  handleBluetoothInput();  // process queued chars from BT keyboard
  handleKeyRepeat();       // built-in keyboard repeat
  pollPrinter();
  pollUiAnimation();
  handleBtKeyRepeat();     // Bluetooth keyboard repeat


  unsigned long now = millis();
  if (idleAutosaveEnabled && bufferDirty && !savedAfterIdle && (now - lastKeyPressTime) >= idleAutosaveDelayMs) {
    if (saveToFile()) savedAfterIdle = true;
    // Back off a failed autosave, retaining the unsaved indicator.
    lastKeyPressTime = now;
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
  M5Cardputer.Display.setRotation(0); // M5GFX 0.3 uses upright rotation 0 on Cardputer
  M5Cardputer.Display.fillScreen(BLACK);

  M5Cardputer.Display.setFont(&fonts::efontJA_12);
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

  applyUiTextSize();
  applyEditorFontSize();
}

float uiTextScale() { return uiFontSizeIndex == 0 ? 1.0f : uiFontSizeIndex == 1 ? 1.4f : 2.0f; }

void applyUiTextSize() {
  M5Cardputer.Display.setFont(&fonts::efontJA_12);
  M5Cardputer.Display.setTextSize(uiTextScale());
  M5Cardputer.Display.setTextWrap(false, false);
  lineHeight = M5Cardputer.Display.fontHeight() + 2;
  topBarHeight = lineHeight + 4;
  listVisibleLines = std::max<int>(1, (M5Cardputer.Display.height() - topBarHeight - uiFooterHeight()) / lineHeight);
}

String fitUiLabel(const String &label, int widthPx) {
  if (M5Cardputer.Display.textWidth(label) <= widthPx) return label;
  String result = label;
  while (result.length() && M5Cardputer.Display.textWidth(result + "...") > widthPx) {
    size_t end = result.length() - 1;
    while (end && (((uint8_t)result[end] & 0xC0) == 0x80)) --end;
    result.remove(end);
  }
  return result + "...";
}

#include "JournalMenuUi.inl"
#include "JournalGuide.inl"

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

  const int topBarHeight = baseLineHeight + 6; // Always standard height.
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

  float scale = editorFontSizeIndex == 3 ? 1.4f : (editorFontSizeIndex == 0 ? 1.0f : 2.0f);

  int charWidth = baseCharWidth * scale;
  int editorLineH = baseLineHeight * scale;

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
  if (batteryLevelPercent < 0) return;
  if (batteryLevelPercent <= CRITICAL_BATTERY_LEVEL && !criticalBatteryWarningShown) {
    bool ok = saveDirtyDocument();
    showStatusMessage(ok ? "Critical battery - note saved" : "Critical battery - SAVE FAILED", 4000);
    criticalBatteryWarningShown = lowBatteryWarningShown = true;
    needsRedraw = true;
  } else if (batteryLevelPercent <= LOW_BATTERY_LEVEL && !lowBatteryWarningShown) {
    bool ok = saveDirtyDocument();
    showStatusMessage(ok ? "Low battery - note saved" : "Low battery - SAVE FAILED", 3000);
    lowBatteryWarningShown = true;
    needsRedraw = true;
  }
  if (batteryLevelPercent > LOW_BATTERY_LEVEL) {
    lowBatteryWarningShown = criticalBatteryWarningShown = false;
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

  // Restore interrupted saves before defaults could mask a missing config.
  recoverJournalFiles();
  ensureConfigTemplates();
}

// Move legacy root-level config files into /journal/config so old SD cards keep working.
void migrateConfigFileIfNeeded(const char *oldPath, const char *newPath) {
  // FAT rename keeps the sole valid copy intact if migration cannot be completed.
  if (!SD.exists(newPath) && SD.exists(oldPath) && !SD.rename(oldPath, newPath)) {
    showStatusMessage("Config migration failed; original kept", 3000);
  }
}

// Move the original root-level first note into /journal/misc for a tidy journal root.
void migrateLegacyRootJournalFile() {
  const char *oldPath = "/journal/journal.txt";
  if (!SD.exists(oldPath)) return;
  SD.mkdir("/journal/misc");
  String newPath;
  for (uint32_t suffix = 0; suffix < 100000; ++suffix) {
    String candidate = "/journal/misc/journal" + (suffix ? "-" + String(suffix) : String("")) + ".txt";
    if (!SD.exists(candidate) && !SD.exists(candidate + ".bak")) { newPath = candidate; break; }
  }
  if (!newPath.length() || !SD.rename(oldPath, newPath)) {
    showStatusMessage("Note migration failed; original kept", 3000);
    return;
  }
  if (currentFilePath == oldPath) { currentFilePath = newPath; saveLastEditedFilePath(); }
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

bool saveLastEditedFilePath() {
  if (!currentFilePath.startsWith("/journal/") || currentFilePath.startsWith("/journal/config/") || currentFilePath.startsWith("/journal/templates/")) return true;
  String value;
  bool ok = value.reserve(currentFilePath.length() + 1) &&
            value.concat(currentFilePath) && value.concat('\n') &&
            writeFileSafely(LAST_FILE_PATH, (const uint8_t *)value.c_str(), value.length());
  if (!ok) showStatusMessage("Last-opened file save failed", 4000);
  return ok;
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
  bool rejected = false;

  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      String name = f.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      String lower = name; lower.toLowerCase();
      // Ignore host metadata and unrelated files copied alongside templates.
      if (!name.startsWith(".") && lower.endsWith(".tpl")) {
        String path = String("/journal/templates/") + name;
        if (!parseTemplateFile(path)) rejected = true;
      }
    }
    f.close();
  }
  dir.close();
  if (rejected) showStatusMessage("Some templates exceed safe limits", 4000);
}

// ---------- File I/O ----------

bool documentFitsMemory(const String &candidate) {
  // Reserve for the largest font too, so changing display size cannot turn an
  // otherwise valid document into an allocation failure or boot loop.
  int minColumns = std::max<int>(1, (M5Cardputer.Display.width() - EDITOR_LEFT_MARGIN) / std::max<int>(1, baseCharWidth * CATARACTS_TEXT_SCALE));
  size_t lines = journalVisualLineCount(candidate.c_str(), candidate.length(), minColumns, MAX_VISUAL_LINES);
  if (candidate.length() > MAX_DOCUMENT_BYTES || lines > MAX_VISUAL_LINES) {
    showStatusMessage("Document exceeds safe editor capacity", 4000); return false;
  }
  if (lines > visualLines.capacity()) {
    size_t desired = max(lines, min(MAX_VISUAL_LINES, max((size_t)32, visualLines.capacity() * 2)));
    size_t required = desired * sizeof(VisualLine);
    // reserve allocates a new block while the existing vector still exists.
    if (ESP.getFreeHeap() < required + candidate.length() * 2 + 32768 || ESP.getMaxAllocHeap() < required + 1024) {
      showStatusMessage("Not enough memory to edit this document", 4000); return false;
    }
    visualLines.reserve(desired);
  }
  return true;
}

bool loadFromFile() {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) { showStatusMessage("Open failed: SD unavailable", 3000); return false; }
  String backup = currentFilePath + ".bak";
  if (!SD.exists(currentFilePath) && SD.exists(backup)) SD.rename(backup, currentFilePath);
  File f = SD.open(currentFilePath, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); showStatusMessage("Open failed; previous note kept", 3000); return false; }
  size_t size = f.size();
  // The editor rebuilds layout and temporarily copies strings during insertion.
  if (size > MAX_DOCUMENT_BYTES || ESP.getFreeHeap() < size * 3 + 32768) {
    f.close(); showStatusMessage("Note too large for available memory", 4000); return false;
  }
  String loaded;
  if (size && !loaded.reserve(size + 1)) { f.close(); showStatusMessage("Open failed: insufficient memory", 3000); return false; }
  uint8_t chunk[1024];
  size_t total = 0;
  while (total < size) {
    size_t wanted = (size - total < sizeof(chunk)) ? size - total : sizeof(chunk);
    size_t n = f.read(chunk, wanted);
    if (n != wanted || !loaded.concat((const char *)chunk, n)) {
      f.close(); showStatusMessage("Read failed; previous note kept", 3000); return false;
    }
    total += n;
  }
  f.close();
  if (!documentFitsMemory(loaded)) return false;
  textBuffer = std::move(loaded);
  bufferDirty = false;
  return true;
}

bool writeFileSafely(const String &path, const uint8_t *data, size_t length) {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) return false;
  const String temp = path + ".tmp", backup = path + ".bak";
  // Complete an interrupted rename by restoring the last known good version.
  if (!SD.exists(path) && SD.exists(backup) && !SD.rename(backup, path)) return false;
  if (SD.exists(temp) && !SD.remove(temp)) return false;
  File out = SD.open(temp, FILE_WRITE);
  if (!out) return false;
  size_t total = 0;
  while (total < length) {
    size_t n = (length - total < 1024) ? length - total : 1024;
    if (out.write(data + total, n) != n) { out.close(); SD.remove(temp); return false; }
    total += n;
  }
  out.flush();
  bool complete = out.size() == length && !out.getWriteError();
  out.close();
  if (!complete) { SD.remove(temp); return false; }
  // Read back while the original is still present. FAT cannot guarantee atomicity
  // under power loss; the backup and boot recovery cover the replacement window.
  File verify = SD.open(temp, FILE_READ);
  uint8_t chunk[512]; size_t checked = 0;
  while (verify && checked < length) {
    size_t n = (length - checked < sizeof(chunk)) ? length - checked : sizeof(chunk);
    if (verify.read(chunk, n) != n || memcmp(chunk, data + checked, n)) { complete = false; break; }
    checked += n;
  }
  if (!verify || checked != length) complete = false;
  if (verify) verify.close();
  if (!complete) { SD.remove(temp); return false; }
  bool hadOriginal = SD.exists(path);
  if (hadOriginal) {
    if (SD.exists(backup) && !SD.remove(backup)) return false;
    if (!SD.rename(path, backup)) return false;
  }
  if (!SD.rename(temp, path)) {
    if (hadOriginal) SD.rename(backup, path);
    return false;
  }
  return true;
}

bool saveToFile() {
  if (!writeFileSafely(currentFilePath, (const uint8_t *)textBuffer.c_str(), textBuffer.length())) {
    showStatusMessage("SAVE FAILED - note remains unsaved", 5000);
    needsRedraw = true;
    return false;
  }
  bufferDirty = false;
  savedAfterIdle = true;
  bool timeSaved = saveRTCToSD();
  bool lastFileSaved = saveLastEditedFilePath();
  if (!timeSaved || !lastFileSaved)
    showStatusMessage("Note saved; settings save failed", 5000);
  return true;
}

bool saveDirtyDocument() { return !bufferDirty || saveToFile(); }

static void recoverBackupsInDir(const char *dirPath) {
  File dir = SD.open(dirPath);
  if (!dir) return;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String name = f.name(); bool directory = f.isDirectory(); f.close();
    int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
    if (!directory && name.endsWith(".bak")) {
      String backup = String(dirPath) + "/" + name;
      String original = backup.substring(0, backup.length() - 4);
      if (!SD.exists(original)) SD.rename(backup, original);
    } else if (!directory && name.endsWith(".txt.tmp") && strcmp(dirPath, "/journal/config") != 0) {
      // An interrupted first save has no older version to restore. Keep any
      // partial/new text as a separately named recovery note; never silently
      // replace the committed document with an uncommitted temporary file.
      String temp = String(dirPath) + "/" + name;
      String stem = name.substring(0, name.length() - 8);
      String recovered = unusedFilePath(String(dirPath) + "/" + stem + "-unsaved-recovered", ".txt");
      if (recovered.length()) SD.rename(temp, recovered);
    }
  }
  dir.close();
}

void recoverJournalFiles() {
  const char *dirs[] = {"/journal/config", "/journal/daily", "/journal/notes", "/journal/meetings", "/journal/projects", "/journal/travel", "/journal/misc", "/journal/archive"};
  for (const char *dir : dirs) recoverBackupsInDir(dir);
}

static String unusedFilePath(const String &base, const String &extension) {
  for (uint32_t suffix = 0; suffix < 100000; ++suffix) {
    String path = base + (suffix ? "-" + String(suffix) : String("")) + extension;
    if (!SD.exists(path) && !SD.exists(path + ".bak") && !SD.exists(path + ".tmp") && !SD.exists(path + ".part")) return path;
  }
  showStatusMessage("Unable to allocate unique filename", 3000);
  return String("");
}

// ---------- Template parsing & instantiation ----------

bool parseTemplateFile(const String &path) {
  if (templates.size() >= JOURNAL_MAX_TEMPLATES || ESP.getFreeHeap() < 16384 || ESP.getMaxAllocHeap() < 8192) return false;
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory() || f.size() > MAX_DOCUMENT_BYTES) { if (f) f.close(); return false; }
  TemplateInfo info;
  info.filePath = path;
  if (info.filePath != path) { f.close(); return false; }
  size_t metadataBytes = 0;
  char metadataLine[JOURNAL_TEMPLATE_LINE_BYTES + 1];
  for (;;) {
    auto readByte = [&]() { if (!f.available()) return -1; int value = f.read(); return value < 0 ? -2 : value; };
    auto result = readJournalTemplateMetadataLine(readByte, metadataBytes, metadataLine);
    if (result == JournalTemplateLine::Invalid) { f.close(); return false; }
    if (result == JournalTemplateLine::End) break;
    String line(metadataLine);
    if (line.length() != strlen(metadataLine)) { f.close(); return false; }
    line.trim();
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

  }
  f.close();
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

  templates.push_back(std::move(info));
  return true;
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
  if (!saveDirtyDocument()) return;
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

  String path = unusedFilePath(dir + baseWithPrefix, ext);
  if (!path.length()) return;

  File f = SD.open(info.filePath.c_str(), FILE_READ);
  if (!f) return;

  String content;
  size_t templateSize = f.size();
  if (templateSize > MAX_DOCUMENT_BYTES || ESP.getFreeHeap() < templateSize * 3 + 32768 || !content.reserve(templateSize + 1)) { f.close(); showStatusMessage("Template too large", 3000); return; }
  uint8_t templateChunk[512];
  size_t templateRead = 0;
  while (templateRead < templateSize) {
    size_t wanted = std::min(sizeof(templateChunk), templateSize - templateRead);
    if (f.read(templateChunk, wanted) != wanted || !content.concat((const char *)templateChunk, wanted)) {
      f.close(); showStatusMessage("Template load failed", 3000); return;
    }
    templateRead += wanted;
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
  if (body.length() != content.length() - pos) { showStatusMessage("Template load failed", 3000); return; }
  content = "";
  String expanded = expandTemplateTokens(body);
  if (body.length() && !expanded.length()) { showStatusMessage("Template expansion failed", 3000); return; }
  body = std::move(expanded);
  if (!documentFitsMemory(body)) return;

  currentFilePath = path;
  saveLastEditedFilePath();
  textBuffer = std::move(body);
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
  if (!saveDirtyDocument()) return;
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    return;
  }
  String dir = "/journal/misc";
  if (!SD.exists(dir.c_str())) {
    SD.mkdir(dir.c_str());
  }
  unsigned long t = millis();
  String path = unusedFilePath(dir + "/note-" + String(t), ".txt");
  if (!path.length()) return;

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
  if (!saveDirtyDocument()) return;
  int idx = findTemplateIndexByName("Daily Journal");
  if (idx < 0) {
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
      return;
    }
    String dir = "/journal/daily";
    if (!SD.exists(dir.c_str())) SD.mkdir(dir.c_str());
    unsigned long t = millis();
    String path = unusedFilePath(dir + "/daily-" + String(t), ".txt");
    if (!path.length()) return;
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

bool saveRTCToSD() {
  if (!rtcInitialised || gDateTime.year < 2000 || gDateTime.year > 2099 ||
      gDateTime.month < 1 || gDateTime.month > 12 || gDateTime.day < 1 ||
      gDateTime.day > daysInMonth(gDateTime.year, gDateTime.month) ||
      gDateTime.hour < 0 || gDateTime.hour > 23 || gDateTime.minute < 0 ||
      gDateTime.minute > 59 || gDateTime.second < 0 || gDateTime.second > 59) {
    showStatusMessage("Time values invalid / not initialised", 4000);
    return false;
  }
  char line[32];
  int length = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d\n",
                        gDateTime.year, gDateTime.month, gDateTime.day,
                        gDateTime.hour, gDateTime.minute, gDateTime.second);
  bool ok = length == 20 &&
            writeFileSafely(RTC_CFG_PATH, (const uint8_t *)line, (size_t)length);
  if (!ok) showStatusMessage("Time settings save failed", 4000);
  return ok;
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
  if (wifiPassword.endsWith("\r")) wifiPassword.remove(wifiPassword.length()-1);
  f.close();

  // Treat placeholder content as "not configured"
  if (wifiSsid.length() == 0 || wifiSsid == WIFI_PLACEHOLDER_SSID) {
    wifiConfigured = false;
    return;
  }

  wifiConfigured = true;
}

bool saveWiFiConfig(const String &ssid, const String &password) {
  String config;
  bool ok = config.reserve(ssid.length() + password.length() + 2) &&
            config.concat(ssid) && config.concat('\n') &&
            config.concat(password) && config.concat('\n') &&
            writeFileSafely(WIFI_CFG_PATH, (const uint8_t *)config.c_str(), config.length());
  if (!ok)
    showStatusMessage("WiFi settings save failed", 3000);
  return ok;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) { wifiConnected=true; return true; }
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
  if (JournalPrinter::busy()) return; // Printer owns the radio until its queue finishes.
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
  int uiSize = f.available() ? f.readStringUntil('\n').toInt() : 0;
  uiFontSizeIndex = constrain(uiSize, 0, 2);
  applyUiTextSize();

  f.close();

  if (line1.length() > 0) {
    int themeIdx = line1.toInt();
    if (themeIdx >= 0 && themeIdx <= 3) {
      applyEditorTheme(themeIdx);
    }
  }

  if (line2.length() > 0) {
    int fontIdx = line2.toInt();
    if (fontIdx >= 0 && fontIdx <= 3) {
      editorFontSizeIndex = fontIdx;
      applyEditorFontSize();
    }
  }
}

void saveAppearanceToSD() {
  String config = String(editorThemeIndex) + "\n" + String(editorFontSizeIndex) + "\n" + String(uiFontSizeIndex) + "\n";
  if (!writeFileSafely(APPEARANCE_CFG_PATH, (const uint8_t *)config.c_str(), config.length()))
    showStatusMessage("Appearance save failed", 3000);
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

// ---------- Network receipt printer ----------
static const char *PRINTER_CFG_PATH="/journal/config/printer.cfg";
static int printerSetupField=0;
static bool printerSetupEditing=false;
static String printerSetupValue;

void loadPrinterConfig() {
  File f=SD.open(PRINTER_CFG_PATH,FILE_READ);
  if(!f)return;
  JsonDocument doc;DeserializationError error=deserializeJson(doc,f);f.close();if(error)return;
  String host=doc["host"] | "";host.trim();
  int port=doc["port"] | 9100;int cols=doc["columns"] | 32;
  if(host.length()<128)JournalPrinter::config.host=host;
  if(port>0&&port<65536)JournalPrinter::config.port=port;
  if(cols>=16&&cols<=80)JournalPrinter::config.columns=cols;
  JournalPrinter::config.live=doc["printOnEnter"] | false;
  JournalPrinter::config.cut=doc["cutAfterReceipt"] | true;
}

bool savePrinterConfig() {
  JsonDocument doc;doc["host"]=JournalPrinter::config.host;doc["port"]=JournalPrinter::config.port;
  doc["columns"]=JournalPrinter::config.columns;doc["printOnEnter"]=JournalPrinter::config.live;
  doc["cutAfterReceipt"]=JournalPrinter::config.cut;
  String data;serializeJson(doc,data);
  bool ok=writeFileSafely(PRINTER_CFG_PATH,(const uint8_t*)data.c_str(),data.length());
  if(!ok)showStatusMessage("Printer settings save failed",4000);return ok;
}

void openPrinterSetup() {
  currentMenu=MENU_SETTINGS_PRINTER;printerSetupField=0;printerSetupEditing=false;needsRedraw=true;
}

void handlePrinterSetupKeys(const Keyboard_Class::KeysState &ks) {
  if(ks.del){
    if(printerSetupEditing){if(printerSetupValue.length())printerSetupValue.remove(printerSetupValue.length()-1);}
    else openMenu(MENU_SETTINGS_SYNC);
  }else if(ks.enter){
    if(printerSetupEditing){
      bool valid=true;
      if(printerSetupField==0){String host=printerSetupValue;host.trim();if(host.length()>127||host.indexOf(' ')>=0)valid=false;else JournalPrinter::config.host=host;}
      else {
        bool digits=printerSetupValue.length()>0;
        for(size_t i=0;i<printerSetupValue.length();++i)if(!isdigit((unsigned char)printerSetupValue[i]))digits=false;
        long number=printerSetupValue.toInt();
        if(printerSetupField==1){if(!digits||number<1||number>65535)valid=false;else JournalPrinter::config.port=number;}
        else {if(!digits||number<16||number>80)valid=false;else JournalPrinter::config.columns=number;}
      }
      if(valid){if(savePrinterConfig())printerSetupEditing=false;}else showStatusMessage("Invalid printer setting",3000);
    }else if(printerSetupField<3){
      printerSetupEditing=true;
      printerSetupValue=printerSetupField==0?JournalPrinter::config.host:printerSetupField==1?String(JournalPrinter::config.port):String(JournalPrinter::config.columns);
    }else if(printerSetupField==3){JournalPrinter::config.live=!JournalPrinter::config.live;savePrinterConfig();}
    else if(printerSetupField==4){JournalPrinter::config.cut=!JournalPrinter::config.cut;savePrinterConfig();}
    else if(printerSetupField==5){
      if(!JournalPrinter::enqueue("Tiny Journal\nPrinter test: ABC xyz 123\nUK pound: \xC2\xA3",wifiSsid,wifiPassword))showStatusMessage("Printer busy / not configured",4000);
      else showStatusMessage("Sending printer test...",3000);
    }else openMenu(MENU_SETTINGS_SYNC);
  }else for(auto c:ks.word){
    if(c==27||c=='`'){printerSetupEditing=false;openMenu(MENU_SETTINGS_SYNC);break;}
    if(printerSetupEditing){if(c>=32&&c<127&&printerSetupValue.length()<127)printerSetupValue+=(char)c;}
    else if(c==';'&&printerSetupField>0)--printerSetupField;
    else if(c=='.'&&printerSetupField<6)++printerSetupField;
  }
  needsRedraw=true;
}

void drawPrinterSetup() {
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(3, 0);
  M5Cardputer.Display.print(fitUiLabel("Network printer", M5Cardputer.Display.width() - 6));
  String rows[] = {"Host: " + JournalPrinter::config.host, "Port: " + String(JournalPrinter::config.port),
    "Columns: " + String(JournalPrinter::config.columns), String("Print on Enter: ") + (JournalPrinter::config.live ? "ON" : "OFF"),
    String("Cut after receipt: ") + (JournalPrinter::config.cut ? "ON" : "OFF"), "Send test receipt", "Back"};
  int first = max(0, printerSetupField - listVisibleLines + 1);
  for (int row = 0; row < listVisibleLines && first + row < 7; ++row) {
    int i = first + row, y = topBarHeight + row * lineHeight;
    bool selected = i == printerSetupField;
    M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), lineHeight, selected ? editorFgColor : editorBgColor);
    M5Cardputer.Display.setTextColor(selected ? editorBgColor : editorFgColor, selected ? editorFgColor : editorBgColor);
    String label = printerSetupEditing && selected ? "> " + printerSetupValue : rows[i];
    drawUiListLabel(label, 3, y, uiListLabelWidth(7, listVisibleLines), selected);
  }
  drawUiScrollbar(7, first, listVisibleLines, topBarHeight, listVisibleLines * lineHeight);
  drawUiFooter(printerSetupEditing ? "Enter: save  Esc: back" : "Arrows: choose  Enter: edit");
}

void printCurrentDocument() {
  if(!saveDirtyDocument())return;
  if(!JournalPrinter::config.host.length()){openPrinterSetup();showStatusMessage("Set printer host first",3000);return;}
  if(!JournalPrinter::enqueue(textBuffer,wifiSsid,wifiPassword))showStatusMessage("Printer busy or job exceeds 32 KiB",5000);
  else showStatusMessage("Sending note to printer...",3000);
}

void printCompletedLine() {
  if(!JournalPrinter::config.live)return;
  size_t end=cursorIndex;
  if(end>0&&textBuffer[end-1]=='\n')--end;
  size_t start=end;while(start>0&&textBuffer[start-1]!='\n')--start;
  if(!JournalPrinter::enqueue(textBuffer.substring(start,end),wifiSsid,wifiPassword,false))
    showStatusMessage("Line not printed: printer busy/setup",4000);
}

void pollPrinter() {
  if(JournalPrinter::releaseWiFiDue(millis()))disconnectWiFi();
  int result=JournalPrinter::outcome.exchange(0);
  if(!result)return;
  wifiConnected=WiFi.status()==WL_CONNECTED;
  if(result==JournalPrinter::SENT)showStatusMessage("Job sent; check printer output",3500);
  else if(result==JournalPrinter::PARTIAL)showStatusMessage("Print interrupted; check before retry",5000);
  else if(result==JournalPrinter::WIFI_SETUP)showStatusMessage("Printer: set up WiFi first",5000);
  else if(result==JournalPrinter::WIFI_FAILED)showStatusMessage("Printer: WiFi connection failed",5000);
  else showStatusMessage("Printer: cannot reach host/port",5000);
  needsRedraw=true;
}

// Establish wall-clock time before any certificate validation. Failure is closed.
bool ensureSecureNetworkTime() {
  if (time(nullptr) >= 1735689600) return true; // 2025-01-01
  showStatusMessage("Checking secure connection time...", 3000);
  configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
  unsigned long started = millis();
  while (time(nullptr) < 1735689600 && millis() - started < 15000) delay(50);
  if (time(nullptr) < 1735689600) {
    showStatusMessage("Sync stopped: set time / check network", 5000); return false;
  }
  return true;
}

static void configureGoogleTLS(WiFiClientSecure &client) {
  client.setCACert(GOOGLE_TRUST_ROOTS);
  client.setHandshakeTimeout(15);
  client.setTimeout(15000);
}

bool getGoogleAccessToken(String &accessToken) {
  accessToken = "";
  if (!loadGDriveConfigFromSD()) { showStatusMessage("Google setup missing", 3000); return false; }
  if (!ensureSecureNetworkTime()) return false;
  WiFiClientSecure client; configureGoogleTLS(client);
  HTTPClient https; https.setTimeout(15000); https.setConnectTimeout(15000);
  if (!https.begin(client, "https://oauth2.googleapis.com/token")) return false;
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "client_id=" + JournalNet::urlEncode(gdriveClientId)
    + "&client_secret=" + JournalNet::urlEncode(gdriveClientSecret)
    + "&refresh_token=" + JournalNet::urlEncode(gdriveRefreshToken) + "&grant_type=refresh_token";
  int code = https.POST(body);
  if (code != 200) { https.end(); showStatusMessage(code<0?"Google TLS/network verification failed":"Google sign-in failed: " + String(code),5000); return false; }
  JsonDocument response;
  DeserializationError error = deserializeJson(response, https.getString()); https.end();
  if (error || !response["access_token"].is<const char*>()) { showStatusMessage("Invalid Google response",4000); return false; }
  accessToken = response["access_token"].as<String>();
  return accessToken.length() > 0 && accessToken.length() < 8192;
}

String jsonEscape(const String &value) {
  String escaped; JsonDocument doc; doc.set(value); serializeJson(doc,escaped);
  return escaped.substring(1,escaped.length()-1);
}

// Reserve an ID BEFORE creating a file, then persist it. A response lost after
// upload can consequently be retried against the same ID instead of duplicating.
static bool reserveDriveId(const String &token, String &id) {
  WiFiClientSecure client; configureGoogleTLS(client); HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client,"https://www.googleapis.com/drive/v3/files/generateIds?count=1&space=drive&type=files")) return false;
  http.addHeader("Authorization","Bearer "+token); int code=http.GET();
  JsonDocument doc; bool ok=code==200 && !deserializeJson(doc,http.getString()); http.end();
  if(ok && doc["ids"][0].is<const char*>()) id=doc["ids"][0].as<String>();
  else ok=false;
  return ok && JournalNet::validId(id);
}

bool uploadFileToDriveMultipart(const String &accessToken, const String &path, const String &mimeType, bool showResultMessage) {
  if (!ensureSecureNetworkTime()) return false;
  File file=SD.open(path,FILE_READ); if(!file || file.isDirectory())return false;
  String hash=JournalNet::fileHash(file); if(!hash.length()){file.close();return false;}
  if(!SD.exists("/journal/config/drive"))SD.mkdir("/journal/config/drive");
  String key=JournalNet::sha256(gdriveFolderId+"\n"+path);
  if(!key.length()){file.close();return false;}
  String manifestPath="/journal/config/drive/"+key+".json";
  JsonDocument manifest;
  File mf=SD.open(manifestPath,FILE_READ);
  if(mf && mf.size()>4096){mf.close();file.close();showStatusMessage("Drive state too large",4000);return false;}
  if(mf){DeserializationError err=deserializeJson(manifest,mf);mf.close();if(err){file.close();showStatusMessage("Drive state damaged; restore backup",5000);return false;}}
  String id=manifest["id"] | "";
  bool created=manifest["created"] | false;
  if(id.length()&&!JournalNet::validId(id)){file.close();return false;}
  if(created && manifest["sha256"].as<String>()==hash){file.close();return true;}
  if(!id.length()){
    if(!reserveDriveId(accessToken,id)){file.close();showStatusMessage("Cannot reserve Google file",4000);return false;}
    manifest["id"]=id;manifest["path"]=path;manifest["folder"]=gdriveFolderId;manifest["created"]=false;
    String state;serializeJson(manifest,state);
    if(!writeFileSafely(manifestPath,(const uint8_t*)state.c_str(),state.length())){file.close();return false;}
  }
  // Preserve category identity in metadata, even with equal basenames in folders.
  String filename=path.substring(path.lastIndexOf('/')+1);
  JsonDocument metadata;metadata["name"]=filename;
  metadata["appProperties"]["tinyJournalPath"]=path;
  if(!created){metadata["id"]=id;if(gdriveFolderId.length())metadata["parents"].add(gdriveFolderId);}
  String meta;serializeJson(metadata,meta);
  const String boundary="tj_"+key.substring(0,24);
  String prefix="--"+boundary+"\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"+meta
    +"\r\n--"+boundary+"\r\nContent-Type: "+mimeType+"\r\n\r\n";
  String suffix="\r\n--"+boundary+"--\r\n";
  int code=0;
  for(int attempt=0;attempt<2;++attempt){
    file.seek(0);
    WiFiClientSecure client;configureGoogleTLS(client);HTTPClient http;http.setTimeout(15000);
    String url="https://www.googleapis.com/upload/drive/v3/files"+(created?"/"+id:String(""))+"?uploadType=multipart&fields=id";
    if(!http.begin(client,url)){file.close();return false;}
    http.addHeader("Authorization","Bearer "+accessToken);
    http.addHeader("Content-Type","multipart/related; boundary="+boundary);
    JournalNet::MultipartStream stream(prefix,file,suffix);
    code=http.sendRequest(created?"PATCH":"POST",&stream,prefix.length()+file.size()+suffix.length());
    bool complete=stream.complete();http.end();
    if(code==409&&!created){
      // The reserved ID already exists after an interrupted previous request.
      created=true;metadata.remove("id");metadata.remove("parents");meta="";serializeJson(metadata,meta);
      prefix="--"+boundary+"\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"+meta+"\r\n--"+boundary+"\r\nContent-Type: "+mimeType+"\r\n\r\n";
      continue;
    }
    if(code>=200&&code<300&&complete){
      file.close();manifest["created"]=true;manifest["sha256"]=hash;
      String state;serializeJson(manifest,state);
      if(!writeFileSafely(manifestPath,(const uint8_t*)state.c_str(),state.length())){showStatusMessage("Uploaded; Drive state save failed",5000);return false;}
      if(showResultMessage)showStatusMessage("Synced "+filename,1500);return true;
    }
    break;
  }
  file.close();
  // Never forget an existing remote ID automatically (404 may be permissions).
  showStatusMessage(code<0?"Upload TLS/network verification failed":"Upload failed: "+String(code),5000);
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
      int basenameAt = name.lastIndexOf('/');
      if (basenameAt >= 0) name = name.substring(basenameAt + 1);
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
      int basenameAt = name.lastIndexOf('/');
      if (basenameAt >= 0) name = name.substring(basenameAt + 1);
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
  if (!saveDirtyDocument()) return false;
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
  syncAudioInDir("/journal/audio/export", accessToken, allOk, audioCount);

  // Restore editor state path
  currentFilePath = originalPath;

  if (docCount == 0 && audioCount == 0) {
    showStatusMessage("No files to sync", 2000);
    return true;  // nothing to do, but not an error
  }

  return allOk;
}


void runManualSync() {
  if (JournalPrinter::busy()) { showStatusMessage("Wait for print job",2500); return; }
  if (!saveDirtyDocument()) return;
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
  if (JournalPrinter::busy()) { showStatusMessage("Wait for print job",2500); return; }
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
  bool saved = saveRTCToSD();

  disconnectWiFi();
  showStatusMessage(saved ? "Time synced, WiFi off" : "Time synced; SD save failed", 4000);
}

#include "JournalBluetooth.inl"

// ---------- WiFi setup (scan + connect) ----------

void startWiFiScan() {
  if (JournalPrinter::busy()) { showStatusMessage("Wait for print job",2500); return; }
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
        bool saved = saveWiFiConfig(wifiSsid, wifiPassword);
        disconnectWiFi();
        if (saved) {
          showStatusMessage("WiFi config saved", 3000);
          openMenu(MENU_SETTINGS_SYNC);
        }
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

static bool writeWavHeader(File &file, uint32_t sampleRate, uint32_t totalSamples) {
  uint8_t header[44] = {};
  memcpy(header, "RIFF", 4); memcpy(header + 8, "WAVEfmt ", 8); memcpy(header + 36, "data", 4);
  auto le32 = [&](size_t offset, uint32_t value) { for (int i = 0; i < 4; ++i) header[offset + i] = value >> (8 * i); };
  auto le16 = [&](size_t offset, uint16_t value) { header[offset] = value; header[offset + 1] = value >> 8; };
  le32(4, 36 + totalSamples * 2); le32(16, 16); le16(20, 1); le16(22, 1);
  le32(24, sampleRate); le32(28, sampleRate * 2); le16(32, 2); le16(34, 16); le32(40, totalSamples * 2);
  uint32_t position = file.position();
  if (!file.seek(0) || file.write(header, sizeof(header)) != sizeof(header)) return false;
  file.flush();
  return !file.getWriteError() && file.seek(position < 44 ? 44 : position);
}

void recoverAudioNotes() {
  File dir = SD.open("/journal/audio"); if (!dir) return;
  while (true) {
    File entry = dir.openNextFile(); if (!entry) break;
    String name = entry.name(); bool isDir = entry.isDirectory(); entry.close();
    int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
    if (isDir || !name.endsWith(".wav.part")) continue;
    String part = String("/journal/audio/") + name;
    File f = SD.open(part, "r+");
    uint8_t header[44];
    if (!f || f.size() < 44 || f.read(header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) || memcmp(header + 36, "data", 4) ||
        header[20] != 1 || header[22] != 1 || header[34] != 16) { if (f) f.close(); continue; }
    uint32_t rate = (uint32_t)header[24] | ((uint32_t)header[25] << 8) | ((uint32_t)header[26] << 16) | ((uint32_t)header[27] << 24);
    uint32_t samples = (f.size() - 44) / 2;
    bool repaired = rate >= 8000 && rate <= 96000 && writeWavHeader(f, rate, samples);
    f.close();
    String recovered = part.substring(0, part.length() - 5);
    if (SD.exists(recovered)) recovered = unusedFilePath(recovered.substring(0, recovered.length() - 4) + "-recovered", ".wav");
    if (repaired && recovered.length()) SD.rename(part, recovered);
  }
  dir.close();
}


void startAudioNote() {
  if (!saveDirtyDocument()) return;
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) { showStatusMessage("Audio: SD unavailable", 3000); return; }
  SD.mkdir("/journal/audio");
  String path = unusedFilePath(String("/journal/audio/") + getCurrentDateString() + "_" + getCurrentTimeString(), ".wav");
  if (!path.length()) return;
  String part = path + ".part";
  File audioFile = SD.open(part, FILE_WRITE);
  if (!audioFile) { showStatusMessage("Audio: cannot create file", 3000); return; }
  const uint32_t sampleRate = 17000;
  const size_t chunkSamples = 256;
  if (!writeWavHeader(audioFile, sampleRate, 0)) { audioFile.close(); showStatusMessage("Audio: header write failed", 3000); return; }
  int16_t sampleBuf[chunkSamples];
  uint32_t samplesWritten = 0;
  unsigned long startedMs = millis(), checkpointMs = startedMs, screenMs = 0;
  int warmup = 8;
  bool failed = false, stopped = false;
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(0, 0); M5Cardputer.Display.println("Recording audio");
  M5Cardputer.Display.println(fitUiLabel(path.substring(path.lastIndexOf('/') + 1), M5Cardputer.Display.width()));
  M5Cardputer.Display.println("Any key stops");
  M5Cardputer.Speaker.end(); delay(50);
  if (!M5Cardputer.Mic.begin()) failed = true;
  while (!failed && !stopped) {
    M5Cardputer.update(); updateRTC(); updateBatteryStatus();
    if (millis() - startedMs > 500 && anyAppKeyPressed()) break;
    if (batteryLevelPercent >= 0 && batteryLevelPercent <= CRITICAL_BATTERY_LEVEL) break;
    if (!M5Cardputer.Mic.record(sampleBuf, chunkSamples, sampleRate)) { failed = true; break; }
    // record() queues an asynchronous fill. Never write or reuse its buffer
    // before completion. end() below cancels outstanding DMA before stack exit.
    unsigned long captureStart = millis();
    while (M5Cardputer.Mic.isRecording()) {
      M5Cardputer.update(); updateRTC();
      if (millis() - startedMs > 500 && anyAppKeyPressed()) { stopped = true; break; }
      if (millis() - captureStart > 2000) { failed = true; break; }
      delay(1);
    }
    if (failed || stopped) break;
    if (warmup) { --warmup; continue; }
    if (samplesWritten > (UINT32_MAX - 44) / 2 - chunkSamples ||
        audioFile.write((const uint8_t *)sampleBuf, sizeof(sampleBuf)) != sizeof(sampleBuf)) { failed = true; break; }
    samplesWritten += chunkSamples;
    if (millis() - checkpointMs >= 1000) {
      if (!writeWavHeader(audioFile, sampleRate, samplesWritten)) { failed = true; break; }
      checkpointMs = millis();
    }
    if (millis() - screenMs >= 500) {
      int y = lineHeight * 4;
      M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), lineHeight, editorBgColor);
      M5Cardputer.Display.setCursor(0, y);
      M5Cardputer.Display.print(String("Time: ") + String((millis() - startedMs) / 1000) + "s");
      screenMs = millis();
    }
    delay(1);
  }
  M5Cardputer.Mic.end(); delay(50); M5Cardputer.Speaker.begin();
  // Use actual length so a short final write is recoverable too.
  uint32_t actualSamples = audioFile.size() >= 44 ? (audioFile.size() - 44) / 2 : 0;
  bool headerOk = writeWavHeader(audioFile, sampleRate, actualSamples);
  audioFile.close();
  bool committed = headerOk && SD.rename(part, path);
  if (failed || !committed) showStatusMessage("Audio interrupted; recoverable recording kept", 5000);
  else showStatusMessage("Saved audio: " + path.substring(path.lastIndexOf('/') + 1), 3000);
  needsRedraw = true;
}
void drawAudioNotesList(AudioNoteEntry *entries, int count, int selectedIndex, int scrollOffset) {
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(3, 0);
  M5Cardputer.Display.print("Audio notes");
  for (int row = 0; row < listVisibleLines; ++row) {
    int idx = scrollOffset + row;
    if (idx >= count) break;
    int y = topBarHeight + row * lineHeight;
    bool selected = idx == selectedIndex;
    M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), lineHeight, selected ? editorFgColor : editorBgColor);
    M5Cardputer.Display.setTextColor(selected ? editorBgColor : editorFgColor, selected ? editorFgColor : editorBgColor);
    drawUiListLabel(entries[idx].name, 3, y, uiListLabelWidth(count, listVisibleLines), selected);
  }
  drawUiScrollbar(count, scrollOffset, listVisibleLines, topBarHeight, listVisibleLines * lineHeight);
  drawUiFooter("Up/Down d:del D:listed Esc:back");
}

// A different, fresh key is required: the invoking D/Enter and its repeat
// cannot confirm a destructive operation. Clear queued presses until release.
static bool confirmBulkDelete(const String &description) {
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setFont(&fonts::efontJA_12);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(2, 4);
  M5Cardputer.Display.print(description);
  M5Cardputer.Display.setCursor(2, 22);
  M5Cardputer.Display.print("This cannot be undone.");
  drawUiFooter("Y: delete   N/Esc: cancel");
  do {
    M5Cardputer.update();
    btKeyboardPollConnection();
    btProcessHidReports();
    btClearInputEventQueue();
    delay(5);
  } while (M5Cardputer.Keyboard.isPressed() || btAnyKeyDown);
  for (;;) {
    M5Cardputer.update();
    updateRTC();
    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {
      for (char c : ks.word) {
        if (c == 'y' || c == 'Y') return true;
        if (c == 'n' || c == 'N' || c == '`' || c == 27) return false;
      }
    }
    delay(5);
  }
}

void startAudioNoteBrowser() {
  applyUiTextSize();
  std::unique_ptr<AudioNoteEntry[]> ownedEntries(new (std::nothrow) AudioNoteEntry[MAX_AUDIO_NOTES]);
  if (!ownedEntries) { showStatusMessage("Not enough memory for audio browser", 3000); return; }
  AudioNoteEntry *entries = ownedEntries.get();
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
  int rows = listVisibleLines;
  if (rows < 1) rows = 1;

  drawAudioNotesList(entries, count, selectedIndex, scrollOffset);

  bool browsing = true;
  while (browsing) {
    pollUiAnimation();
    if (needsRedraw) { drawAudioNotesList(entries, count, selectedIndex, scrollOffset); needsRedraw = false; }
    M5Cardputer.update();
    updateRTC();

    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {

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
            if (!SD.remove(delPath.c_str())) {
              showStatusMessage("Audio delete failed; file kept", 2500);
              drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
              continue;
            }

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

        // Delete all only after explicit confirmation. Failed removals remain
        // in the browser instead of being reported as successful deletions.
        if (c == 'D') {
          if (!confirmBulkDelete("Delete listed audio notes?")) {
            drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
            continue;
          }
          int remaining = 0;
          for (int i = 0; i < count; ++i) {
            if (!SD.remove(entries[i].path.c_str())) entries[remaining++] = entries[i];
          }
          count = remaining;
          selectedIndex = scrollOffset = 0;
          showStatusMessage(count ? "Some audio files could not be deleted" : "Listed audio notes deleted", 3000);
          browsing = count != 0;
          if (browsing) drawAudioNotesList(entries, count, selectedIndex, scrollOffset);
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
      int basenameAt = name.lastIndexOf('/');
      if (basenameAt >= 0) name = name.substring(basenameAt + 1);
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
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(3, 0);
  M5Cardputer.Display.print("Open document");
  for (int row = 0; row < listVisibleLines; ++row) {
    int idx = scrollOffset + row;
    if (idx >= count) break;
    int y = topBarHeight + row * lineHeight;
    bool selected = idx == selectedIndex;
    M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), lineHeight, selected ? editorFgColor : editorBgColor);
    M5Cardputer.Display.setTextColor(selected ? editorBgColor : editorFgColor, selected ? editorFgColor : editorBgColor);
    drawUiListLabel(entries[idx].displayName, 3, y, uiListLabelWidth(count, listVisibleLines), selected);
  }
  drawUiScrollbar(count, scrollOffset, listVisibleLines, topBarHeight, listVisibleLines * lineHeight);
  drawUiFooter("Enter:open d:rename Esc:back");
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

    drawUiFooter("Enter: save Del: back Esc: cancel");

    M5Cardputer.update();
    updateRTC();

    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {

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

  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0 || newName.indexOf(':') >= 0 || newName == "." || newName == "..") {
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

  if (SD.exists(newFullPath.c_str()) || SD.exists(newFullPath + ".bak") || SD.exists(newFullPath + ".tmp")) {
    showStatusMessage("File already exists", 2000);
    return false;
  }

  bool hadBackup = SD.exists(fullPath + ".bak");
  if (hadBackup && !SD.rename(fullPath + ".bak", newFullPath + ".bak")) {
    showStatusMessage("Rename failed; note kept", 2000); return false;
  }
  if (!SD.rename(fullPath.c_str(), newFullPath.c_str())) {
    if (hadBackup) SD.rename(newFullPath + ".bak", fullPath + ".bak");
    showStatusMessage("Rename failed", 2000);
    return false;
  }

  // Keep the previous revision associated with the renamed note.
  if (SD.exists(fullPath + ".tmp")) SD.remove(fullPath + ".tmp");
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
  applyUiTextSize();
  if (!saveDirtyDocument()) return;
  std::unique_ptr<DocumentEntry[]> ownedEntries(new (std::nothrow) DocumentEntry[MAX_DOCUMENTS]);
  if (!ownedEntries) { showStatusMessage("Not enough memory for document browser", 3000); return; }
  DocumentEntry *entries = ownedEntries.get();
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
  int rows = listVisibleLines;
  if (rows < 1) rows = 1;

  drawDocumentList(entries, count, selectedIndex, scrollOffset);

  bool browsing = true;
  while (browsing) {
    pollUiAnimation();
    if (needsRedraw) { drawDocumentList(entries, count, selectedIndex, scrollOffset); needsRedraw = false; }
    M5Cardputer.update();
    updateRTC();

    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {

      // Open on ENTER
      if (ks.enter && count > 0) {
        if (!saveDirtyDocument()) continue;
        String previousPath = currentFilePath;
        currentFilePath = entries[selectedIndex].path;
        if (!loadFromFile()) { currentFilePath = previousPath; continue; }
        saveLastEditedFilePath();
        appMode = MODE_EDITOR;
        currentMenu = MENU_NONE;
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
// If excludeSystem is true, config/readme files are NOT deleted.
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
      int basenameAt = name.lastIndexOf('/');
      if (basenameAt >= 0) name = name.substring(basenameAt + 1);
      String lower = name;
      lower.toLowerCase();

      bool isTxt = lower.endsWith(".txt");
      bool isRtc = (lower == "rtc.txt");
      bool isCfg = (lower == "config-readme.txt");
      bool isGuide = String(dirPath) == "/journal" && (lower == "readme.txt" || lower == "tiny-journal-readme.txt");

      if (isTxt && !isGuide && !(excludeSystem && (isRtc || isCfg))) {
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";

        int slashPos = name.lastIndexOf('/');
        if (slashPos >= 0) {
          fullPath += name.substring(slashPos + 1);
        } else {
          fullPath += name;
        }

        // Delete retained revisions first so recovery never resurrects a
        // document the user explicitly deleted.
        bool revisionsRemoved = true;
        for (const char *suffix : {".bak", ".tmp"}) {
          String revision = fullPath + suffix;
          if (SD.exists(revision) && !SD.remove(revision)) revisionsRemoved = false;
        }
        if (revisionsRemoved && SD.remove(fullPath.c_str())) deleted++;
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
      int basenameAt = name.lastIndexOf('/');
      if (basenameAt >= 0) name = name.substring(basenameAt + 1);
      String lower = name;
      lower.toLowerCase();

      if (lower.endsWith(".wav") || lower.endsWith(".wav.part")) {
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
static bool deleteAllFilesRecursively(const char *dirPath, bool preserveSyncCfg) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  bool complete = true;
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    String name = String(f.name());
    if (name.length() == 0) {
      f.close();
      complete = false;
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
      if (!deleteAllFilesRecursively(subPath.c_str(), preserveSyncCfg)) complete = false;

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

      if (!SD.remove(fullPath.c_str())) complete = false;
    }
  }

  dir.close();
  return complete;
}


// Show a simple storage summary for /journal
void showJournalStorageSummary() {
  applyUiTextSize();
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

  drawUiFooter("Esc: back");

  // Wait for ` or ESC
  bool done = false;
  while (!done) {
    M5Cardputer.update();
    updateRTC();

    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {
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
  if (!confirmBulkDelete("Delete all text documents?")) { showStatusMessage("Delete cancelled", 2000); return; }
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
  if (!confirmBulkDelete("Delete all audio notes?")) { showStatusMessage("Delete cancelled", 2000); return; }
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

// A reset may already have removed some files when a later deletion fails.
// Keep the in-memory document available and dirty it if its file was removed,
// so retrying/saving remains possible instead of pretending reset succeeded.
static bool deleteJournalForFactoryReset(bool preserveSyncCfg) {
  if (deleteAllFilesRecursively("/journal", preserveSyncCfg)) return true;
  if (currentFilePath.length() && !SD.exists(currentFilePath)) {
    bufferDirty = true;
    savedAfterIdle = false;
  }
  showStatusMessage("Reset incomplete; some files remain", 5000);
  needsRedraw = true;
  return false;
}

static bool resetFileMatches(const char *path, const char *expected) {
  File file = SD.open(path, FILE_READ);
  size_t length = strlen(expected);
  if (!file || file.isDirectory() || file.size() != length) { if (file) file.close(); return false; }
  for (size_t i = 0; i < length; ++i) {
    if (file.read() != (uint8_t)expected[i]) { file.close(); return false; }
  }
  file.close();
  return true;
}

static bool resetConfigReadable(const char *path, size_t minimumLines) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory() || !file.size() || file.size() > MAX_DOCUMENT_BYTES) { if (file) file.close(); return false; }
  size_t size = file.size(), lines = 0;
  int last = -1;
  for (size_t i = 0; i < size; ++i) {
    int value = file.read();
    if (value < 0) { file.close(); return false; }
    if (value == '\n') ++lines;
    last = value;
  }
  file.close();
  if (last != '\n') ++lines;
  return lines >= minimumLines;
}

// Void provisioning helpers can fail independently. Check their postconditions
// before discarding the old editor buffer or claiming a successful reset.
static bool completeFactoryReset() {
  auto incomplete = [&]() {
    if (currentFilePath.length() && !SD.exists(currentFilePath)) {
      bufferDirty = true;
      savedAfterIdle = false;
    }
    showStatusMessage("Reset incomplete; setup/save failed", 5000);
    needsRedraw = true;
    return false;
  };
  const char *directories[] = {"/journal", "/journal/config", "/journal/templates", "/journal/daily", "/journal/notes",
    "/journal/meetings", "/journal/projects", "/journal/travel", "/journal/misc", "/journal/archive", "/journal/audio", "/journal/audio/export"};
  for (const char *path : directories) {
    File directory = SD.open(path);
    bool ok = directory && directory.isDirectory();
    if (directory) directory.close();
    if (!ok) return incomplete();
  }
  if (!resetConfigReadable(WIFI_CFG_PATH, 2) || !resetConfigReadable(GDRIVE_CFG_PATH, 4) ||
      !resetConfigReadable(CONFIG_README_PATH, 1)) return incomplete();
  const char *templatePaths[] = {"/journal/templates/daily.tpl", "/journal/templates/quick.tpl", "/journal/templates/meeting.tpl",
    "/journal/templates/project.tpl", "/journal/templates/travel.tpl"};
  const char *templateContents[] = {TEMPLATE_DAILY, TEMPLATE_QUICK, TEMPLATE_MEETING, TEMPLATE_PROJECT, TEMPLATE_TRAVEL};
  for (size_t i = 0; i < 5; ++i) if (!resetFileMatches(templatePaths[i], templateContents[i])) return incomplete();

  // Prerequisites succeeded: intentionally discard the old document only now.
  bufferDirty = false;
  createNewBlankNote();
  if (!bufferDirty || textBuffer.length() != 0) return incomplete();
  if (!saveToFile()) return incomplete(); // checked temp-file save and readback
  String expectedLastFile = currentFilePath + "\n";
  if (!resetFileMatches(LAST_FILE_PATH, expectedLastFile.c_str())) {
    bufferDirty = true;
    savedAfterIdle = false;
    return incomplete();
  }
  return true;
}

void runFactoryReset() {
  applyUiTextSize();
  // Warning screen
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int y = 0;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("FACTORY RESET");

  y += lineHeight * 2;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("This will erase journal files");
  y += lineHeight;
  M5Cardputer.Display.setCursor(0, y);
  M5Cardputer.Display.println("under /journal, including");
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

    Keyboard_Class::KeysState ks;
    if (readAppKeys(ks)) {
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

      Keyboard_Class::KeysState ks2;
      if (readAppKeys(ks2)) {
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

  // Keep unrelated files on the SD card intact.
  if (!deleteJournalForFactoryReset(keepSyncCfg)) return;


  // Rebuild journal structure and default config/template files
  ensureJournalDirs();       // recreates /journal tree + /journal/config files/README
  ensureUserGuide();
  ensureDefaultTemplates();  // recreates template .tpl files
  loadTemplates();           // refresh in-memory template list

  // Reset editor state to a clean default: create a standard blank note in /journal/misc
  if (!completeFactoryReset()) return;
  lastKeyPressTime = millis();

  showStatusMessage("Factory reset complete", 2000);
  needsRedraw = true;
}


// ---------- USB mass storage mode ----------

static int32_t usbMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!usbMediaActive || bufsize == 0 || offset >= USB_MSC_BLOCK_SIZE || lba >= usbMscBlockCount ||
      (uint64_t)lba * USB_MSC_BLOCK_SIZE + offset + bufsize > (uint64_t)usbMscBlockCount * USB_MSC_BLOCK_SIZE) {
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
  if (!usbMediaActive || bufsize == 0 || offset >= USB_MSC_BLOCK_SIZE || lba >= usbMscBlockCount ||
      (uint64_t)lba * USB_MSC_BLOCK_SIZE + offset + bufsize > (uint64_t)usbMscBlockCount * USB_MSC_BLOCK_SIZE) {
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
  if (load_eject && !start) {
    usbMediaActive = false;
    if (usbMsc) usbMsc->mediaPresent(false);
    usbHostEjected = true;
  }
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

  // usbMscInterface already registered its descriptor before Arduino started
  // TinyUSB. Do not allocate a replacement here: late interfaces are rejected.
  if (usbMsc == nullptr) return;

  usbMsc->vendorID("TinyJ");
  usbMsc->productID("Journal SD");
  usbMsc->productRevision(TJ_USB_REVISION);
  usbMsc->onRead(usbMscRead);
  usbMsc->onWrite(usbMscWrite);
  usbMsc->onStartStop(usbMscStartStop);
  usbHostEjected = false;
  usbMediaActive = false;
  usbMsc->mediaPresent(false);

  // Publish media last. USB is already running with CDC-on-boot, so the host
  // may issue a capacity/read request immediately after mediaPresent(true).
  usbMscConfigured = usbMsc->begin(usbMscBlockCount, USB_MSC_BLOCK_SIZE);
  if (usbMscConfigured) {
    usbMediaActive = presentOnStart;
    usbMsc->mediaPresent(presentOnStart);
    USB.begin(); // Idempotent; also supports builds without CDC-on-boot.
  }
#endif
}

void drawUsbStorageModeScreen() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setFont(&fonts::efontJA_12);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);

  int y = 14;
  const int lineStep = 24;

  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("USB storage");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("Eject on PC");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("before leaving");
  y += lineStep;
  M5Cardputer.Display.setCursor(8, y);
  M5Cardputer.Display.println("Any key exits");

  M5Cardputer.Display.setTextSize(1);
}

void enterUsbStorageMode() {
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE == 1)
  showStatusMessage("Set USB Mode: USB-OTG", 3000);
  needsRedraw = true;
  return;
#else
  if (!saveDirtyDocument()) return;
  Preferences usbPrefs;
  if (!usbPrefs.begin("tinyjournal", false)) { showStatusMessage("USB mode flag failed", 3000); return; }
  bool stored = usbPrefs.putBool("usb", true) != 0;
  usbPrefs.end();
  if (!stored) { showStatusMessage("USB mode flag failed", 3000); return; }

  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setFont(&fonts::efontJA_12);
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
#if ARDUINO_USB_CDC_ON_BOOT
  // A serial-port probe/upload touch can otherwise reboot into the ROM loader
  // while the storage screen remains visible. Never allow CDC control lines
  // to interrupt host ownership of the SD card. Normal boot restores this.
  Serial.enableReboot(false);
#endif
  // Migrate the old FAT flag before host ownership begins.
  SD.remove(USB_STORAGE_FLAG_PATH);
  Preferences usbPrefs;
  usbPrefs.begin("tinyjournal", false);
  usbPrefs.putBool("usb", false);
  usbPrefs.end();
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
    bool leaveRequested = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
#ifdef TJ_DIAGNOSTICS
    // Test-only exit is equivalent to a physical key and still requires the
    // computer's successful SCSI eject. Normal firmware has no serial exit.
    while (Serial.available()) {
      int command = Serial.read();
      if (command == 'x' || command == 'X') leaveRequested = true;
    }
#endif
    if (leaveRequested) {
      if (usbHostEjected) break;
      M5Cardputer.Display.setCursor(0, 115);
      M5Cardputer.Display.setTextSize(1);
      M5Cardputer.Display.println("Eject disk from computer first");
    }
    delay(20);
  }

  usbMediaActive = false;
  usbMsc->mediaPresent(false);
  // No FAT operations after host raw writes. Reboot remounts a fresh filesystem.

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
  if (!saveDirtyDocument()) return;
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

  if (appMode == MODE_MENU && currentMenu == MENU_SETTINGS_PRINTER) {
    handlePrinterSetupKeys(ks);
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
  if(ks.ctrl && ks.space){cycleKeyboardLayout();return;}
  if(ks.ctrl){for(auto k:ks.hid_keys){if(k==0x1d){journalUndo();return;}if(k==0x1c){journalRedo();return;}if(k==9){journalFind();return;}}}
  if(!fnDown&&!optDown&&btKeyboardLayout>=BT_LAYOUT_DE){
    for(auto usage:ks.hid_keys){if(usage==0x28||usage==0x2a||usage==0x2b)continue;char out[16];if(builtInTextMapper.text(btKeyboardLayout,usage,ks.modifiers,out))insertText(out);}
    if(!ks.enter&&!ks.del&&!ks.tab){setLastAction(ACTION_NONE,0);return;}
  }

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

size_t utf8CharLenAt(size_t idx){return journal::utf8Length(textBuffer.c_str(),textBuffer.length(),idx);}

size_t utf8NextIndex(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx >= len) return len;
  size_t step = utf8CharLenAt(idx);
  if (step < 1) step = 1;
  idx += step;
  if (idx > len) idx = len;
  return idx;
}

size_t utf8PrevIndex(size_t idx){return journal::previous(textBuffer.c_str(),textBuffer.length(),idx);}

String utf8CharStringAt(size_t idx) {
  size_t len = (size_t)textBuffer.length();
  if (idx >= len) return String("");
  size_t charLen = utf8CharLenAt(idx);
  if (charLen < 1) charLen = 1;
  if (idx + charLen > len) charLen = len - idx;
  if(charLen==1&&uint8_t(textBuffer[idx])>=128)return String("\xef\xbf\xbd");
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
  if(textBuffer.length()+insertValue.length()>128*1024 || ESP.getFreeHeap()<textBuffer.length()*3+insertValue.length()+16384){showStatusMessage("Document memory limit",2500);return;}
  String candidate=textBuffer.substring(0,cursorIndex)+insertValue+textBuffer.substring(cursorIndex);
  if(candidate.length()!=textBuffer.length()+insertValue.length()||!documentFitsMemory(candidate)){showStatusMessage("Document memory limit",2500);return;}
  rememberEdit();
  textBuffer=std::move(candidate);
  cursorIndex += insertValue.length();
  bufferDirty = true;
  rebuildLayout();
  ensureCursorVisible();
  needsRedraw = true;
}

void insertNewline() {
  size_t before = cursorIndex;
  insertChar('\n');
  if (cursorIndex > before) printCompletedLine();
}

void insertTab() {
  insertChar('\t');
}

void backspaceChar() {
  cursorIndex = clampCursorIndex(cursorIndex);
  if (cursorIndex == 0) return;

  size_t prevIndex = utf8PrevIndex(cursorIndex);
  rememberEdit();
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
  rememberEdit();
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
    rememberEdit();
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
    rememberEdit();
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
      for (size_t idx = vl.start; idx < cursorIndex && idx < vl.end; idx=utf8NextIndex(idx)) {
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
  for (size_t idx = last.start; idx < last.end; idx=utf8NextIndex(idx)) {
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

size_t clampCursorIndex(size_t idx){size_t n=textBuffer.length();if(idx>n)return n;if(!idx||idx==n)return idx;for(size_t back=1;back<=3&&back<=idx;back++){size_t p=idx-back;if(p+utf8CharLenAt(p)>idx)return p;}return idx;}

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
    idx=utf8NextIndex(idx);
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

// Bluetooth key repeat is implemented in JournalBluetooth.inl.


// ---------- Menu helpers ----------

int getMenuItemCount(MenuId menu) {
  switch (menu) {
    case MENU_MAIN:
      return 5;
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
      return 3;
    case MENU_SETTINGS_SYNC:
      return 6;
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
        case 4: return "Read user guide";
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
        case 2: return String("Menu text: ") + (uiFontSizeIndex == 0 ? "standard" : uiFontSizeIndex == 1 ? "medium" : "large");
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
              case 3: sizeName = "medium (1.4x)"; break;
              case 1: sizeName = "large (2x)"; break;
              case 2: sizeName = "extra large"; break;
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
        case 4: return "Reset journal only";
      }
      break;


    case MENU_SETTINGS_SYNC:
      switch (index) {
        case 0: return "Sync now";
        case 1: return "WiFi setup";
        case 2: return "Sync time via WiFi";
        case 4: return "Print current note";
        case 5: return "Network printer setup";
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
    case MENU_SETTINGS_PRINTER: return MENU_SETTINGS_SYNC;
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
        case 4:
          showUserGuide();
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
        case 2:
          uiFontSizeIndex = (uiFontSizeIndex + 1) % 3;
          applyUiTextSize();
          applyEditorFontSize();
          rebuildLayout();
          saveAppearanceToSD();
          lastRenderedMenu = MENU_NONE;
          needsRedraw = true;
          break;
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
            editorFontSizeIndex = editorFontSizeIndex == 0 ? 3 : (editorFontSizeIndex == 3 ? 1 : (editorFontSizeIndex == 1 ? 2 : 0));

            // Recalculate editor layout and re-wrap text
            applyEditorFontSize();
            saveAppearanceToSD();
            rebuildLayout();
            ensureCursorVisible();
            needsRedraw = true;

            const char *sizeName;
            switch (editorFontSizeIndex) {
              case 0: sizeName = "standard"; break;
              case 3: sizeName = "medium (1.4x)"; break;
              case 1: sizeName = "large (2x)"; break;
              case 2: sizeName = "extra large"; break;
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
        case 4:
          printCurrentDocument();
          break;
        case 5:
          openPrinterSetup();
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
    if (saveRTCToSD()) {
      showStatusMessage("Time saved", 2000);
      openMenu(MENU_SETTINGS);
    }
    needsRedraw = true;
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
  if (ks.enter) { ++aboutPage; needsRedraw = true; return; }
  // Backspace / Del or Enter -> return to Settings menu
  if (ks.del || ks.enter) {
    openMenu(MENU_SETTINGS);
    needsRedraw = true;
    return;
  }
}

void drawDateTimeEditor() {
  applyUiTextSize();
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
  M5Cardputer.Display.print(fitUiLabel("Arrows: field/value", screenW - 6));

  const char *names[] = {"Year", "Month", "Day", "Hour", "Minute"};
  int values[] = {gDateTime.year, gDateTime.month, gDateTime.day, gDateTime.hour, gDateTime.minute};
  M5Cardputer.Display.setCursor(3, startY + lineHeight);
  M5Cardputer.Display.print(String(names[dtEditField]) + ": " + String(values[dtEditField]));
  drawUiFooter(statusMessageUntil > millis() && statusMessage.length() ? statusMessage : "Arrows: edit   Enter: save");

  M5Cardputer.Display.endWrite();
}

void drawAboutScreen() {
  applyUiTextSize();
  M5Cardputer.Display.fillScreen(editorBgColor);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  const char *items[] = {"Tiny Journal " TJ_VERSION_LABEL, "JoeJee90 / GitHub", "Esc: editor/menu", "Opt+Tab: record", "Ctrl+arrows: words", "Ctrl+Space: layout", "Ctrl+Z/Y: undo/redo", "Ctrl+F: find", "Enter: next page", "Backspace: return"};
  int rows = std::max<int>(1, M5Cardputer.Display.height() / lineHeight);
  int pages = (10 + rows - 1) / rows;
  aboutPage %= pages;
  for (int i = 0; i < rows && aboutPage * rows + i < 10; ++i) {
    M5Cardputer.Display.setCursor(2, i * lineHeight);
    M5Cardputer.Display.print(fitUiLabel(items[aboutPage * rows + i], M5Cardputer.Display.width() - 4));
  }
  lastRenderedAppMode = MODE_MENU;
  lastRenderedMenu = currentMenu;
}

// ---------- Status messages ----------

void showStatusMessage(const String &msg, unsigned long durationMs) {
  statusMessage = msg;
  statusMessageUntil = millis() + durationMs;
  needsRedraw = true;
}

// ---------- WiFi setup rendering ----------

void drawWiFiSetup() {
  applyUiTextSize();
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


        drawUiListLabel(label, 3, y, uiListLabelWidth(count, listVisibleLines), selected);
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
    M5Cardputer.Display.print(fitUiLabel("SSID: " + ssidLabel, screenW - 6));
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print("Enter password:");
    y += lineHeight;

    M5Cardputer.Display.fillRect(0, y, screenW, lineHeight, editorBgColor);
    M5Cardputer.Display.setCursor(3, y);
    M5Cardputer.Display.print(fitUiLabel(String(wifiPasswordInput.length()) + " password chars", screenW - 6));

  }

  if (wifiSetupState == WIFI_STATE_LIST)
    drawUiScrollbar(wifiNetworks.size(), wifiScrollOffset, listVisibleLines, startY, listVisibleLines * lineHeight);
  drawUiFooter(statusMessageUntil > millis() && statusMessage.length() ? statusMessage :
               wifiSetupState == WIFI_STATE_PWENTRY ? "Enter: connect   Esc: back" : "Enter: choose   Backspace: back");

  M5Cardputer.Display.endWrite();
}

// ---------- Rendering (editor + menus) ----------

void drawEditor() {
  const int lineHeight = baseLineHeight + 2;
  const int topBarHeight = lineHeight + 4;
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
  String savedStr = String(savedChar) + " " + journal::layoutName(btKeyboardLayout);
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
  if (editorFontSizeIndex == 3) {
    scale = 1.4f;
  } else if (editorFontSizeIndex == 0) {
    scale = 1;
  } else if (editorFontSizeIndex == 1) {
    scale = 2;
  } else {
    scale = CATARACTS_TEXT_SCALE;
  }

  int charWidth = baseCharWidth * scale;
  int editorLineH = baseLineHeight * scale;
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
    M5Cardputer.Display.print(fitUiLabel(statusMessage, M5Cardputer.Display.width() - 4));
  }

  M5Cardputer.Display.endWrite();
}

void drawMenu() {
  applyUiTextSize();
  uiMarqueeActive = false;
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
  if (currentMenu == MENU_SETTINGS_PRINTER) {
    drawPrinterSetup();
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


    drawUiListLabel(label, 3, y, uiListLabelWidth(count, listVisibleLines), selected);
  }

  drawUiScrollbar(count, menuScrollOffset, listVisibleLines, startY, listVisibleLines * lineHeight);
  drawUiFooter(statusMessageUntil > millis() && statusMessage.length() ? statusMessage : "Enter: select   Backspace: back");

  M5Cardputer.Display.endWrite();
}


void rememberEdit() {
  if (historyFilePath != currentFilePath) { editHistory.clear(); historyFilePath = currentFilePath; }
  editHistory.remember(textBuffer.c_str(), cursorIndex);
}

void restoreHistory(bool redo) {
  if (historyFilePath != currentFilePath) { editHistory.clear(); historyFilePath = currentFilePath; return; }
  JournalHistory::State state;
  bool ok = redo ? editHistory.redo(textBuffer.c_str(), cursorIndex, state) : editHistory.undo(textBuffer.c_str(), cursorIndex, state);
  if (!ok) { showStatusMessage(redo ? "Nothing to redo" : "Nothing to undo", 1500); return; }
  String restored;
  if (!restored.reserve(state.text.size() + 1) ||
      !restored.concat(state.text.data(), state.text.size()) ||
      !documentFitsMemory(restored)) {
    showStatusMessage("Undo memory limit; current note kept", 3000);
    return;
  }
  textBuffer = std::move(restored);
  cursorIndex = min(state.cursor, (size_t)textBuffer.length());
  bufferDirty = true; savedAfterIdle = false; lastKeyPressTime = millis();
  rebuildLayout(); ensureCursorVisible(); needsRedraw = true;
}
void journalUndo() { restoreHistory(false); }
void journalRedo() { restoreHistory(true); }

void journalFind() {
  static String query;
  bool active = true;
  while (active) {
    applyUiTextSize();
    M5Cardputer.Display.fillScreen(editorBgColor);
    M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
    M5Cardputer.Display.setCursor(2, 0);
    M5Cardputer.Display.println("Find (case sensitive)");
    M5Cardputer.Display.println(fitUiLabel(query, M5Cardputer.Display.width()-4));
    M5Cardputer.Display.println("Enter: next match");
    M5Cardputer.Display.println("Esc: return");
    Keyboard_Class::KeysState ks;
    while (!readAppKeys(ks)) { updateRTC(); updateBatteryStatus(); handleLowBattery(); delay(10); }
    if (ks.enter && query.length()) {
      int found = textBuffer.indexOf(query, cursorIndex + (cursorIndex < textBuffer.length() ? 1 : 0));
      if (found < 0) found = textBuffer.indexOf(query);
      if (found >= 0) { cursorIndex = found; ensureCursorVisible(); }
      else showStatusMessage("No match", 2000);
      active = false;
    } else if (ks.del) {
      if (query.length()) { size_t i = query.length()-1; while(i && ((uint8_t)query[i]&0xC0)==0x80) --i; query.remove(i); }
    } else {
      for (auto c : ks.word) { if (c == 27 || c == '`') { active = false; break; } if (query.length() < 128) query += c; }
    }
  }
  lastRenderedAppMode = MODE_MENU; needsRedraw = true;
}

#include "JournalDiagnostics.inl"
