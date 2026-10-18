// Flash log record layout, kept in a header so Arduino's generated prototypes see it.
#pragma once
#include <stdint.h>
#include <vector>

static constexpr uint32_t kSector = 4096;
static constexpr uint32_t kMagic = 0x3143504F;     // "OPC1"
static constexpr uint8_t kTypeAudio = 0;
static constexpr uint8_t kTypeEnd = 1;
static constexpr uint8_t kTypeMeta = 2;

struct __attribute__((packed)) ChunkHeader {
  uint32_t magic;
  uint32_t seq;         // global, increases by one per chunk
  uint16_t recId;
  uint8_t type;
  uint8_t reserved;
  uint32_t firstFrame;  // frame index within the recording
  uint16_t frames;
  uint16_t payloadLen;
  uint32_t crc;         // CRC32 of header (crc = 0) and payload
};
static_assert(sizeof(ChunkHeader) == 24, "header size");

// Time sources, best first when several meta chunks disagree.
static constexpr uint8_t kTimeNone = 0;
static constexpr uint8_t kTimeUsb = 1;   // set over USB (bench)
static constexpr uint8_t kTimeFc = 2;    // Betaflight MSP_RTC (GPS or configurator)

// Written at the start of each recording, when the clock first becomes known, and
// every 30 s, so the oldest audio can be overwritten without losing the timestamp.
struct __attribute__((packed)) MetaPayload {
  uint32_t boot;        // power-up counter
  uint16_t flight;      // recording number within this power-up, from 1
  uint8_t timeSource;
  uint8_t reserved;
  int64_t utcMs;        // UTC wall clock at frame atFrame (0 if unknown)
  uint32_t atFrame;
  int16_t tzMinutes;    // local offset for file dates, from the TZ command
  uint16_t reserved2;
};
static_assert(sizeof(MetaPayload) == 24, "meta size");

struct ChunkLoc {
  uint32_t firstFrame;
  uint16_t sector;
  uint16_t offset;
};

struct Recording {
  uint16_t id = 0;
  uint32_t firstSeq = 0xffffffff, lastSeq = 0;
  uint32_t frames = 0;        // span of surviving audio, gaps included (WAV length)
  uint32_t firstFrame = 0xffffffff;  // first surviving frame (older audio overwritten)
  uint32_t packetFrames = 0;  // stored packets, for O downloads
  uint32_t packetBytes = 0;   // length bytes + packets, for O downloads
  bool ended = false;
  char reason[16] = "";
  uint32_t boot = 0;
  uint16_t flight = 0;
  uint8_t timeSource = kTimeNone;
  int64_t utcMs = 0;          // UTC at frame utcFrame
  uint32_t utcFrame = 0;
  int16_t tzMinutes = 0;
  std::vector<ChunkLoc> chunks;  // audio chunks in order, for random access

  // UTC at the first surviving frame, or 0 when no clock was available.
  int64_t startUtcMs() const {
    if (timeSource == kTimeNone) return 0;
    return utcMs + (static_cast<int64_t>(firstFrame) - static_cast<int64_t>(utcFrame)) * 20;
  }
};
