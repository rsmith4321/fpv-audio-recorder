#pragma once
#include <cstdint>
typedef int32_t (*msc_read_cb)(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
typedef int32_t (*msc_write_cb)(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
extern msc_read_cb hostMscRead;
extern bool hostMediaPresent;
class USBMSC {
 public:
  bool begin(uint32_t, uint16_t) { return true; }
  void vendorID(const char *) {} void productID(const char *) {} void productRevision(const char *) {}
  void mediaPresent(bool p) { hostMediaPresent = p; }
  void isWritable(bool) {}
  void onRead(msc_read_cb cb) { hostMscRead = cb; }
  void onWrite(msc_write_cb) {}
};
