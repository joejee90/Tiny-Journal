#include "../JournalTemplateMetadata.h"
#include <cassert>
#include <string>
#include <vector>
#include <iostream>
static std::vector<std::string> parse(const std::string &file, bool &ok, size_t &read) {
 size_t total=0;read=0;ok=true;std::vector<std::string> lines;char line[JOURNAL_TEMPLATE_LINE_BYTES+1];
 auto next=[&](){return read<file.size()?int(uint8_t(file[read++])):-1;};
 for(;;){auto result=readJournalTemplateMetadataLine(next,total,line);if(result==JournalTemplateLine::Invalid){ok=false;return {};};if(result==JournalTemplateLine::End)return lines;lines.emplace_back(line);}
}
int main(){
 bool ok;size_t read;
 auto lines=parse("#@name: Notes\n#@targetDir: /journal/notes\n"+std::string(100000,'x'),ok,read);
 assert(ok&&lines.size()==2&&read<100); // never allocates/reads the large body
 lines=parse("#@name: "+std::string(600,'x'),ok,read);assert(!ok&&read<=513);
 lines=parse(std::string(100000,'x'),ok,read);assert(ok&&lines.empty()&&read==1);
 std::string many;for(int i=0;i<1000;++i)many+="#@name: x\n";
 lines=parse(many,ok,read);assert(!ok&&read==JOURNAL_TEMPLATE_METADATA_BYTES+1);
 lines=parse(" \t#@name: Notes\r\nBody",ok,read);assert(ok&&lines.size()==1);
 lines=parse("#@name: Notes",ok,read);assert(ok&&lines.size()==1);
 size_t total=0;char line[JOURNAL_TEMPLATE_LINE_BYTES+1];
 auto broken=[](){return -2;};assert(readJournalTemplateMetadataLine(broken,total,line)==JournalTemplateLine::Invalid);
 std::cout<<"Template metadata stops before body; rejects long lines, excess metadata and read failures\n";
}
