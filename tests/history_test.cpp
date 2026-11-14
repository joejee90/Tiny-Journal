#include "../JournalHistory.h"
#include <cassert>
#include <iostream>
int main() {
 JournalHistory h; JournalHistory::State s;
 h.remember("",0); h.remember("a",1);
 assert(h.undo("a£",3,s) && s.text=="a" && s.cursor==1);
 assert(h.redo("a",1,s) && s.text=="a£" && s.cursor==3);
 h.remember("new branch",10); assert(!h.redo("x",1,s));
 std::string large(20000,'x'); assert(!h.remember(large.c_str(),0)); assert(!h.undo("x",1,s));
 std::cout << "History branching, UTF-8 roundtrip and memory bound passed\n";
}
