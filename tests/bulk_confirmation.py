#!/usr/bin/env python3
"""Compile the actual modal confirmation helper with controlled key sources."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'Tiny_Journal.ino').read_text();start=s.index('static bool confirmBulkDelete(');body=s.index('{',start);depth=1;end=body+1
while depth:
 depth+=(s[end]=='{')-(s[end]=='}');end+=1
actual=s[start:end]
shim=r'''
#include <cassert>
#include <string>
#include <deque>
#include <vector>
#include <iostream>
using String=std::string;
namespace fonts{int efontJA_12;}
namespace Keyboard_Class { struct KeysState{std::vector<char> word;}; }
int editorFgColor=1,editorBgColor=0,updates=0,clears=0;
bool btAnyKeyDown=true;
std::deque<char> future,queued={'Y'};
struct Display {void fillScreen(int){} void setFont(int*){} void setTextSize(int){} void setTextColor(int,int){} void setCursor(int,int){} void print(const String&){} };
struct Keyboard {bool isPressed(){return updates<3;}};
struct Cardputer {struct Display Display;struct Keyboard Keyboard;void update(){++updates;}} M5Cardputer;
void drawUiFooter(const String&){} void updateRTC(){} void delay(int){}
void btKeyboardPollConnection(){} void btProcessHidReports(){btAnyKeyDown=updates<3;}
void btClearInputEventQueue(){++clears;queued.clear();}
bool readAppKeys(Keyboard_Class::KeysState &ks){assert(updates>=3&&!btAnyKeyDown&&queued.empty());assert(!future.empty());ks.word.push_back(future.front());future.pop_front();return true;}
'''
main=r'''
int main(){
 future={'D','D','\n','n'};
 assert(!confirmBulkDelete("Delete all?"));assert(future.empty()&&clears>=3);
 updates=clears=0;btAnyKeyDown=true;queued={'Y'};future={'D','Y'};
 assert(confirmBulkDelete("Delete all?"));assert(future.empty()&&clears>=3);
 updates=clears=0;btAnyKeyDown=true;future={27};
 assert(!confirmBulkDelete("Delete all?"));
 std::cout<<"Actual bulk confirmation drains held/queued presses; D/Enter repeats cannot confirm, fresh Y required\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(shim+actual+main);out=Path(d)/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
