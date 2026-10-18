#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
typedef int esp_partition_subtype_t;
#define ESP_PARTITION_TYPE_DATA 1
struct esp_partition_t { uint32_t size; };
extern std::vector<uint8_t> hostFlash;
extern esp_partition_t hostPart;
inline const esp_partition_t *esp_partition_find_first(int, esp_partition_subtype_t, const char *) { return &hostPart; }
inline int esp_partition_read(const esp_partition_t *, uint32_t a, void *d, size_t n) { if (a + n > hostFlash.size()) return -1; memcpy(d, &hostFlash[a], n); return 0; }
inline int esp_partition_write(const esp_partition_t *, uint32_t a, const void *s, size_t n) {
  if (a + n > hostFlash.size()) return -1; auto *p = (const uint8_t *)s; for (size_t i = 0; i < n; ++i) hostFlash[a + i] &= p[i]; return 0; }
inline int esp_partition_erase_range(const esp_partition_t *, uint32_t a, size_t n) { if (a % 4096 || n % 4096 || a + n > hostFlash.size()) return -1; memset(&hostFlash[a], 0xff, n); return 0; }
