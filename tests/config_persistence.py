#!/usr/bin/env python3
"""Exercise actual clock/last-document persistence and consumers under failures."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
src=(root/'Tiny_Journal.ino').read_text()
def function(signature):
    start=src.index(signature)
    while src[src.index(')',start)+1:].lstrip().startswith(';'): start=src.index(signature,start+1)
    brace=src.index('{',start);depth=1;p=brace+1
    while depth:
        depth+=(src[p]=='{')-(src[p]=='}');p+=1
    return src[start:p]
shim=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <iostream>
static bool reserveFail=false;
static int concatFailAt=-1,concatCalls=0;
struct String:std::string {
 using std::string::string;using std::string::operator=;
 size_t length()const{return size();}
 bool startsWith(const char*p)const{return rfind(p,0)==0;}
 bool reserve(size_t n){if(reserveFail)return false;std::string::reserve(n);return true;}
 bool concat(const String&s){if(concatCalls++==concatFailAt)return false;append(s);return true;}
 bool concat(char c){if(concatCalls++==concatFailAt)return false;push_back(c);return true;}
};
struct Date {int year=2026,month=11,day=14,hour=12,minute=34,second=56;}gDateTime;
bool rtcInitialised=true,bufferDirty=true,savedAfterIdle=false,needsRedraw=false;
unsigned long rtcLastMillis=0,now=0;
unsigned long millis(){return ++now;}void delay(int){}
String currentFilePath="/journal/notes/example.txt",textBuffer="example note",lastMessage,failPath,lastClock,lastPath;
const char*RTC_CFG_PATH="clock";const char*LAST_FILE_PATH="last-document";
int writes=0,opened=0,disconnects=0,dtEditField=0;
void showStatusMessage(const String&s,int){lastMessage=s;}
bool writeFileSafely(const String&path,const uint8_t*p,size_t n){
 ++writes;if(path==failPath)return false;
 if(path==RTC_CFG_PATH)lastClock.assign((const char*)p,n);
 if(path==LAST_FILE_PATH)lastPath.assign((const char*)p,n);
 return true;
}
namespace Keyboard_Class{struct KeysState{bool del=false,enter=false;std::vector<char>word;};}
enum{MENU_SETTINGS};void openMenu(int){++opened;}
namespace JournalPrinter{bool busy(){return false;}}
bool connectWiFi(){return true;}void disconnectWiFi(){++disconnects;}
void configTime(int,int,const char*,const char*){}
bool getLocalTime(tm*t,int){t->tm_year=126;t->tm_mon=10;t->tm_mday=14;t->tm_hour=1;t->tm_min=2;t->tm_sec=3;return true;}
'''
main=r'''
int main(){
 Keyboard_Class::KeysState ks;ks.enter=true;
 failPath=RTC_CFG_PATH;handleDateTimeEditorKeys(ks);
 assert(opened==0&&needsRedraw&&lastMessage=="Time settings save failed");
 failPath="";handleDateTimeEditorKeys(ks);
 assert(opened==1&&lastMessage=="Time saved"&&lastClock=="2026-11-14 12:34:56\n");
 failPath=RTC_CFG_PATH;runTimeSync();
 assert(disconnects==1&&lastMessage=="Time synced; SD save failed");
 failPath="";runTimeSync();
 assert(disconnects==2&&lastMessage=="Time synced, WiFi off"&&lastClock=="2026-11-14 01:02:03\n");
 failPath=RTC_CFG_PATH;bufferDirty=true;savedAfterIdle=false;
 assert(saveToFile()&&!bufferDirty&&savedAfterIdle&&lastMessage=="Note saved; settings save failed");
 failPath=LAST_FILE_PATH;bufferDirty=true;savedAfterIdle=false;
 assert(saveToFile()&&!bufferDirty&&savedAfterIdle&&lastMessage=="Note saved; settings save failed");
 assert(!saveLastEditedFilePath()&&lastMessage=="Last-opened file save failed");
 int before=writes;reserveFail=true;assert(!saveLastEditedFilePath()&&writes==before);reserveFail=false;
 for(int fail=0;fail<2;++fail){concatCalls=0;concatFailAt=fail;assert(!saveLastEditedFilePath()&&writes==before);}
 concatFailAt=-1;failPath="";assert(saveLastEditedFilePath()&&lastPath=="/journal/notes/example.txt\n");
 rtcInitialised=false;before=writes;assert(!saveRTCToSD()&&writes==before);rtcInitialised=true;
 gDateTime.month=2;gDateTime.day=31;assert(!saveRTCToSD()&&writes==before);
 gDateTime.month=11;gDateTime.day=14;
 failPath=currentFilePath;before=writes;bufferDirty=true;savedAfterIdle=false;
 assert(!saveToFile()&&bufferDirty&&!savedAfterIdle&&writes==before+1);
 assert(lastMessage=="SAVE FAILED - note remains unsaved");
 std::cout<<"Clock and last-document failures keep editors honest; committed notes remain saved when auxiliary settings fail\n";
}
'''
code=shim+'\n'.join(function(s) for s in ['bool isLeapYear(', 'int daysInMonth(', 'bool saveRTCToSD(', 'bool saveLastEditedFilePath(', 'bool saveToFile(', 'void runTimeSync()', 'void handleDateTimeEditorKeys('])+main
with tempfile.TemporaryDirectory(prefix='journal-config-save-') as directory:
    cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
