#!/usr/bin/env python3
"""Actual reset completion helper with injected provisioning/save/read failures."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];source=(root/'Tiny_Journal.ino').read_text()
def fn(name):
 a=source.index(name);b=source.index('{',a);depth=1;e=b+1
 while depth:depth+=(source[e]=='{')-(source[e]=='}');e+=1
 return source[a:e]
a=source.index('const char *TEMPLATE_DAILY =');b=source.index('void createTemplateFileIfMissing(',a)
templates=source[a:b]
actual='\n'.join(fn(n) for n in ['static bool resetFileMatches(','static bool resetConfigReadable(','static bool completeFactoryReset('])
shim=r'''
#include <cassert>
#include <string>
#include <map>
#include <set>
#include <cstring>
#include <iostream>
class String:public std::string{public:using std::string::string;String(const std::string&s):std::string(s){}size_t length()const{return size();}};
struct Node{bool directory;std::string contents;};
std::map<std::string,Node> files;
std::set<std::string> failReads;
struct File{
 std::string path;size_t position=0;bool valid=false;
 File()=default;explicit File(std::string p):path(p),valid(files.count(p)){}
 explicit operator bool()const{return valid;}
 bool isDirectory()const{return files.at(path).directory;}
 size_t size()const{return files.at(path).contents.size();}
 int read(){if(!valid||position>=size()||failReads.count(path))return -1;return uint8_t(files.at(path).contents[position++]);}
 void close(){}
};
constexpr int FILE_READ=0;constexpr size_t MAX_DOCUMENT_BYTES=128*1024;
struct{File open(const char*p,int=0){return File(p);}bool exists(const String&p){return files.count(p);}}SD;
const char *WIFI_CFG_PATH="/journal/config/wifi.cfg",*GDRIVE_CFG_PATH="/journal/config/gdrive.cfg",*CONFIG_README_PATH="/journal/config/CONFIG-README.txt",*LAST_FILE_PATH="/journal/config/lastfile.txt";
String currentFilePath,textBuffer,status;size_t cursorIndex=0;
bool bufferDirty=false,savedAfterIdle=true,needsRedraw=false,failCreate=false,failSave=false,failLastFile=false;
int creates=0,saves=0;
void showStatusMessage(const String&s,int){status=s;}
void createNewBlankNote(){++creates;if(failCreate)return;currentFilePath="/journal/misc/new.txt";textBuffer="";cursorIndex=0;bufferDirty=true;savedAfterIdle=false;}
bool saveToFile(){++saves;if(failSave)return false;files[currentFilePath]={false,""};if(!failLastFile)files[LAST_FILE_PATH]={false,currentFilePath+"\n"};bufferDirty=false;savedAfterIdle=true;return true;}
'''
main=r'''
void init(){
 files.clear();failReads.clear();failCreate=failSave=failLastFile=false;creates=saves=0;
 currentFilePath="/journal/notes/old.txt";textBuffer="Existing editor text";cursorIndex=5;bufferDirty=true;savedAfterIdle=false;needsRedraw=false;status="";
 for(const char*p:{"/journal","/journal/config","/journal/templates","/journal/daily","/journal/notes","/journal/meetings","/journal/projects","/journal/travel","/journal/misc","/journal/archive","/journal/audio","/journal/audio/export"})files[p]={true,""};
 files[WIFI_CFG_PATH]={false,"ssid\npassword\n"};files[GDRIVE_CFG_PATH]={false,"refresh\nclient\nsecret\nfolder\n"};files[CONFIG_README_PATH]={false,"Guide\n"};
 const char*paths[]={"daily","quick","meeting","project","travel"};const char*contents[]={TEMPLATE_DAILY,TEMPLATE_QUICK,TEMPLATE_MEETING,TEMPLATE_PROJECT,TEMPLATE_TRAVEL};
 for(int i=0;i<5;i++)files[std::string("/journal/templates/")+paths[i]+".tpl"]={false,contents[i]};
}
void kept(){assert(creates==0&&textBuffer=="Existing editor text"&&cursorIndex==5&&bufferDirty&&!savedAfterIdle&&status.find("incomplete")!=std::string::npos);}
int main(){
 init();assert(completeFactoryReset());assert(creates==1&&saves==1&&!bufferDirty&&savedAfterIdle&&files.count(currentFilePath));
 init();files.erase("/journal/audio/export");assert(!completeFactoryReset());kept();
 init();files["/journal/misc"].directory=false;assert(!completeFactoryReset());kept();
 init();files.erase(WIFI_CFG_PATH);assert(!completeFactoryReset());kept();
 init();files[GDRIVE_CFG_PATH].contents="truncated";assert(!completeFactoryReset());kept();
 init();failReads.insert(CONFIG_README_PATH);assert(!completeFactoryReset());kept();
 init();files["/journal/templates/quick.tpl"].contents="partial";assert(!completeFactoryReset());kept();
 init();failCreate=true;assert(!completeFactoryReset());assert(creates==1&&saves==0&&textBuffer=="Existing editor text"&&cursorIndex==5&&bufferDirty);
 init();failSave=true;assert(!completeFactoryReset());assert(creates==1&&saves==1&&textBuffer.empty()&&bufferDirty&&!savedAfterIdle);
 init();failLastFile=true;assert(!completeFactoryReset());assert(files.count(currentFilePath)&&bufferDirty&&!savedAfterIdle);
 std::cout<<"Actual reset completion verifies directories/config/templates and requires checked blank-note save plus last-file readback\n";
}
'''
with tempfile.TemporaryDirectory()as d:
 p=Path(d)/'test.cpp';p.write_text(shim+templates+actual+main);o=Path(d)/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(o)],check=True);subprocess.run([str(o)],check=True)
caller=fn('void runFactoryReset() {')
assert caller.index('if (!completeFactoryReset()) return;')<caller.index('showStatusMessage("Factory reset complete"')
assert 'bufferDirty = false;' not in caller
