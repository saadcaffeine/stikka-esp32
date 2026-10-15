#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <algorithm>
using std::min;
#define HEX 16
struct String : std::string {
  String() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(int v) : std::string(std::to_string(v)) {}
  String(int v, int base) { char b[16]; snprintf(b, sizeof b, base == 16 ? "%x" : "%d", v); assign(b); }
  String(unsigned v) : std::string(std::to_string(v)) {}
  String(long v) : std::string(std::to_string(v)) {}
  String(unsigned long v) : std::string(std::to_string(v)) {}
  String(char c) : std::string(1, c) {}
  bool reserve(size_t n) { std::string::reserve(n); return true; }
  bool isEmpty() const { return empty(); }
  long toInt() const { return atol(c_str()); }
  bool concat(const char* p, size_t n) { append(p, n); return true; }
  void replace(const char* a, const char* b) { std::string f(a), t(b); size_t p = 0; while ((p = find(f, p)) != npos) { std::string::replace(p, f.size(), t); p += t.size(); } }
  const char* c_str() const { return std::string::c_str(); }
  String operator+(const String& o) const { return String(std::string(*this) + std::string(o)); }
  String operator+(const char* o) const { return String(std::string(*this) + o); }
  bool operator==(const char* o) const { return std::string(*this) == o; }
  bool operator==(const String& o) const { return std::string(*this) == std::string(o); }
  bool operator!=(const char* o) const { return !(*this == o); }
};
inline String operator+(const char* a, const String& b) { return String(std::string(a) + std::string(b)); }
class Print {
 public:
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { for (size_t i = 0; i < n; i++) write(b[i]); return n; }
  virtual ~Print() {}
  size_t print(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  size_t print(const String& s) { return write((const uint8_t*)s.data(), s.size()); }
  template <class T> size_t print(const T& v) { return print(String(std::to_string(v))); }
};
extern unsigned long fakeNow;
inline unsigned long millis() { return fakeNow; }
inline void delay(unsigned long ms) { fakeNow += ms; }
struct EspC { bool restarted = false; void restart() { restarted = true; } unsigned long getFreeHeap() { return 200000; } unsigned long getMaxAllocHeap() { return 100000; } };
extern EspC ESP;
#define PROGMEM
