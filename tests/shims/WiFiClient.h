#pragma once
#include <string>
#include <new>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
// Arduino String returns success/failure from reserve and concat. The
// fault controls model allocation failures without allocating a huge buffer.
class String : public std::string {
 public:
  using std::string::string;
  static inline bool failNextReserve = false;
  static inline int concatBudget = -1;
  bool reserve(size_t n) {
    if (failNextReserve) { failNextReserve = false; return false; }
    try { std::string::reserve(n); return true; }
    catch (const std::bad_alloc &) { return false; }
  }
  bool concat(const char *s) {
    if (concatBudget == 0) return false;
    if (concatBudget > 0) --concatBudget;
    try { std::string::append(s); return true; }
    catch (const std::bad_alloc &) { return false; }
  }
  bool concat(char c) {
    if (concatBudget == 0) return false;
    if (concatBudget > 0) --concatBudget;
    try { std::string::push_back(c); return true; }
    catch (const std::bad_alloc &) { return false; }
  }
  bool endsWith(const char *s) const {
    size_t n = std::strlen(s);
    return size() >= n && compare(size() - n, n, s) == 0;
  }
};
inline size_t strlcpy(char*d,const char*s,size_t n){size_t length=strlen(s);if(n){size_t copy=std::min(length,n-1);memcpy(d,s,copy);d[copy]=0;}return length;}
inline unsigned long millis(){using namespace std::chrono;return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
class WiFiClient {
 int fd=-1;
 public:
 void setTimeout(unsigned long){}
 int connect(const char*host,uint16_t port,int){fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);inet_pton(AF_INET,host,&addr.sin_addr);return ::connect(fd,(sockaddr*)&addr,sizeof(addr))==0;}
 bool connected(){return fd>=0;}
 size_t write(const uint8_t*p,size_t size){ssize_t n=::send(fd,p,size,0);return n>0?(size_t)n:0;}
 void stop(){if(fd>=0){::shutdown(fd,SHUT_RDWR);::close(fd);fd=-1;}}
};
#define WIFI_STA 1
#define WL_CONNECTED 3
struct FakeWiFi {int status(){return WL_CONNECTED;}void mode(int){}void begin(const char*,const char*){}};
inline FakeWiFi WiFi;
