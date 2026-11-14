#include "../JournalMenuUi.h"
#include <cassert>
#include <cstdint>
#include <iostream>
int main() {
 using namespace journalui;
 assert(!scrollThumb(3,0,3,100).visible);
 assert(!scrollThumb(20,0,0,100).visible);
 auto first=scrollThumb(10,0,3,90),last=scrollThumb(10,7,3,90);
 assert(first.visible && first.top==0 && first.height==27);
 assert(last.top+last.height==90);
 assert(scrollThumb(10,100,3,90).top==last.top);
 assert(marqueeOffset(899,100)==0 && marqueeOffset(1900,100)==30);
 assert(marqueeOffset(4300,100)==100 && marqueeOffset(5100,100)==100);
 assert(marqueeOffset(0,0)==0);
 uint32_t start=UINT32_MAX-500,now=start+1900;
 assert(marqueeOffset(uint32_t(now-start),100)==30);
 std::cout<<"Menu scrollbar endpoints, short lists, marquee dwell and clock rollover passed\n";
}
