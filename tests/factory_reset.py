#!/usr/bin/env python3
"""Failure injection against actual journal recursion and reset-state handler."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'Tiny_Journal.ino').read_text()
def function(name):
 start=source.index(name);body=source.index('{',start);depth=1;end=body+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
actual=function('static bool deleteAllFilesRecursively(')+function('static bool deleteJournalForFactoryReset(')
shim=r'''
#include <cassert>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <cstring>
#include <iostream>
class String:public std::string {
public:
 using std::string::string;
 String(const std::string &s):std::string(s){}
 bool endsWith(const char *s)const{size_t n=strlen(s);return size()>=n&&compare(size()-n,n,s)==0;}
};
std::map<std::string,bool> files; // bool isDirectory
std::set<std::string> failingRemoves,failingOpens;
struct File {
 std::string path;std::vector<std::string> children;size_t next=0;bool valid=false;
 File()=default;
 explicit File(std::string p):path(p){
  valid=files.count(p);
  if(valid&&files[p]) for(const auto &entry:files){
   auto split=entry.first.find_last_of('/');
   if(entry.first.substr(0,split)==p)children.push_back(entry.first);
  }
 }
 explicit operator bool()const{return valid;}
 bool isDirectory()const{return valid&&files.at(path);}
 void close(){}
 File openNextFile(){return next<children.size()?File(children[next++]):File();}
 const char*name()const{auto split=path.find_last_of('/');return path.c_str()+split+1;}
};
struct {
 File open(const char*p){return failingOpens.count(p)?File():File(p);}
 bool exists(const String&p){return files.count(p);}
 bool remove(const char*p){if(failingRemoves.count(p)||!files.count(p)||files[p])return false;files.erase(p);return true;}
} SD;
const char *WIFI_CFG_PATH="/journal/config/wifi.cfg",*GDRIVE_CFG_PATH="/journal/config/gdrive.cfg";
String currentFilePath="/journal/notes/current.txt",status;
bool bufferDirty=false,savedAfterIdle=true,needsRedraw=false;
String textBuffer="Unchanged editor contents";size_t cursorIndex=12;
void showStatusMessage(const String&s,int){status=s;}
void init(){
 files={{"/journal",true},{"/journal/config",true},{"/journal/notes",true},
        {"/journal/notes/current.txt",false},{"/journal/notes/other.txt",false},
        {"/journal/config/wifi.cfg",false},{"/journal/config/gdrive.cfg",false},
        {"/unrelated.txt",false}};
 failingOpens.clear();failingRemoves.clear();status="";bufferDirty=false;savedAfterIdle=true;needsRedraw=false;
}
'''
main=r'''
int main(){
 init();assert(deleteJournalForFactoryReset(true));
 assert(files.count(WIFI_CFG_PATH)&&files.count(GDRIVE_CFG_PATH)&&files.count("/unrelated.txt"));
 assert(!files.count(currentFilePath));
 // A removal failure propagates out of a nested folder. The current file was
 // removed first, so its in-memory text must remain available to save again.
 init();failingRemoves.insert("/journal/notes/other.txt");
 assert(!deleteJournalForFactoryReset(false));
 assert(files.count("/journal/notes/other.txt")&&!files.count(currentFilePath));
 assert(bufferDirty&&!savedAfterIdle&&needsRedraw&&status.find("incomplete")!=std::string::npos);
 assert(textBuffer=="Unchanged editor contents"&&cursorIndex==12);
 // If the current file itself could not be deleted, retain its existing flags.
 init();failingRemoves.insert(currentFilePath);
 assert(!deleteJournalForFactoryReset(false));
 assert(files.count(currentFilePath)&&!bufferDirty&&savedAfterIdle);
 // Failure to open a directory must not be treated as an empty, successful reset.
 init();failingOpens.insert("/journal/notes");
 assert(!deleteJournalForFactoryReset(false));assert(files.count(currentFilePath));
 init();failingOpens.insert("/journal");
 assert(!deleteJournalForFactoryReset(false));assert(files.count("/unrelated.txt"));
 std::cout<<"Actual reset propagates remove/open failures, preserves current editor state and confines deletion to journal\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(shim+actual+main);out=Path(d)/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
# Ensure the actual interactive caller stops before discarding/reinitialising
# its editor state when the tested failure handler returns false.
caller=function('void runFactoryReset() {')
assert caller.index('if (!deleteJournalForFactoryReset(keepSyncCfg)) return;') < caller.index('ensureJournalDirs();')
