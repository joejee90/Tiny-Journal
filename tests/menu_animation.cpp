// Exercise the actual renderer/poll code: animation must publish only a completed
// selected-row buffer, with no full redraw request or visible clear.
#include "../JournalMenuUi.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
class String : public std::string {
 public:
  using std::string::string;
  size_t length() const { return size(); }
};
namespace fonts { int efontJA_12; }
enum MenuId { MENU_NONE, MENU_MAIN, MENU_OTHER };
constexpr int MODE_MENU=1,MODE_EDITOR=0;
int appMode=MODE_MENU,baseLineHeight=12,lineHeight=26;
MenuId currentMenu=MENU_MAIN;
bool needsRedraw=false;
uint16_t editorFgColor=0xffff,editorBgColor=0x001f;
String statusMessage;
unsigned long statusMessageUntil=0;
uint32_t fakeNow=0;
unsigned long millis() { return fakeNow; }
float scale=2;
float uiTextScale() { return scale; }
String fitUiLabel(const String &text,int) { return text; }
struct Rect { int x,y,w,h; };
struct Display {
 int rawClears=0,blits=0,waits=0;
 Rect last{};
 int width(){return 240;} int height(){return 135;}
 int textWidth(const String &text){return int(text.size()*10*scale);}
 void fillRect(int,int,int,int,uint16_t){++rawClears;}
 void drawRect(int,int,int,int,uint16_t){}
 void setFont(int*){} void setTextSize(float){}
 void setTextColor(uint16_t,uint16_t){} void setCursor(int,int){}
 void print(const String&){} void setClipRect(int,int,int,int){} void clearClipRect(){}
 void waitDMA(){++waits;}
} ;
struct { struct Display Display; } M5Cardputer;
struct M5Canvas {
 static inline bool failAllocation=false;
 Display *parent;bool allocated=false,filled=false;int w=0,h=0,cursor=0;
 explicit M5Canvas(Display *p):parent(p){}
 void *getBuffer(){return allocated?this:nullptr;}
 void deleteSprite(){allocated=false;}
 void setPsram(bool){} void setColorDepth(int){}
 void *createSprite(int width,int height){w=width;h=height;allocated=!failAllocation;return getBuffer();}
 void fillSprite(uint16_t){assert(allocated);filled=true;}
 void setFont(int*){} void setTextSize(float){} void setTextWrap(bool,bool){}
 void setTextColor(uint16_t,uint16_t){} void setCursor(int x,int){cursor=x;}
 void print(const String&){}
 void pushSprite(int x,int y){assert(allocated&&filled);filled=false;++parent->blits;parent->last={x,y,w,h};}
};
#include "../JournalMenuUi.inl"
int main() {
 auto &display=M5Cardputer.Display;
 String longText="This selected menu label is much longer than its row";
 drawUiListLabel(longText,3,30,227,true);
 assert(uiMarqueeActive&&display.blits==1&&display.rawClears==0);
 assert(display.last.x==3&&display.last.y==30&&display.last.w==227&&display.last.h==26);
 int clears=display.rawClears;
 for(fakeNow=10;fakeNow<=900;fakeNow+=10)pollUiAnimation();
 assert(display.blits==1&&!needsRedraw); // stationary dwell produces zero traffic
 fakeNow=1000;pollUiAnimation();
 assert(display.blits==2&&uiMarqueeCanvas.cursor==-3);
 assert(display.rawClears==clears&&!needsRedraw);
 fakeNow=1010;pollUiAnimation();assert(display.blits==2);
 // Navigation/state redraw cannot receive a stale-row write.
 needsRedraw=true;fakeNow=1100;pollUiAnimation();assert(display.blits==2);
 needsRedraw=false;currentMenu=MENU_OTHER;pollUiAnimation();assert(display.blits==2);
 currentMenu=MENU_MAIN;fakeNow=1200;
 drawUiListLabel("short",3,56,227,true);
 assert(!uiMarqueeActive&&!uiMarqueeCanvas.getBuffer());
 fakeNow=2000;pollUiAnimation();assert(display.blits==2);
 // Allocation failure falls back to static text, and never schedules animation.
 M5Canvas::failAllocation=true;drawUiListLabel(longText,3,30,227,true);
 assert(!uiMarqueeActive);clears=display.rawClears;
 fakeNow=4000;pollUiAnimation();assert(display.rawClears==clears&&!needsRedraw);
 M5Canvas::failAllocation=false;
 // Active buffer is released upon returning to the editor.
 drawUiListLabel(longText,3,30,227,true);assert(uiMarqueeActive);
 appMode=MODE_EDITOR;pollUiAnimation();assert(!uiMarqueeActive&&!uiMarqueeCanvas.getBuffer());
 // Clock rollover does not restart or freeze animation.
 appMode=MODE_MENU;fakeNow=UINT32_MAX-500;
 drawUiListLabel(longText,3,30,227,true);
 fakeNow=uint32_t(UINT32_MAX-500)+uint32_t(1900);pollUiAnimation();
 assert(uiMarqueeOffset==30&&!needsRedraw);
 // Status expiration still requests its one structural redraw.
 statusMessage="Saved";statusMessageUntil=fakeNow+100;
 pollUiAnimation();fakeNow+=110;pollUiAnimation();assert(needsRedraw);
 std::cout<<"Buffered selected-row updates, dwell suppression, navigation ownership, allocation fallback and clock rollover passed\n";
}
