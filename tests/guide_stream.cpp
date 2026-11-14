#include "../JournalGuide.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
struct Reader {
 std::string text; size_t cursor=0;
 int read(){return cursor<text.size()?(unsigned char)text[cursor++]:-1;}
 uint32_t position(){return cursor;}
 bool seek(uint32_t n){if(n>text.size())return false;cursor=n;return true;}
};
int characters(const char *s){int n=0;for(;*s;++s)if(((unsigned char)*s&0xc0)!=0x80)++n;return n;}
int main(){
 using namespace journalguide;
 assert(generatedMarker("Tiny Journal generated user guide | V. 2026.11.14"));
 for(const char *s:{"My private README", "Tiny Journal generated user guide | V. ", "Tiny Journal generated user guide | V. 2026.", "Tiny Journal generated user guide | V. 2026.11 notes", "Tiny Journal generated user guide | V. .."})assert(!generatedMarker(s));
 assert(safeTarget(false,false,false,false,false,false));
 assert(safeTarget(true,true,true,true,true,true));
 // Protect a user's original, backup or temporary file, even if the main guide is absent.
 assert(!safeTarget(true,false,false,false,false,false));
 assert(!safeTarget(false,false,true,false,false,false));
 assert(!safeTarget(false,false,false,false,true,false));
 assert(!safeTarget(true,true,true,false,false,false));
 char line[LINE_BYTES];
 Reader unicode{"AB£éЖ😀CD\n\nnext\r\nlast"};
 std::vector<std::string> expected={"AB£","éЖ😀","CD","","next","last"};
 std::vector<std::string> actual;std::vector<uint32_t> starts;
 while(unicode.position()<unicode.text.size()){
  starts.push_back(unicode.position());
  assert(nextLine(unicode,line,sizeof(line),3,characters));actual.emplace_back(line);
 }
 // Width three splits 'next' too.
 expected={"AB£","éЖ😀","CD","","nex","t","las","t"};
 assert(actual==expected);
 // Backtracking to each recorded page/line must reproduce the same bytes.
 for(size_t i=starts.size();i-->0;){assert(unicode.seek(starts[i]));assert(nextLine(unicode,line,sizeof(line),3,characters));assert(actual[i]==line);}
 Reader longLine{std::string(1500,'x')};size_t bytes=0;
 while(nextLine(longLine,line,sizeof(line),10000,characters)){assert(strlen(line)<LINE_BYTES);bytes+=strlen(line);}
 assert(bytes==1500);
 Reader bad{"\xff\xc2Z"};assert(nextLine(bad,line,sizeof(line),100,characters));assert(std::string(line)=="??Z");
 Reader empty{""};assert(!nextLine(empty,line,sizeof(line),3,characters));
 Reader narrow{"Жx"};assert(nextLine(narrow,line,sizeof(line),0,characters));assert(std::string(line)=="Ж");
 std::cout<<"Guide ownership protects user files/sidecars; bounded UTF-8 streaming, backward page offsets and malformed input passed\n";
}
