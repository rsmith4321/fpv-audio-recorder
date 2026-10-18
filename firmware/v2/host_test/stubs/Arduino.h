// Host stand-ins for the Arduino/ESP-IDF APIs the recorder uses (host_test only).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <deque>
#include <condition_variable>
#define IRAM_ATTR
#define SET_LOOP_TASK_STACK_SIZE(x)
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define SERIAL_8N1 0
#define ARDUINO_USB_CDC_ON_BOOT 0
#define ARDUINO_USB_MODE 0
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
inline uint32_t millis() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
extern int hostLed, hostButton;
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) { if (pin == 21) hostLed = v; }
inline int digitalRead(int pin) { return pin == 0 ? hostButton : HIGH; }
inline void *ps_malloc(size_t n) { return calloc(1, n); }

// FreeRTOS
typedef void *TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define pdMS_TO_TICKS(ms) (ms)
struct portMUX_TYPE { std::mutex m; };
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(x) (x)->m.lock()
#define portEXIT_CRITICAL(x) (x)->m.unlock()
struct HostNotify { std::mutex m; std::condition_variable cv; int count = 0; };
extern HostNotify hostNotify;
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t, void *arg, int, TaskHandle_t *h, int) {
  if (strcmp(name, "capture") == 0) return pdPASS;  // the harness feeds the PCM ring itself
  std::thread(fn, arg).detach();
  if (h) *h = &hostNotify;
  return pdPASS;
}
inline uint32_t ulTaskNotifyTake(int, uint32_t ms) {
  std::unique_lock<std::mutex> l(hostNotify.m);
  hostNotify.cv.wait_for(l, std::chrono::milliseconds(ms), [] { return hostNotify.count > 0; });
  int c = hostNotify.count; hostNotify.count = 0; return c;
}
inline void xTaskNotifyGive(TaskHandle_t) { { std::lock_guard<std::mutex> l(hostNotify.m); ++hostNotify.count; } hostNotify.cv.notify_one(); }
inline void vTaskDelay(uint32_t ms) { delay(ms ? ms : 1); }

class Print {
 public:
  virtual size_t write(const uint8_t *b, size_t n) = 0;
  size_t write(uint8_t c) { return write(&c, 1); }
  size_t printf(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return write((const uint8_t *)buf, n);
  }
  size_t println(const char *s) { size_t n = write((const uint8_t *)s, strlen(s)); return n + write((const uint8_t *)"\n", 1); }
  void flush() {}
};
class HardwareSerial : public Print {
 public:
  void begin(int, int = 0, int = 0, int = 0) {}
  int available() { return 0; }
  int read() { return -1; }
  using Print::write;
  size_t write(const uint8_t *, size_t n) override { return n; }
};
extern HardwareSerial Serial1;
#define Serial Serial0
extern HardwareSerial Serial0;
