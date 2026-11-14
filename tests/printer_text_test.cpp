#include "../PrinterText.h"
#include <cassert>
#include <string>
int main(){
 assert(PrinterText::cp437(0xa3)==156);
 assert(PrinterText::cp437(0xfc)==129);
 assert(PrinterText::cp437(0x1f642)=='?');
 std::string text="a\xc2\xa3\xc3\xbc";size_t i=0;
 assert(PrinterText::next(text.data(),text.size(),i)=='a');
 assert(PrinterText::next(text.data(),text.size(),i)==0xa3);
 assert(PrinterText::next(text.data(),text.size(),i)==0xfc);
 std::string invalid="\xe2X";i=0;
 assert(PrinterText::next(invalid.data(),invalid.size(),i)==0xfffd);
 assert(PrinterText::next(invalid.data(),invalid.size(),i)=='X');
 std::string overlong="\xc0\xaf";i=0;
 assert(PrinterText::next(overlong.data(),overlong.size(),i)==0xfffd);
}
