static bool uiMarqueeActive = false;
static String uiMarqueeText;
static uint32_t uiMarqueeStarted = 0, uiLastFrame = 0;
static int uiMarqueeX = 0, uiMarqueeY = 0, uiMarqueeWidth = 0, uiMarqueeHeight = 0;
static int uiMarqueeOverflow = 0, uiMarqueeOffset = 0;
static float uiMarqueeScale = 1;
static MenuId uiMarqueeMenu = MENU_NONE;
static uint16_t uiMarqueeFg = 0, uiMarqueeBg = 0;
static M5Canvas uiMarqueeCanvas(&M5Cardputer.Display);

static void stopUiMarquee() {
  uiMarqueeActive = false;
  // A display DMA transfer may still reference the sprite's memory.
  M5Cardputer.Display.waitDMA();
  uiMarqueeCanvas.deleteSprite();
  uiMarqueeText = "";
}

static void drawUiMarqueeFrame(int offset) {
  M5Cardputer.Display.waitDMA();
  uiMarqueeCanvas.fillSprite(uiMarqueeBg);
  uiMarqueeCanvas.setFont(&fonts::efontJA_12);
  uiMarqueeCanvas.setTextSize(uiMarqueeScale);
  uiMarqueeCanvas.setTextWrap(false, false);
  uiMarqueeCanvas.setTextColor(uiMarqueeFg, uiMarqueeBg);
  uiMarqueeCanvas.setCursor(-offset, 0);
  uiMarqueeCanvas.print(uiMarqueeText);
  // Publish the finished row in one transfer; never clear the visible row first.
  uiMarqueeCanvas.pushSprite(uiMarqueeX, uiMarqueeY);
  M5Cardputer.Display.waitDMA();
  uiMarqueeOffset = offset;
}

int uiFooterHeight() { return (baseLineHeight > 0 ? baseLineHeight : 12) + 2; }
int uiListLabelWidth(int total, int visible) {
  return M5Cardputer.Display.width() - 6 - (total > visible ? 7 : 0);
}
void drawUiFooter(const String &text) {
  int height = uiFooterHeight();
  int y = M5Cardputer.Display.height() - height;
  M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), height, editorBgColor);
  M5Cardputer.Display.setFont(&fonts::efontJA_12);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(editorFgColor, editorBgColor);
  M5Cardputer.Display.setCursor(2, y + 1);
  M5Cardputer.Display.print(fitUiLabel(text, M5Cardputer.Display.width() - 4));
  M5Cardputer.Display.setTextSize(uiTextScale());
}
void drawUiScrollbar(int total, int first, int visible, int topY, int height) {
  auto thumb = journalui::scrollThumb(total, first, visible, height);
  if (!thumb.visible) return;
  int x = M5Cardputer.Display.width() - 5;
  M5Cardputer.Display.fillRect(x, topY, 5, height, editorBgColor);
  M5Cardputer.Display.drawRect(x, topY, 5, height, editorFgColor);
  M5Cardputer.Display.fillRect(x + 1, topY + thumb.top, 3, thumb.height, editorFgColor);
}
void drawUiListLabel(const String &text, int x, int y, int widthPx, bool selected) {
  int overflow = M5Cardputer.Display.textWidth(text) - widthPx;
  if (selected && overflow > 0 && widthPx > 0) {
    bool same = uiMarqueeText == text && uiMarqueeMenu == currentMenu &&
                uiMarqueeX == x && uiMarqueeY == y && uiMarqueeWidth == widthPx &&
                uiMarqueeHeight == lineHeight && uiMarqueeScale == uiTextScale();
    if (!same) uiMarqueeStarted = millis();
    M5Cardputer.Display.waitDMA();
    if (!uiMarqueeCanvas.getBuffer() || uiMarqueeWidth != widthPx || uiMarqueeHeight != lineHeight) {
      uiMarqueeCanvas.deleteSprite();
      uiMarqueeCanvas.setPsram(false);
      uiMarqueeCanvas.setColorDepth(16);
      uiMarqueeCanvas.createSprite(widthPx, lineHeight);
    }
    uiMarqueeText = text;
    uiMarqueeMenu = currentMenu;
    uiMarqueeX = x; uiMarqueeY = y;
    uiMarqueeWidth = widthPx; uiMarqueeHeight = lineHeight;
    uiMarqueeOverflow = overflow;
    uiMarqueeScale = uiTextScale();
    uiMarqueeFg = editorBgColor; uiMarqueeBg = editorFgColor;
    uiMarqueeActive = uiMarqueeCanvas.getBuffer() && uiMarqueeText == text;
    if (uiMarqueeActive) {
      uiLastFrame = millis();
      drawUiMarqueeFrame(journalui::marqueeOffset(uint32_t(uiLastFrame - uiMarqueeStarted), overflow));
      return;
    }
    // Low memory: show a static shortened label instead of repeatedly clearing it.
  } else if (selected) {
    stopUiMarquee();
  }
  M5Cardputer.Display.fillRect(x, y, widthPx, lineHeight, selected ? editorFgColor : editorBgColor);
  M5Cardputer.Display.setTextColor(selected ? editorBgColor : editorFgColor, selected ? editorFgColor : editorBgColor);
  M5Cardputer.Display.setClipRect(x, y, widthPx, lineHeight);
  M5Cardputer.Display.setCursor(x, y);
  M5Cardputer.Display.print(fitUiLabel(text, widthPx));
  M5Cardputer.Display.clearClipRect();
}
void pollUiAnimation() {
  uint32_t now = millis();
  if (appMode != MODE_MENU) {
    if (uiMarqueeCanvas.getBuffer() || uiMarqueeActive) stopUiMarquee();
    return;
  }
  static bool hadStatus = false;
  bool hasStatus = statusMessage.length() && (long)(statusMessageUntil - now) > 0;
  if (hadStatus && !hasStatus) needsRedraw = true;
  hadStatus = hasStatus;
  // A pending navigation/state redraw owns the screen; never paint its old row.
  if (needsRedraw || !uiMarqueeActive || uiMarqueeMenu != currentMenu || uint32_t(now - uiLastFrame) < 33) return;
  int offset = journalui::marqueeOffset(uint32_t(now - uiMarqueeStarted), uiMarqueeOverflow);
  if (offset == uiMarqueeOffset) return; // dwell or sub-pixel movement
  uiLastFrame = now;
  drawUiMarqueeFrame(offset);
}

