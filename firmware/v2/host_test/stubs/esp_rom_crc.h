#pragma once
#include <zlib.h>
inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *b, uint32_t n) { return crc32(crc, b, n); }
