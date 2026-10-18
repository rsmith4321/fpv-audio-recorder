#pragma once
#include <map>
#include <string>
#include <cstdint>
struct HostString : std::string { using std::string::string; HostString(const std::string &s) : std::string(s) {} };
class Preferences {
  std::map<std::string, uint64_t> n; std::map<std::string, std::string> s;
 public:
  bool begin(const char *, bool) { return true; }
  uint16_t getUShort(const char *k, uint16_t d) { return n.count(k) ? n[k] : d; }
  void putUShort(const char *k, uint16_t v) { n[k] = v; }
  uint32_t getUInt(const char *k, uint32_t d) { return n.count(k) ? n[k] : d; }
  void putUInt(const char *k, uint32_t v) { n[k] = v; }
  HostString getString(const char *k, const char *d) { return s.count(k) ? HostString(s[k]) : HostString(d); }
  void putString(const char *k, const char *v) { s[k] = v; }
};
