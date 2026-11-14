#include <string>
#include <map>
#include <memory>
#include <vector>
#include <cstring>
#include <cassert>
#include <iostream>
using String = std::string;
static const int FILE_WRITE = 1, FILE_READ = 0;
static const int SD_SPI_CS_PIN=12, SPI=0;
struct MockSD;
extern MockSD SD;
struct File {
  std::shared_ptr<std::string> bytes;
  size_t position=0;
  bool writable=false, error=false;
  File()=default;
  File(std::shared_ptr<std::string> b,bool w):bytes(b),writable(w){}
  explicit operator bool() const {return bool(bytes);}
  size_t write(const uint8_t*,size_t);
  size_t read(uint8_t *dst,size_t n);
  void close() { bytes.reset(); }
  void flush(){}
  size_t size() const {return bytes?bytes->size():0;}
  bool getWriteError()const{return error;}
};
struct MockSD {
 std::map<std::string,std::shared_ptr<std::string>> files;
 int failures=0, failAt=-1, steps=0;
 bool shortWrite=false;
 bool fail() {return ++steps == failAt;}
 bool begin(int,int,int){return !fail();}
 bool exists(const String& p){return files.count(p);}
 bool remove(const String& p){if(fail())return false;return files.erase(p);}
 bool rename(const String&a,const String&b){if(fail()||!exists(a)||exists(b))return false;files[b]=files[a];files.erase(a);return true;}
 File open(const String&p,int mode) {
  if(fail())return {};
  if(mode==FILE_WRITE) {files[p]=std::make_shared<std::string>();return {files[p],true};}
  if(!exists(p))return {};return {files[p],false};
 }
};
MockSD SD;
size_t File::read(uint8_t *dst,size_t n) {if(!bytes||SD.fail())return 0;n=std::min(n,bytes->size()-position);memcpy(dst,bytes->data()+position,n);position+=n;return n;}
size_t File::write(const uint8_t*src,size_t n){if(!bytes)return 0;if(SD.fail()||SD.shortWrite){n/=2;error=true;}bytes->append((const char*)src,n);return n;}
bool writeFileSafely(const String &path, const uint8_t *data, size_t length) {
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) return false;
  const String temp = path + ".tmp", backup = path + ".bak";
  // Complete an interrupted rename by restoring the last known good version.
  if (!SD.exists(path) && SD.exists(backup) && !SD.rename(backup, path)) return false;
  if (SD.exists(temp) && !SD.remove(temp)) return false;
  File out = SD.open(temp, FILE_WRITE);
  if (!out) return false;
  size_t total = 0;
  while (total < length) {
    size_t n = (length - total < 1024) ? length - total : 1024;
    if (out.write(data + total, n) != n) { out.close(); SD.remove(temp); return false; }
    total += n;
  }
  out.flush();
  bool complete = out.size() == length && !out.getWriteError();
  out.close();
  if (!complete) { SD.remove(temp); return false; }
  // Read back while the original is still present. FAT cannot guarantee atomicity
  // under power loss; the backup and boot recovery cover the replacement window.
  File verify = SD.open(temp, FILE_READ);
  uint8_t chunk[512]; size_t checked = 0;
  while (verify && checked < length) {
    size_t n = (length - checked < sizeof(chunk)) ? length - checked : sizeof(chunk);
    if (verify.read(chunk, n) != n || memcmp(chunk, data + checked, n)) { complete = false; break; }
    checked += n;
  }
  if (!verify || checked != length) complete = false;
  if (verify) verify.close();
  if (!complete) { SD.remove(temp); return false; }
  bool hadOriginal = SD.exists(path);
  if (hadOriginal) {
    if (SD.exists(backup) && !SD.remove(backup)) return false;
    if (!SD.rename(path, backup)) return false;
  }
  if (!SD.rename(temp, path)) {
    if (hadOriginal) SD.rename(backup, path);
    return false;
  }
  return true;
}

static bool goodOriginalSurvives() {
 for (const auto &path : {"note.txt", "note.txt.bak"}) {
  auto i=SD.files.find(path);if(i!=SD.files.end() && *i->second=="old reliable note")return true;
 }
 return false;
}
int main(){
 const std::string fresh(4097,'x');
 int failuresTested=0;
 for(int initial=0;initial<3;++initial) {
  auto reset=[&]{
    SD=MockSD{};
    if(initial!=2) SD.files["note.txt"]=std::make_shared<std::string>("old reliable note");
    if(initial!=0) SD.files["note.txt.bak"]=std::make_shared<std::string>(initial==1?"older revision":"old reliable note");
  };
  reset();
  bool ok=writeFileSafely("note.txt",(const uint8_t*)fresh.data(),fresh.size());
  assert(ok && *SD.files["note.txt"]==fresh && goodOriginalSurvives());
  int total=SD.steps;
  for(int fail=1;fail<=total;++fail) {
    reset();SD.failAt=fail;
    bool result=writeFileSafely("note.txt",(const uint8_t*)fresh.data(),fresh.size());
    assert(goodOriginalSurvives());
    if(result)assert(*SD.files["note.txt"]==fresh);
    ++failuresTested;
  }
 }
 SD=MockSD{};SD.files["note.txt.bak"]=std::make_shared<std::string>("old reliable note");SD.shortWrite=true;
 assert(!writeFileSafely("note.txt",(const uint8_t*)fresh.data(),fresh.size()));
 assert(goodOriginalSurvives());
 std::cout<<"PASS: replacement success, "<<failuresTested<<" injected filesystem/readback failures across three initial states, recovery and short writes preserve prior note\n";
}
