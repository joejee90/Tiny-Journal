#!/usr/bin/env python3
"""Exercise actual Wi-Fi config save and menu handler under SD/allocation faults."""
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
#include <string>
#include <vector>
#include <iostream>
static bool reserveFail=false,saveOk=true,connectOk=true;
static int concatFailAt=-1,concatCalls=0,writes=0,disconnects=0,opened=0;
struct String:std::string {
 using std::string::string;using std::string::operator=;
 size_t length()const{return size();}void remove(size_t p){erase(p);}
 bool reserve(size_t n){if(reserveFail)return false;std::string::reserve(n);return true;}
 bool concat(const String&s){if(concatCalls++==concatFailAt)return false;append(s);return true;}
 bool concat(char c){if(concatCalls++==concatFailAt)return false;push_back(c);return true;}
};
String lastMessage,wifiSsid,wifiPassword,wifiPasswordInput,lastSaved;
const char*WIFI_CFG_PATH="config";
void showStatusMessage(const String&s,int){lastMessage=s;}
bool writeFileSafely(const char*,const uint8_t*p,size_t n){++writes;lastSaved.assign((const char*)p,n);return saveOk;}
namespace Keyboard_Class{struct KeysState{bool del=false,enter=false;std::vector<char>word;};}
enum{WIFI_STATE_LIST,WIFI_STATE_PWENTRY,MENU_SETTINGS_SYNC};
int wifiSetupState=WIFI_STATE_PWENTRY,wifiSelectedIndex=0,wifiScrollOffset=0,listVisibleLines=3;
bool wifiConfigured=false,needsRedraw=false;
struct WiFiNetwork{String ssid;};std::vector<WiFiNetwork>wifiNetworks={{"example-network"}};
void openMenu(int){++opened;}void openWiFiSetup(){}
bool connectWiFi(){return connectOk;}void disconnectWiFi(){++disconnects;}
'''
main=r'''
int main(){
 wifiPasswordInput="example-password";
 Keyboard_Class::KeysState ks;ks.enter=true;
 saveOk=false;handleWiFiSetupKeys(ks);
 assert(opened==0&&disconnects==1&&wifiSetupState==WIFI_STATE_PWENTRY);
 assert(lastMessage=="WiFi settings save failed"&&wifiPasswordInput=="example-password");
 saveOk=true;handleWiFiSetupKeys(ks);
 assert(opened==1&&disconnects==2&&lastMessage=="WiFi config saved");
 assert(lastSaved=="example-network\nexample-password\n");
 int before=writes;reserveFail=true;
 assert(!saveWiFiConfig("s","p")&&writes==before);reserveFail=false;
 for(int fail=0;fail<4;++fail){concatCalls=0;concatFailAt=fail;assert(!saveWiFiConfig("s","p")&&writes==before);}
 concatFailAt=-1;concatCalls=0;assert(saveWiFiConfig("s","p")&&writes==before+1);
 assert(lastSaved=="s\np\n");
 std::cout<<"Wi-Fi SD/allocation failures preserve setup, suppress false success and retain complete config writes\n";
}
'''
code=shim+function('bool saveWiFiConfig(')+function('void handleWiFiSetupKeys(')+main
with tempfile.TemporaryDirectory(prefix='journal-wifi-save-') as directory:
    cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test';cpp.write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
