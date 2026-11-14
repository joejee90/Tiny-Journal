#include "../JournalLayout.h"
#include <cassert>
#include <string>
#include <iostream>
int main() {
 assert(journalVisualLineCount("",0,6,4096)==1);
 assert(journalVisualLineCount("123456",6,6,4096)==1);
 assert(journalVisualLineCount("1234567",7,6,4096)==2);
 assert(journalVisualLineCount("abc def",7,6,4096)==2);
 assert(journalVisualLineCount("\n\n",2,6,4096)==3);
 assert(journalVisualLineCount("\t\t\t",3,6,4096)==3);
 std::string hostile(131072,'\n');
 assert(journalVisualLineCount(hostile.data(),hostile.size(),6,4096)==4097);
 std::string cyrillic="абвгдеж";
 assert(journalVisualLineCount(cyrillic.data(),cyrillic.size(),6,4096)==2);
 std::string malformed;
 for(int i=0;i<20000;++i)malformed += "\xc0\xaf";
 assert(journalVisualLineCount(malformed.data(),malformed.size(),6,4096)==4097);
 assert(journalVisualLineCount("\xed\xa0\x80",3,1,4096)==3);
 assert(journalVisualLineCount("\xf4\x90\x80\x80",4,1,4096)==4);
 std::cout<<"PASS: bounded layout preflight for wrapping, Unicode, tabs and newline-heavy imports\n";
}
