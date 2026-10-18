#pragma once
#include <chrono>
#include <cstdint>
extern int64_t hostTimeOffsetUs;
inline int64_t esp_timer_get_time() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return duration_cast<microseconds>(steady_clock::now() - t0).count() + hostTimeOffsetUs;
}
