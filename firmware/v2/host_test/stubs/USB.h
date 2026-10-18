#pragma once
#include "Arduino.h"
#include <string>
class USBCDC : public Print {
 public:
  std::string input;  // bytes the harness "types" over USB
  void begin(int = 0) {}
  int available() { return static_cast<int>(input.size()); }
  int read() { if (input.empty()) return -1; int c = static_cast<unsigned char>(input[0]); input.erase(0, 1); return c; }
  using Print::write;
  size_t write(const uint8_t *b, size_t n) override { fwrite(b, 1, n, stdout); return n; }
};
struct HostUSB { void productName(const char *) {} void manufacturerName(const char *) {} bool begin() { return true; } };
extern HostUSB USB;
