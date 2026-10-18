#include <Arduino.h>
#include <Preferences.h>
#include <map>
#include <vector>
#include <algorithm>
#include "driver/i2s_pdm.h"
#include "driver/temperature_sensor.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include <atomic>
#include <string>
#include <time.h>
#include <opus.h>
#include "USB.h"
#include "USBMSC.h"
#include "tusb.h"
#include "recorder_log.h"

#if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MODE
#error "Build with USBMode=default,CDCOnBoot=cdc (CDC on boot disabled): the recorder is a TinyUSB drive plus serial port"
#endif
// TinyUSB composite device: the serial port (old protocol) plus a read-only drive.
USBCDC USBSerial;
#undef Serial
#define Serial USBSerial

// SD-free flight recorder: records the XIAO ESP32S3 Sense PDM microphone to a raw
// "rec" flash partition (see partitions.csv) as Opus, 16 kHz mono, 20 ms frames.
//
// Storage is a ring log of 4 KB sectors. Each sector holds back-to-back chunks:
// a 24-byte header (magic, global sequence, recording id, first frame, CRC32) plus
// up to 0.5 s of length-prefixed Opus packets. Chunks never cross a sector, so a
// power cut can only corrupt the chunk being written. When the head reaches the
// oldest sector it erases it: the oldest audio is overwritten (dashcam-style).
//
// USB protocol is the SD firmware's, so Easy Eject keeps working: I, T, L, D,
// G/RECnnnn.WAV, S, E. L/D/G return WAV, decoded on the fly from Opus. Extras:
// O/RECnnnn.WAV streams the raw packets (fast; fetch_flash_recordings.py),
// Q prints status, WIPE erases all recordings.
//
// Plugged into a computer it is also a read-only USB drive (FPVAUDIO) with one WAV
// per recording, named by local start time, power-up (P) and flight (F) number, plus
// FLIGHTS.CSV and README.TXT. The drive is built on the fly from the flash log (see
// "USB drive" below); it goes away while a recording is running.
//
// Each recording carries meta chunks (power-up, flight, UTC clock) at the start, when
// the clock first becomes known, and every 30 s, so the start time survives when the
// ring overwrites the oldest audio. The clock comes from Betaflight MSP_RTC (set by
// GPS), or the CLOCK command on the bench.
//
// Format: hold BOOT for 10 s while idle (LED blinks fast, then three long flashes when done), or
// the WIPE command. It is instant: everything older than the wipe is ignored and
// overwritten as new audio is recorded.
//
// FC link is MSP read-only (MSP_BOXIDS, MSP_STATUS, MSP_RTC). No flight commands are sent.
// D6/GPIO43 TX -> FC RX; D7/GPIO44 RX <- FC TX; share GND.

SET_LOOP_TASK_STACK_SIZE(64 * 1024);  // libopus uses alloca

static constexpr int kMicClock = 42;
static constexpr int kMicData = 41;
static constexpr uint8_t kFCRx = 44;
static constexpr uint8_t kFCTx = 43;
static constexpr uint32_t kSampleRate = 16000;
static constexpr int kFrameSamples = 320;           // 20 ms
static constexpr int kFrameBytes = kFrameSamples * 2;
static constexpr int kBitrate = 32000;
static constexpr int kComplexity = 5;
static constexpr int kMaxPacket = 255;              // length fits the 1-byte prefix
static constexpr int kFramesPerChunk = 25;          // flush every 0.5 s
static constexpr int kRingFrames = 512;             // 10.24 s PCM backlog in PSRAM
static constexpr uint8_t kMspStatus = 101;
static constexpr uint8_t kMspBoxIDs = 119;
static constexpr uint8_t kMspRtc = 247;
static constexpr int kLed = 21;                     // user LED, active low
static constexpr int kBootButton = 0;
static constexpr uint32_t kMetaEveryFrames = 1500;  // 30 s
static const char *kDefaultTz = "EST5EDT,M3.2.0,M11.1.0";
// Digital gain in Q8 (64 = -12 dB) and DC blocker pole, as in the SD firmware.
static constexpr int32_t kGainQ8 = 64;
static constexpr int32_t kDcPoleQ15 = 32604;

static const esp_partition_t *recPart = nullptr;
static uint32_t sectorCount = 0;
static std::vector<std::pair<uint32_t, uint32_t>> sectorOrder;  // (first seq, sector), oldest first
static std::map<uint16_t, Recording> recordings;

static i2s_chan_handle_t rxChan = nullptr;
static temperature_sensor_handle_t chipTemp = nullptr;
static OpusEncoder *encoder = nullptr;
static OpusDecoder *decoder = nullptr;
static opus_int32 lookahead = 0;
static Preferences prefs;

// PCM ring (capture task -> loop).
static int16_t *pcmRing = nullptr;
static volatile uint32_t ringHead = 0, ringTail = 0;
static volatile uint32_t ringOverflowFrames = 0;
static volatile bool capturing = false;
static volatile bool resetFilter = false;
static volatile uint32_t dmaOverflows = 0;
static volatile uint64_t samplesCaptured = 0;

// Writer state.
static uint32_t headSector = 0, headOffset = 0, nextSeq = 1;
static bool headErased = false;
static uint8_t chunk[kSector];
static uint16_t pendingFrames = 0, pendingPayload = 0;
static uint32_t recFrame = 0;        // frames written to the current recording
static uint16_t currentId = 0, nextId = 1, latestId = 0;

// Stats.
static uint64_t encUsTotal = 0, flashUsTotal = 0;
static uint32_t encUsMax = 0, flashUsMax = 0, encFrames = 0, maxBacklog = 0, flashWrites = 0;
static uint64_t recStartUs = 0;
static uint32_t recLostFrames = 0, recPacketBytes = 0, recStartDma = 0;
static float tempMax = NAN;

static bool recording = false, manualRecording = false, fcArmed = false;
static bool boxIdsKnown = false, ready = false;
static uint8_t armBoxIndex = 0xff, armedReplies = 0, disarmedReplies = 0;
static uint32_t lastTempAt = 0, lastPollAt = 0, lastStatusAt = 0, lastRtcAt = 0;

// Power-up and flight numbering, wipe point.
static uint32_t bootCount = 0, wipeSeq = 0;
static uint16_t flightNo = 0;
static uint32_t lastMetaFrame = 0;
static uint8_t metaSource = kTimeNone;   // clock source of the last meta written
static uint64_t recStartSamples = 0;

// Wall clock: UTC ms at esp_timer clockBaseUs.
static uint8_t clockSource = kTimeNone;
static int64_t clockBaseUtcMs = 0;
static int64_t clockBaseUs = 0;
static char tzRule[48] = "";
static char usbCommand[64] = {};
static uint8_t usbLength = 0;

// ---------- Capture ----------

static IRAM_ATTR bool onRecvOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  ++dmaOverflows;
  return false;
}

static void captureTask(void *) {
  int16_t frame[kFrameSamples];
  int32_t prevIn = 0, prevOut = 0;
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rxChan, frame, sizeof(frame), &got, portMAX_DELAY) != ESP_OK ||
        got != sizeof(frame))
      continue;
    samplesCaptured += kFrameSamples;
    if (!capturing) continue;
    if (resetFilter) { prevIn = prevOut = 0; resetFilter = false; }
    for (int i = 0; i < kFrameSamples; ++i) {
      const int32_t in = frame[i];
      prevOut = in - prevIn + ((prevOut * kDcPoleQ15) >> 15);
      prevIn = in;
      int32_t out = (prevOut * kGainQ8) >> 8;
      frame[i] = static_cast<int16_t>(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
    }
    if (ringHead - ringTail >= kRingFrames) { ++ringOverflowFrames; continue; }
    memcpy(pcmRing + (ringHead % kRingFrames) * kFrameSamples, frame, sizeof(frame));
    ringHead = ringHead + 1;
  }
}

static bool startMicrophone() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // 16 x 500 frames = 500 ms of DMA buffering to ride out flash erase stalls.
  chanCfg.dma_desc_num = 16;
  chanCfg.dma_frame_num = 500;
  if (i2s_new_channel(&chanCfg, nullptr, &rxChan) != ESP_OK) return false;
  i2s_pdm_rx_config_t pdmCfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRate),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .clk = static_cast<gpio_num_t>(kMicClock),
      .din = static_cast<gpio_num_t>(kMicData),
      .invert_flags = {.clk_inv = false},
    },
  };
  if (i2s_channel_init_pdm_rx_mode(rxChan, &pdmCfg) != ESP_OK) return false;
  i2s_event_callbacks_t cbs = {};
  cbs.on_recv_q_ovf = onRecvOverflow;
  i2s_channel_register_event_callback(rxChan, &cbs, nullptr);
  if (i2s_channel_enable(rxChan) != ESP_OK) return false;
  return xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, 20, nullptr, 0) == pdPASS;
}

// ---------- Flash log ----------

static uint32_t chunkCrc(const ChunkHeader &h, const uint8_t *payload) {
  ChunkHeader copy = h;
  copy.crc = 0;
  uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&copy), sizeof(copy));
  return esp_rom_crc32_le(crc, payload, h.payloadLen);
}

// Calls fn(header, payload) for each valid chunk in a sector buffer.
template <typename Fn>
static void forEachChunk(const uint8_t *sector, Fn fn) {
  uint32_t offset = 0;
  while (offset + sizeof(ChunkHeader) <= kSector) {
    ChunkHeader h;
    memcpy(&h, sector + offset, sizeof(h));
    if (h.magic != kMagic || offset + sizeof(h) + h.payloadLen > kSector) break;
    const uint8_t *payload = sector + offset + sizeof(h);
    if (chunkCrc(h, payload) != h.crc) break;
    fn(h, payload);
    offset += sizeof(h) + h.payloadLen;
  }
}

static uint32_t scanStorage() {
  const uint64_t started = esp_timer_get_time();
  sectorOrder.clear();
  recordings.clear();
  uint32_t maxSeq = 0, maxSeqSector = sectorCount - 1;
  static uint8_t buf[kSector];
  std::vector<std::pair<uint32_t, uint32_t>> found;
  for (uint32_t s = 0; s < sectorCount; ++s) {
    ChunkHeader h;
    if (esp_partition_read(recPart, s * kSector, &h, sizeof(h)) != ESP_OK || h.magic != kMagic) continue;
    found.push_back({static_cast<uint32_t>(h.seq), s});
  }
  std::sort(found.begin(), found.end());
  for (auto &entry : found) {
    const uint32_t s = entry.second;
    if (esp_partition_read(recPart, s * kSector, buf, kSector) != ESP_OK) continue;
    bool any = false;
    uint32_t offset = 0;
    forEachChunk(buf, [&](const ChunkHeader &h, const uint8_t *payload) {
      const uint32_t at = offset;
      offset += sizeof(h) + h.payloadLen;
      if (h.seq >= maxSeq) { maxSeq = h.seq; maxSeqSector = s; }
      if (h.seq < wipeSeq) return;  // formatted away
      any = true;
      Recording &r = recordings[h.recId];
      r.id = h.recId;
      r.firstSeq = std::min(r.firstSeq, h.seq);
      r.lastSeq = std::max(r.lastSeq, h.seq);
      if (h.type == kTypeAudio) {
        r.packetFrames += h.frames;
        r.packetBytes += h.payloadLen;
        r.firstFrame = std::min(r.firstFrame, h.firstFrame);
        r.frames = std::max(r.frames, h.firstFrame + h.frames);  // end frame for now
        r.chunks.push_back({h.firstFrame, static_cast<uint16_t>(s), static_cast<uint16_t>(at)});
      } else if (h.type == kTypeEnd) {
        r.ended = true;
        const size_t n = std::min<size_t>(h.payloadLen, sizeof(r.reason) - 1);
        memcpy(r.reason, payload, n);
        r.reason[n] = '\0';
      } else if (h.type == kTypeMeta && h.payloadLen >= sizeof(MetaPayload)) {
        MetaPayload m;
        memcpy(&m, payload, sizeof(m));
        r.boot = m.boot;
        r.flight = m.flight;
        // Keep the first meta of the best clock source.
        if (m.timeSource > r.timeSource && m.utcMs > 0) {
          r.timeSource = m.timeSource;
          r.utcMs = m.utcMs;
          r.utcFrame = m.atFrame;
          r.tzMinutes = m.tzMinutes;
        } else if (r.timeSource == kTimeNone) {
          r.tzMinutes = m.tzMinutes;
        }
      }
    });
    if (any) sectorOrder.push_back(entry);
  }
  // Drop recordings with no surviving audio; turn end frames into spans.
  for (auto it = recordings.begin(); it != recordings.end();) {
    Recording &r = it->second;
    if (r.packetFrames == 0) { it = recordings.erase(it); continue; }
    r.frames -= r.firstFrame;
    ++it;
  }
  latestId = 0;
  uint32_t latestSeq = 0;
  uint16_t maxId = 0;
  for (auto &kv : recordings) {
    maxId = std::max(maxId, kv.first);
    if (kv.second.lastSeq >= latestSeq) { latestSeq = kv.second.lastSeq; latestId = kv.first; }
  }
  nextSeq = std::max(maxSeq + 1, wipeSeq);
  // Resume in a fresh sector after the newest one, so a torn tail is never appended to.
  headSector = found.empty() ? 0 : (maxSeqSector + 1) % sectorCount;
  headOffset = 0;
  headErased = false;
  nextId = std::max<uint32_t>(nextId, static_cast<uint32_t>(maxId) + 1);
  if (nextId == 0 || nextId > 9999) nextId = 1;
  return static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
}

static bool flushChunk(uint8_t type = kTypeAudio, const void *data = nullptr, uint16_t dataLen = 0) {
  ChunkHeader h = {};
  h.magic = kMagic;
  h.seq = nextSeq;
  h.recId = currentId;
  h.type = type;
  h.firstFrame = recFrame;
  if (type == kTypeAudio) {
    if (pendingFrames == 0) return true;
    h.frames = pendingFrames;
    h.payloadLen = pendingPayload;
  } else {
    // Non-audio chunks reuse the buffer, so pending audio must be flushed first.
    if (pendingFrames && !flushChunk()) return false;
    h.seq = nextSeq;
    h.firstFrame = recFrame;
    h.payloadLen = dataLen;
    memcpy(chunk + sizeof(h), data, dataLen);
  }
  const uint32_t size = sizeof(h) + h.payloadLen;
  if (headOffset + size > kSector) {
    headSector = (headSector + 1) % sectorCount;
    headOffset = 0;
    headErased = false;
  }
  h.crc = chunkCrc(h, chunk + sizeof(h));
  memcpy(chunk, &h, sizeof(h));
  const uint64_t started = esp_timer_get_time();
  if (!headErased) {
    if (esp_partition_erase_range(recPart, headSector * kSector, kSector) != ESP_OK) return false;
    headErased = true;
  }
  if (esp_partition_write(recPart, headSector * kSector + headOffset, chunk, size) != ESP_OK) return false;
  const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - started);
  flashUsTotal += us;
  flashUsMax = std::max(flashUsMax, us);
  ++flashWrites;
  headOffset += size;
  ++nextSeq;
  if (type == kTypeAudio) {
    recFrame += pendingFrames;
    recPacketBytes += pendingPayload;
  }
  pendingFrames = pendingPayload = 0;
  return true;
}

// Queue one packet (len 0 marks a lost frame, concealed on decode).
static bool appendPacket(const uint8_t *packet, uint8_t len) {
  const uint32_t need = 1 + len;
  if (pendingFrames && headOffset + sizeof(ChunkHeader) + pendingPayload + need > kSector) {
    if (!flushChunk()) return false;
  }
  if (!pendingFrames && headOffset + sizeof(ChunkHeader) + need > kSector) {
    headSector = (headSector + 1) % sectorCount;
    headOffset = 0;
    headErased = false;
  }
  uint8_t *dst = chunk + sizeof(ChunkHeader) + pendingPayload;
  dst[0] = len;
  if (len) memcpy(dst + 1, packet, len);
  pendingPayload += need;
  ++pendingFrames;
  if (pendingFrames >= kFramesPerChunk) return flushChunk();
  return true;
}

// ---------- Clock ----------

static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = static_cast<int>(y - era * 400);
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int64_t nowUtcMs() {
  if (clockSource == kTimeNone) return 0;
  return clockBaseUtcMs + (static_cast<int64_t>(esp_timer_get_time()) - clockBaseUs) / 1000;
}

static void setClock(int64_t utcMs, uint8_t source) {
  // Year 2024 or later only: an unset FC clock reports 1970 or nothing.
  if (utcMs < 1704067200000LL || source < clockSource) return;
  clockBaseUtcMs = utcMs;
  clockBaseUs = esp_timer_get_time();
  clockSource = source;
}

// Local time for a UTC instant using the POSIX TZ rule; returns the offset in minutes.
static int16_t localTime(int64_t utcMs, struct tm &out) {
  const time_t t = static_cast<time_t>(utcMs / 1000);
  localtime_r(&t, &out);
  const int64_t localSec = daysFromCivil(out.tm_year + 1900, out.tm_mon + 1, out.tm_mday) * 86400 +
                           out.tm_hour * 3600 + out.tm_min * 60 + out.tm_sec;
  return static_cast<int16_t>((localSec - static_cast<int64_t>(t)) / 60);
}

// Same, but from a stored offset (recordings keep the offset they were made with).
static void localFromOffset(int64_t utcMs, int16_t tzMinutes, struct tm &out) {
  const time_t t = static_cast<time_t>(utcMs / 1000 + tzMinutes * 60);
  gmtime_r(&t, &out);
}

static const char *timeSourceName(uint8_t source) {
  return source == kTimeFc ? "FC" : source == kTimeUsb ? "USB" : "NONE";
}

// Writes a meta chunk tying the frame being captured right now to the wall clock.
static bool writeMeta() {
  MetaPayload m = {};
  m.boot = bootCount;
  m.flight = flightNo;
  m.timeSource = clockSource;
  if (clockSource != kTimeNone) {
    m.atFrame = static_cast<uint32_t>((samplesCaptured - recStartSamples) / kFrameSamples);
    m.utcMs = nowUtcMs();
    struct tm tmLocal;
    m.tzMinutes = localTime(m.utcMs, tmLocal);
  }
  metaSource = clockSource;
  lastMetaFrame = recFrame;
  return flushChunk(kTypeMeta, &m, sizeof(m));
}

// ---------- Recording ----------

static void stopRecording(const char *reason);

static bool encodeAvailable() {
  uint32_t backlog = ringHead - ringTail;
  maxBacklog = std::max(maxBacklog, backlog);
  while (ringHead != ringTail) {
    const int16_t *pcm = pcmRing + (ringTail % kRingFrames) * kFrameSamples;
    uint8_t packet[kMaxPacket];
    const uint64_t started = esp_timer_get_time();
    int len = opus_encode(encoder, pcm, kFrameSamples, packet, sizeof(packet));
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - started);
    encUsTotal += us;
    encUsMax = std::max(encUsMax, us);
    ++encFrames;
    ringTail = ringTail + 1;
    if (len < 0) len = 0;
    if (!appendPacket(packet, static_cast<uint8_t>(len))) return false;
  }
  // Repeat the meta chunk so the clock survives overwrite; write one now if the
  // clock appeared (or improved) during the recording.
  if (recFrame - lastMetaFrame >= kMetaEveryFrames || clockSource > metaSource) {
    if (!writeMeta()) return false;
  }
  // Keep the timeline aligned if the PSRAM backlog ever overflowed.
  uint32_t lost = ringOverflowFrames;
  if (lost) {
    ringOverflowFrames -= lost;
    recLostFrames += lost;
    for (uint32_t i = 0; i < lost; ++i)
      if (!appendPacket(nullptr, 0)) return false;
  }
  return true;
}

static float chipTemperature() {
  float celsius = NAN;
  if (chipTemp != nullptr) temperature_sensor_get_celsius(chipTemp, &celsius);
  return celsius;
}

static void printTemperature() {
  const float celsius = chipTemperature();
  if (isnan(celsius)) { Serial.println("chip_temp_c unavailable"); return; }
  if (recording && (isnan(tempMax) || celsius > tempMax)) tempMax = celsius;
  Serial.printf("chip_temp_c %.1f\n", celsius);
}

static void driveOffline();
static void driveOnline();

static bool startRecording(bool manual) {
  if (recording) return false;
  currentId = nextId;
  nextId = nextId >= 9999 ? 1 : nextId + 1;
  prefs.putUShort("next", nextId);
  opus_encoder_ctl(encoder, OPUS_RESET_STATE);
  recFrame = 0;
  pendingFrames = pendingPayload = 0;
  encUsTotal = flashUsTotal = 0;
  encUsMax = flashUsMax = encFrames = maxBacklog = flashWrites = 0;
  recLostFrames = recPacketBytes = 0;
  recStartDma = dmaOverflows;
  tempMax = NAN;
  ringTail = ringHead;
  ringOverflowFrames = 0;
  resetFilter = true;
  recStartUs = esp_timer_get_time();
  ++flightNo;
  driveOffline();
  recStartSamples = samplesCaptured;
  capturing = true;
  recording = true;
  manualRecording = manual;
  digitalWrite(kLed, LOW);
  Serial.printf("START /REC%04u.WAV %s P%04lu F%02u clock=%s\n", currentId, manual ? "USB" : "ARM",
                static_cast<unsigned long>(bootCount), flightNo, timeSourceName(clockSource));
  if (!writeMeta()) stopRecording("FLASH_ERROR");
  return true;
}

static void stopRecording(const char *reason) {
  if (!recording) return;
  capturing = false;
  bool ok = encodeAvailable() && flushChunk() && flushChunk(kTypeEnd, reason, strlen(reason));
  recording = false;
  manualRecording = false;
  const float seconds = (esp_timer_get_time() - recStartUs) / 1e6f;
  const uint32_t frames = recFrame;
  digitalWrite(kLed, HIGH);
  scanStorage();
  if (recordings.count(currentId)) latestId = currentId;
  driveOnline();
  Serial.printf("STOP /REC%04u.WAV %s %lu %s\n", currentId, reason,
                static_cast<unsigned long>(frames) * kFrameBytes, ok ? "OK" : "FLASH_ERROR");
  Serial.printf("STATS sec=%.1f frames=%lu enc_avg_us=%lu enc_max_us=%lu busy_pct=%.1f "
                "flash_avg_us=%lu flash_max_us=%lu backlog_max=%lu lost=%lu dma_ovf=%lu "
                "kbps=%.1f temp_max_c=%.1f\n",
                seconds, static_cast<unsigned long>(frames),
                static_cast<unsigned long>(encFrames ? encUsTotal / encFrames : 0),
                static_cast<unsigned long>(encUsMax),
                encFrames ? 100.0f * (encUsTotal + flashUsTotal) / (encFrames * 20000.0f) : 0.0f,
                static_cast<unsigned long>(flashWrites ? flashUsTotal / flashWrites : 0),
                static_cast<unsigned long>(flashUsMax), static_cast<unsigned long>(maxBacklog),
                static_cast<unsigned long>(recLostFrames),
                static_cast<unsigned long>(dmaOverflows - recStartDma),
                frames ? recPacketBytes * 8.0f / (frames * 0.02f) / 1000.0f : 0.0f, tempMax);
  currentId = 0;
}

// ---------- Downloads ----------

static bool parseName(const char *name, uint16_t &id) {
  if (strlen(name) == 12 && strncmp(name, "/REC", 4) == 0 && strcmp(name + 8, ".WAV") == 0 &&
      isdigit(name[4]) && isdigit(name[5]) && isdigit(name[6]) && isdigit(name[7])) {
    id = static_cast<uint16_t>(atoi(name + 4));
    return true;
  }
  return false;
}

static void putLE(uint8_t *p, uint32_t v, int n) {
  for (int i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Walk a recording's packets in order; fn(packet, len) per frame.
template <typename Fn>
static void forEachPacket(const Recording &r, Fn fn) {
  static uint8_t buf[kSector];
  for (auto &entry : sectorOrder) {
    if (esp_partition_read(recPart, entry.second * kSector, buf, kSector) != ESP_OK) continue;
    bool past = false;
    forEachChunk(buf, [&](const ChunkHeader &h, const uint8_t *payload) {
      if (h.seq > r.lastSeq) past = true;
      if (h.recId != r.id || h.type != kTypeAudio || h.seq < r.firstSeq || h.seq > r.lastSeq) return;
      uint32_t off = 0;
      for (uint16_t f = 0; f < h.frames && off < h.payloadLen; ++f) {
        const uint8_t len = payload[off];
        fn(payload + off + 1, len);
        off += 1 + len;
      }
    });
    if (past) break;
  }
}

static void sendWav(uint16_t id) {
  if (recording) { Serial.println("ERROR: stop recording before import"); return; }
  auto it = recordings.find(id);
  if (it == recordings.end()) { Serial.println("ERROR: recording not found"); return; }
  const Recording &r = it->second;
  const uint32_t dataBytes = r.frames * kFrameBytes;
  Serial.printf("BEGIN %lu /REC%04u.WAV\n", static_cast<unsigned long>(dataBytes + 44), id);
  uint8_t header[44] = {};
  memcpy(header, "RIFF", 4);
  putLE(header + 4, dataBytes + 36, 4);
  memcpy(header + 8, "WAVEfmt ", 8);
  putLE(header + 16, 16, 4);
  putLE(header + 20, 1, 2);
  putLE(header + 22, 1, 2);
  putLE(header + 24, kSampleRate, 4);
  putLE(header + 28, kSampleRate * 2, 4);
  putLE(header + 32, 2, 2);
  putLE(header + 34, 16, 2);
  memcpy(header + 36, "data", 4);
  putLE(header + 40, dataBytes, 4);
  Serial.write(header, sizeof(header));
  opus_decoder_ctl(decoder, OPUS_RESET_STATE);
  uint32_t sent = 0;
  static int16_t pcm[kFrameSamples];
  forEachPacket(r, [&](const uint8_t *packet, uint8_t len) {
    if (sent >= dataBytes) return;
    int n = len ? opus_decode(decoder, packet, len, pcm, kFrameSamples, 0)
                : opus_decode(decoder, nullptr, 0, pcm, kFrameSamples, 0);
    if (n < 0) n = 0;
    if (n < kFrameSamples) memset(pcm + n, 0, (kFrameSamples - n) * 2);
    Serial.write(reinterpret_cast<uint8_t *>(pcm), kFrameBytes);
    sent += kFrameBytes;
  });
  // Pad if storage changed underneath us, so the promised size is always met.
  memset(pcm, 0, sizeof(pcm));
  while (sent < dataBytes) {
    const uint32_t n = std::min<uint32_t>(kFrameBytes, dataBytes - sent);
    Serial.write(reinterpret_cast<uint8_t *>(pcm), n);
    sent += n;
  }
  Serial.flush();
}

static void sendOpus(uint16_t id) {
  if (recording) { Serial.println("ERROR: stop recording before import"); return; }
  auto it = recordings.find(id);
  if (it == recordings.end()) { Serial.println("ERROR: recording not found"); return; }
  const Recording &r = it->second;
  Serial.printf("BEGIN_OPUS %lu %lu %ld /REC%04u.WAV\n", static_cast<unsigned long>(r.packetBytes),
                static_cast<unsigned long>(r.packetFrames), lookahead * 3, id);
  uint32_t sent = 0;
  forEachPacket(r, [&](const uint8_t *packet, uint8_t len) {
    if (sent + 1 + len > r.packetBytes) return;
    Serial.write(&len, 1);
    if (len) Serial.write(packet, len);
    sent += 1 + len;
  });
  static const uint8_t zero[64] = {};
  while (sent < r.packetBytes) {
    const uint32_t n = std::min<uint32_t>(sizeof(zero), r.packetBytes - sent);
    Serial.write(zero, n);
    sent += n;
  }
  Serial.flush();
}

static void printStatus() {
  uint32_t usedSectors = sectorOrder.size();
  uint64_t frames = 0, bytes = 0;
  for (auto &kv : recordings) { frames += kv.second.frames; bytes += kv.second.packetBytes; }
  const float bytesPerSec = frames ? bytes / (frames * 0.02f) : kBitrate / 8.0f;
  // Usable bytes per sector after headers and end-of-sector padding (about 2.5%).
  const float capacitySec = sectorCount * (kSector * 0.975f) / bytesPerSec;
  Serial.printf("STATUS part_bytes=%lu sectors=%lu used_sectors=%lu recordings=%u latest=%u "
                "stored_sec=%.1f avg_kbps=%.1f capacity_min=%.1f recording=%d armed=%d "
                "bitrate=%d complexity=%d dma_ovf=%lu boot=%lu clock=%s utc_ms=%lld tz=%s\n",
                static_cast<unsigned long>(recPart->size), static_cast<unsigned long>(sectorCount),
                static_cast<unsigned long>(usedSectors), static_cast<unsigned>(recordings.size()),
                latestId, frames * 0.02f, bytesPerSec * 8 / 1000, capacitySec / 60, recording,
                fcArmed, kBitrate, kComplexity, static_cast<unsigned long>(dmaOverflows),
                static_cast<unsigned long>(bootCount), timeSourceName(clockSource),
                static_cast<long long>(nowUtcMs()), tzRule);
  for (auto &kv : recordings) {
    const Recording &r = kv.second;
    Serial.printf("REC /REC%04u.WAV sec=%.1f opus_bytes=%lu end=%s boot=%lu flight=%u clock=%s "
                  "start_utc_ms=%lld tz_min=%d first_frame=%lu\n",
                  kv.first, r.frames * 0.02f, static_cast<unsigned long>(r.packetBytes),
                  r.ended ? r.reason : "POWER_LOSS", static_cast<unsigned long>(r.boot), r.flight,
                  timeSourceName(r.timeSource), static_cast<long long>(r.startUtcMs()), r.tzMinutes,
                  static_cast<unsigned long>(r.firstFrame));
  }
}

// ---------- MSP ----------

static void requestMsp(uint8_t command) {
  const uint8_t packet[] = {'$', 'M', '<', 0, command, command};
  Serial1.write(packet, sizeof(packet));
}

static void handleMspReply(uint8_t command, const uint8_t *payload, uint8_t length) {
  if (command == kMspBoxIDs) {
    armBoxIndex = 0xff;
    for (uint8_t i = 0; i < length; ++i) {
      if (payload[i] == 0) { armBoxIndex = i; break; }  // ARM permanent ID
    }
    boxIdsKnown = armBoxIndex < 32;
    Serial.printf("MSP_ARM_BOX %s\n", boxIdsKnown ? "FOUND" : "UNAVAILABLE");
    return;
  }
  if (command == kMspRtc) {
    // U16 year, U8 month, day, hours, minutes, seconds, U16 millis; UTC. Empty if unset.
    if (length < 9) return;
    const int year = payload[0] | (payload[1] << 8);
    const int month = payload[2], day = payload[3];
    const int64_t ms = payload[7] | (payload[8] << 8);
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return;
    const int64_t utc = (daysFromCivil(year, month, day) * 86400 + payload[4] * 3600 + payload[5] * 60 +
                         payload[6]) * 1000 + ms;
    const bool first = clockSource != kTimeFc;
    setClock(utc, kTimeFc);
    if (first && clockSource == kTimeFc) Serial.printf("CLOCK FC %lld\n", static_cast<long long>(utc));
    return;
  }
  if (command != kMspStatus || !boxIdsKnown || length < 10) return;
  const bool armed = (payload[6 + armBoxIndex / 8] & (1u << (armBoxIndex % 8))) != 0;
  lastStatusAt = millis();
  if (armed) {
    disarmedReplies = 0;
    if (armedReplies < 2) ++armedReplies;
    if (armedReplies >= 2 && !fcArmed) {
      fcArmed = true;
      if (!recording) startRecording(false);
    }
  } else {
    armedReplies = 0;
    if (disarmedReplies < 2) ++disarmedReplies;
    if (disarmedReplies >= 2 && fcArmed) {
      fcArmed = false;
      if (recording && !manualRecording) stopRecording("DISARM");
    }
  }
}

static void consumeMsp() {
  // MSPv1 response: '$M>' + length + command + payload + XOR checksum.
  static uint8_t state = 0, length = 0, command = 0, offset = 0, checksum = 0;
  static uint8_t payload[128];
  while (Serial1.available()) {
    const uint8_t byte = Serial1.read();
    switch (state) {
      case 0: state = byte == '$' ? 1 : 0; break;
      case 1: state = byte == 'M' ? 2 : 0; break;
      case 2: state = byte == '>' ? 3 : 0; break;
      case 3: length = byte; checksum = byte; offset = 0; state = length <= sizeof(payload) ? 4 : 0; break;
      case 4: command = byte; checksum ^= byte; state = length ? 5 : 6; break;
      case 5: payload[offset++] = byte; checksum ^= byte; if (offset == length) state = 6; break;
      case 6:
        if (byte == checksum) handleMspReply(command, payload, length);
        state = 0;
        break;
      default: state = 0; break;
    }
  }
}

// ---------- USB drive ----------
//
// A read-only FAT16 volume generated on the fly; nothing is stored as FAT on flash.
// 512-byte sectors, 16 KB clusters, 128 MB: boot sector, two FATs, a 1024-entry root
// directory, then the files laid out contiguously. WAV data is decoded from Opus by
// decodeTask into a PSRAM window that runs ahead of the host's sequential reads.

static USBMSC MSC;
static constexpr uint32_t kVolSectors = 262144;
static constexpr uint32_t kSecPerClus = 32;
static constexpr uint32_t kClusterBytes = kSecPerClus * 512;
static constexpr uint32_t kFatSectors = 32;
static constexpr uint32_t kRootEntries = 1024;
static constexpr uint32_t kFat1 = 1, kFat2 = kFat1 + kFatSectors;
static constexpr uint32_t kRootStart = kFat2 + kFatSectors;
static constexpr uint32_t kDataStart = kRootStart + kRootEntries * 32 / 512;
static constexpr uint32_t kClusters = (kVolSectors - kDataStart) / kSecPerClus;
static_assert(kClusters >= 4085 && (kClusters + 2) * 2 <= kFatSectors * 512, "FAT16 geometry");
static constexpr uint32_t kWavHeader = 12 + 24 + 8 + 602 + 8;  // RIFF, fmt, bext, data
static constexpr uint32_t kCacheFrames = 1024;                // 20 s of PCM
// The decoder restarts every kBlockFrames, kPreroll frames early, on every read path, so
// a byte of a WAV is the same however the host reaches it (sequential copy or seek).
static constexpr uint32_t kBlockFrames = 200;                 // 4 s
static constexpr uint32_t kPreroll = 8;

enum FileKind : uint8_t { kFileText, kFileWav };
struct VolFile {
  FileKind kind;
  const Recording *rec = nullptr;
  const std::string *text = nullptr;
  uint32_t size = 0, firstCluster = 0, clusters = 0;
};

static std::vector<VolFile> volFiles;              // in cluster order
static std::string readmeText, flightsText, emptyText;
static uint8_t *rootDir = nullptr;                 // kRootEntries * 32, PSRAM
static uint32_t volSerial = 0x46505641;
static std::atomic<bool> volReady{false};
static std::atomic<int> mscInRead{0};
static std::atomic<bool> decoderBusy{false};

// Decode window: WAV frames [winStart, winEnd) of winRec are in wavCache.
static int16_t *wavCache = nullptr;
static portMUX_TYPE winMux = portMUX_INITIALIZER_UNLOCKED;
static const Recording *winRec = nullptr, *reqRec = nullptr;
static uint32_t winStart = 0, winEnd = 0, wantFrame = 0, reqFrame = 0;
static bool resetPending = false;
static TaskHandle_t decodeHandle = nullptr;

static void putFatTime(uint8_t *p, const struct tm &t) {
  const uint16_t time = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2);
  const int year = std::max(0, t.tm_year + 1900 - 1980);
  const uint16_t date = (year << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday;
  putLE(p, time, 2);
  putLE(p + 2, date, 2);
}

static bool recordingLocalStart(const Recording &r, struct tm &out) {
  const int64_t start = r.startUtcMs();
  if (start <= 0) return false;
  localFromOffset(start, r.tzMinutes, out);
  return true;
}

static std::string wavLongName(const Recording &r) {
  char name[48];
  struct tm t;
  if (r.boot == 0)
    snprintf(name, sizeof(name), "FPV_REC%04u.WAV", r.id);  // made before power-up numbering
  else if (recordingLocalStart(r, t))
    snprintf(name, sizeof(name), "FPV_%04d%02d%02d%02d%02d%02d_P%04lu_F%02u.WAV", t.tm_year + 1900,
             t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, static_cast<unsigned long>(r.boot),
             r.flight);
  else
    snprintf(name, sizeof(name), "FPV_P%04lu_F%02u_NOCLOCK.WAV", static_cast<unsigned long>(r.boot), r.flight);
  return name;
}

static void fallbackTime(struct tm &t) {
  // Build date, for files with no clock.
  memset(&t, 0, sizeof(t));
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  t.tm_mon = static_cast<int>((strstr(months, mon) - months) / 3);
  t.tm_mday = atoi(__DATE__ + 4);
  t.tm_year = atoi(__DATE__ + 7) - 1900;
  t.tm_hour = 12;
}

// Builds the 654-byte WAV header with a Broadcast WAV bext chunk, so editors place the
// clip at its recorded time of day.
static void buildWavHeader(const Recording &r, uint8_t *h) {
  memset(h, 0, kWavHeader);
  const uint32_t dataBytes = r.frames * kFrameBytes;
  memcpy(h, "RIFF", 4);
  putLE(h + 4, kWavHeader - 8 + dataBytes, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  putLE(h + 16, 16, 4);
  putLE(h + 20, 1, 2);
  putLE(h + 22, 1, 2);
  putLE(h + 24, kSampleRate, 4);
  putLE(h + 28, kSampleRate * 2, 4);
  putLE(h + 32, 2, 2);
  putLE(h + 34, 16, 2);
  uint8_t *b = h + 36;
  memcpy(b, "bext", 4);
  putLE(b + 4, 602, 4);
  uint8_t *p = b + 8;
  const int64_t start = r.startUtcMs();
  struct tm t;
  const bool timed = recordingLocalStart(r, t);
  char text[256];
  if (timed) {
    struct tm u;
    const time_t ut = static_cast<time_t>(start / 1000);
    gmtime_r(&ut, &u);
    snprintf(text, sizeof(text),
             "FPV audio P%04lu F%02u start_utc=%04d-%02d-%02dT%02d:%02d:%02d.%03dZ tz_min=%d clock=%s",
             static_cast<unsigned long>(r.boot), r.flight, u.tm_year + 1900, u.tm_mon + 1, u.tm_mday,
             u.tm_hour, u.tm_min, u.tm_sec, static_cast<int>(start % 1000), r.tzMinutes,
             timeSourceName(r.timeSource));
  } else {
    snprintf(text, sizeof(text), "FPV audio P%04lu F%02u clock=NONE", static_cast<unsigned long>(r.boot),
             r.flight);
  }
  strncpy(reinterpret_cast<char *>(p), text, 256);
  strncpy(reinterpret_cast<char *>(p + 256), "FPV Audio Recorder", 32);
  snprintf(text, sizeof(text), "REC%04u", r.id);
  strncpy(reinterpret_cast<char *>(p + 288), text, 32);
  if (timed) {
    snprintf(text, sizeof(text), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    memcpy(p + 320, text, 10);
    snprintf(text, sizeof(text), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    memcpy(p + 330, text, 8);
    // Samples since local midnight, to the millisecond.
    const int64_t msOfDay = ((t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec) * 1000LL) + start % 1000;
    const uint64_t ref = static_cast<uint64_t>(msOfDay) * kSampleRate / 1000;
    putLE(p + 338, static_cast<uint32_t>(ref), 4);
    putLE(p + 342, static_cast<uint32_t>(ref >> 32), 4);
  }
  putLE(p + 346, 1, 2);  // bext version
  uint8_t *d = b + 8 + 602;
  memcpy(d, "data", 4);
  putLE(d + 4, dataBytes, 4);
}

static uint8_t lfnChecksum(const uint8_t *shortName) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = static_cast<uint8_t>(((sum & 1) << 7) + (sum >> 1) + shortName[i]);
  return sum;
}

// Appends long-name entries and the 8.3 entry; returns false when the directory is full.
static bool addDirEntry(uint32_t &slot, const char *shortName, const std::string &longName, uint8_t attr,
                        const VolFile *f, const struct tm &when) {
  const size_t len = longName.size();
  const uint32_t lfnCount = (len + 12) / 13;
  if (slot + lfnCount + 1 > kRootEntries) return false;
  uint8_t sfn[11];
  memcpy(sfn, shortName, 11);
  const uint8_t sum = lfnChecksum(sfn);
  static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  for (uint32_t n = lfnCount; n >= 1; --n) {
    uint8_t *e = rootDir + 32 * slot++;
    memset(e, 0, 32);
    e[0] = static_cast<uint8_t>(n | (n == lfnCount ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (int i = 0; i < 13; ++i) {
      const size_t c = (n - 1) * 13 + i;
      uint16_t ch = c < len ? static_cast<uint8_t>(longName[c]) : c == len ? 0x0000 : 0xFFFF;
      putLE(e + pos[i], ch, 2);
    }
  }
  uint8_t *e = rootDir + 32 * slot++;
  memset(e, 0, 32);
  memcpy(e, sfn, 11);
  e[11] = attr;
  putFatTime(e + 14, when);   // created
  putLE(e + 18, (((when.tm_year + 1900 - 1980) << 9) | ((when.tm_mon + 1) << 5) | when.tm_mday), 2);
  putFatTime(e + 22, when);   // modified
  if (f) {
    putLE(e + 26, f->firstCluster, 2);
    putLE(e + 28, f->size, 4);
  }
  return true;
}

static void buildTexts() {
  readmeText =
      "FPV Audio Recorder\r\n"
      "==================\r\n\r\n"
      "GETTING YOUR AUDIO\r\n"
      "  Plug the recorder into a computer with a USB-C cable. It shows up as a\r\n"
      "  drive called FPVAUDIO, like a memory card. Copy the WAV files off it.\r\n"
      "  The drive is read-only: you can't delete single files. The drive goes\r\n"
      "  away while the recorder is recording and comes back when it stops.\r\n\r\n"
      "ONE FILE PER FLIGHT\r\n"
      "  Recording starts when you arm and stops when you disarm.\r\n"
      "  Names: FPV_<date and time>_P<power-up>_F<flight>.WAV, for example\r\n"
      "  FPV_20261018173825_P0005_F01.WAV = 18 Oct 2026, 17:38:25, power-up 5,\r\n"
      "  first flight. The time comes from the flight controller's clock (GPS).\r\n"
      "  Without a clock the name is FPV_P0005_F01_NOCLOCK.WAV.\r\n"
      "  FLIGHTS.CSV lists every file with its start time.\r\n\r\n"
      "WHEN IT'S FULL\r\n"
      "  It holds about 30 minutes. After that the oldest audio is overwritten\r\n"
      "  automatically, so copy flights you want to keep.\r\n\r\n"
      "ERASING EVERYTHING (FORMAT)\r\n"
      "  1. Unplug it from the drone and plug it into USB power or a computer.\r\n"
      "     Don't hold any button while plugging in.\r\n"
      "  2. Wait 5 seconds.\r\n"
      "  3. Press and hold the button marked B for 10 seconds. Keep the USB\r\n"
      "     plug steady with a finger so it doesn't wiggle.\r\n"
      "     - After 1 second the LED blinks slowly. (No blinking? Let go, that's\r\n"
      "       the wrong button. The other one, R, restarts the recorder.)\r\n"
      "     - After 5 seconds it blinks fast.\r\n"
      "     - At 10 seconds it gives three long flashes: erased. Let go.\r\n"
      "  Letting go before the three flashes cancels; nothing is erased.\r\n\r\n"
      "MATCHING WITH VIDEO\r\n"
      "  Each WAV carries its time of day (Broadcast WAV), so Resolve and\r\n"
      "  Premiere can line it up by timecode. audio_video_sync.py (Mac) pairs\r\n"
      "  each WAV with its DJI clip and lines them up to about 10 ms.\r\n";
  flightsText = "file,boot,flight,start_local,start_utc,duration_s,end,clock,tz_min,start_overwritten\r\n";
  for (auto &kv : recordings) {
    const Recording &r = kv.second;
    char line[200], local[24] = "", utc[32] = "";
    struct tm t;
    if (recordingLocalStart(r, t)) {
      snprintf(local, sizeof(local), "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
               t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
      const int64_t start = r.startUtcMs();
      const time_t ut = static_cast<time_t>(start / 1000);
      struct tm u;
      gmtime_r(&ut, &u);
      snprintf(utc, sizeof(utc), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", u.tm_year + 1900, u.tm_mon + 1,
               u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, static_cast<int>(start % 1000));
    }
    snprintf(line, sizeof(line), "%s,%lu,%u,%s,%s,%.2f,%s,%s,%d,%s\r\n", wavLongName(r).c_str(),
             static_cast<unsigned long>(r.boot), r.flight, local, utc, r.frames * 0.02f,
             r.ended ? r.reason : "POWER_LOSS", timeSourceName(r.timeSource), r.tzMinutes,
             r.firstFrame > 0 ? "yes" : "no");
    flightsText += line;
  }
}

static void buildVolume() {
  volFiles.clear();
  buildTexts();
  memset(rootDir, 0, kRootEntries * 32);
  uint32_t slot = 0, nextCluster = 2;
  struct tm built;
  fallbackTime(built);
  // Volume label.
  memcpy(rootDir, "FPVAUDIO   ", 11);
  rootDir[11] = 0x08;
  putFatTime(rootDir + 22, built);
  slot = 1;
  auto place = [&](VolFile f) -> VolFile & {
    f.clusters = (f.size + kClusterBytes - 1) / kClusterBytes;
    f.firstCluster = f.clusters ? nextCluster : 0;
    nextCluster += f.clusters;
    volFiles.push_back(f);
    return volFiles.back();
  };
  volFiles.reserve(recordings.size() + 3);
  VolFile f;
  f.kind = kFileText;
  f.text = &readmeText;
  f.size = readmeText.size();
  addDirEntry(slot, "README  TXT", "README.TXT", 0x21, &place(f), built);
  f.text = &flightsText;
  f.size = flightsText.size();
  addDirEntry(slot, "FLIGHTS CSV", "FLIGHTS.CSV", 0x21, &place(f), built);
  f.text = &emptyText;
  f.size = 0;
  addDirEntry(slot, "METADA~1   ", ".metadata_never_index", 0x22, &place(f), built);  // no Spotlight
  // Newest last; if the directory or the volume fills, the oldest files are left out.
  std::vector<const Recording *> order;
  for (auto &kv : recordings) order.push_back(&kv.second);
  std::sort(order.begin(), order.end(), [](const Recording *a, const Recording *b) { return a->lastSeq < b->lastSeq; });
  uint64_t bytesLeft = static_cast<uint64_t>(kClusters - (nextCluster - 2)) * kClusterBytes;
  size_t first = order.size();
  uint32_t slotsLeft = kRootEntries - slot;
  while (first > 0) {
    const Recording &r = *order[first - 1];
    const uint64_t bytes = (static_cast<uint64_t>(kWavHeader + r.frames * kFrameBytes) + kClusterBytes - 1) /
                           kClusterBytes * kClusterBytes;
    const uint32_t entries = (wavLongName(r).size() + 12) / 13 + 1;
    if (bytes > bytesLeft || entries > slotsLeft) break;
    bytesLeft -= bytes;
    slotsLeft -= entries;
    --first;
  }
  for (size_t i = first; i < order.size(); ++i) {
    const Recording &r = *order[i];
    VolFile w;
    w.kind = kFileWav;
    w.rec = &r;
    w.size = kWavHeader + r.frames * kFrameBytes;
    char sfn[12];
    snprintf(sfn, sizeof(sfn), "FPV%05uWAV", r.id);
    struct tm t;
    if (!recordingLocalStart(r, t)) t = built;
    addDirEntry(slot, sfn, wavLongName(r), 0x21, &place(w), t);
  }
  ++volSerial;
}

static const VolFile *fileForCluster(uint32_t cluster) {
  static size_t last = 0;  // reads are mostly sequential
  if (last < volFiles.size()) {
    const VolFile &f = volFiles[last];
    if (f.clusters && cluster >= f.firstCluster && cluster < f.firstCluster + f.clusters) return &f;
  }
  for (size_t i = 0; i < volFiles.size(); ++i) {
    const VolFile &f = volFiles[i];
    if (f.clusters && cluster >= f.firstCluster && cluster < f.firstCluster + f.clusters) {
      last = i;
      return &f;
    }
  }
  return nullptr;
}

// macOS doesn't poll the drive, so it never notices a media change on its own. After
// the drive goes offline or comes back, re-attach USB (only when a host is connected;
// in flight nothing happens). The serial port drops for about half a second too.
static bool hostRefreshPending = false;

static void refreshHostIfPending() {
  if (!hostRefreshPending) return;
  hostRefreshPending = false;
  if (!tud_mounted()) return;
  Serial.flush();
  delay(30);
  tud_disconnect();
  delay(300);
  tud_connect();
}

static void driveOffline() {
  hostRefreshPending = true;
  volReady = false;
  MSC.mediaPresent(false);
  while (mscInRead.load() || decoderBusy.load()) delay(1);
  portENTER_CRITICAL(&winMux);
  winRec = reqRec = nullptr;
  resetPending = false;
  portEXIT_CRITICAL(&winMux);
}

static void driveOnline() {
  if (!rootDir || !wavCache) return;
  buildVolume();
  volReady = true;
  MSC.mediaPresent(true);
  hostRefreshPending = true;
}

// Packet cursor over a recording's chunks, for the decoder.
struct Cursor {
  const Recording *r = nullptr;
  size_t chunk = 0;
  uint32_t frame = 0;        // next recording frame to produce
  uint8_t buf[kSector];
  ChunkHeader h = {};
  uint32_t off = 0, frameInChunk = 0;
  bool loaded = false;
};

static bool loadChunk(Cursor &c, size_t index) {
  c.loaded = false;
  if (index >= c.r->chunks.size()) return false;
  const ChunkLoc &loc = c.r->chunks[index];
  const uint32_t addr = loc.sector * kSector + loc.offset;
  if (esp_partition_read(recPart, addr, &c.h, sizeof(c.h)) != ESP_OK || c.h.magic != kMagic ||
      loc.offset + sizeof(c.h) + c.h.payloadLen > kSector)
    return false;
  if (esp_partition_read(recPart, addr + sizeof(c.h), c.buf, c.h.payloadLen) != ESP_OK) return false;
  c.chunk = index;
  c.off = 0;
  c.frameInChunk = 0;
  c.loaded = true;
  return true;
}

static void seekCursor(Cursor &c, const Recording *r, uint32_t frame) {
  c.r = r;
  c.frame = frame;
  auto &chunks = r->chunks;
  size_t lo = 0, hi = chunks.size();
  while (lo < hi) {  // first chunk starting after frame
    const size_t mid = (lo + hi) / 2;
    if (chunks[mid].firstFrame <= frame) lo = mid + 1; else hi = mid;
  }
  if (lo == 0) { loadChunk(c, 0); return; }
  if (!loadChunk(c, lo - 1)) return;
  while (c.h.firstFrame + c.frameInChunk < frame && c.frameInChunk < c.h.frames && c.off < c.h.payloadLen) {
    c.off += 1 + c.buf[c.off];
    ++c.frameInChunk;
  }
}

// Packet for c.frame (len 0 = lost or missing, concealed), then advances.
static void nextPacket(Cursor &c, const uint8_t *&packet, uint8_t &len) {
  packet = nullptr;
  len = 0;
  if (c.loaded && (c.frameInChunk >= c.h.frames || c.off >= c.h.payloadLen)) loadChunk(c, c.chunk + 1);
  if (c.loaded && c.h.firstFrame + c.frameInChunk == c.frame) {
    len = c.buf[c.off];
    packet = c.buf + c.off + 1;
    c.off += 1 + len;
    ++c.frameInChunk;
  }
  ++c.frame;
}

// Resets the decoder for WAV frame block (a multiple of kBlockFrames) and primes it with
// the kPreroll frames before it; the cursor is left at block.
static void restartDecoder(OpusDecoder *dec, Cursor &cursor, const Recording *r, uint32_t block) {
  static int16_t scratch[kFrameSamples];
  const uint32_t from = block >= kPreroll ? block - kPreroll : 0;
  seekCursor(cursor, r, r->firstFrame + from);
  opus_decoder_ctl(dec, OPUS_RESET_STATE);
  for (uint32_t f = from; f < block; ++f) {
    const uint8_t *p; uint8_t len;
    nextPacket(cursor, p, len);
    opus_decode(dec, len ? p : nullptr, len, scratch, kFrameSamples, 0);
  }
}

static void decodeTask(void *) {
  static Cursor cursor;
  int err = 0;
  OpusDecoder *dec = opus_decoder_create(kSampleRate, 1, &err);
  static int16_t scratch[kFrameSamples];
  const Recording *cur = nullptr;
  uint32_t lastYield = millis();
  for (;;) {
    decoderBusy = true;
    if (!volReady || !dec) {
      decoderBusy = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    bool reset = false;
    const Recording *r;
    uint32_t start = 0, end = 0, want = 0;
    portENTER_CRITICAL(&winMux);
    if (resetPending) {
      reset = true;
      winRec = reqRec;
      winStart = winEnd = wantFrame = reqFrame;
      resetPending = false;
    }
    r = winRec;
    start = winStart;
    end = winEnd;
    want = wantFrame;
    portEXIT_CRITICAL(&winMux);
    if (reset || r != cur) {
      cur = r;
      if (r) {
        // Restart at the block holding start, then decode up to start.
        const uint32_t block = start / kBlockFrames * kBlockFrames;
        restartDecoder(dec, cursor, r, block);
        for (uint32_t f = block; f < start; ++f) {
          const uint8_t *p; uint8_t len;
          nextPacket(cursor, p, len);
          opus_decode(dec, len ? p : nullptr, len, scratch, kFrameSamples, 0);
        }
      }
    }
    if (!r || end >= r->frames || end >= want + kCacheFrames - 16) {
      decoderBusy = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    // Decode a few frames per pass, then publish them.
    uint32_t n = 0;
    while (n < 8 && end + n < r->frames && end + n < want + kCacheFrames - 16) {
      if ((end + n) % kBlockFrames == 0) restartDecoder(dec, cursor, r, end + n);
      int16_t *out = wavCache + ((end + n) % kCacheFrames) * kFrameSamples;
      const uint8_t *p; uint8_t len;
      nextPacket(cursor, p, len);
      int got = opus_decode(dec, len ? p : nullptr, len, out, kFrameSamples, 0);
      if (got < 0) got = 0;
      if (got < kFrameSamples) memset(out + got, 0, (kFrameSamples - got) * 2);
      ++n;
    }
    portENTER_CRITICAL(&winMux);
    if (!resetPending && winRec == r && winEnd == end) {
      winEnd = end + n;
      if (winEnd - winStart > kCacheFrames - 16) winStart = winEnd - (kCacheFrames - 16);
    }
    portEXIT_CRITICAL(&winMux);
    decoderBusy = false;
    // A long copy keeps this task busy; let core 0's idle task run so the 5 s task
    // watchdog doesn't reset the chip.
    if (millis() - lastYield >= 50) {
      vTaskDelay(1);
      lastYield = millis();
    }
  }
}

// Copies decoded PCM bytes [pcmOff, pcmOff + n) of a recording, waiting for the decoder.
static void readPcm(const Recording *r, uint32_t pcmOff, uint8_t *dst, uint32_t n) {
  while (n) {
    const uint32_t frame = pcmOff / kFrameBytes, inner = pcmOff % kFrameBytes;
    const uint32_t take = std::min<uint32_t>(n, kFrameBytes - inner);
    bool ready = false;
    for (uint32_t waited = 0; waited < 3000; ++waited) {
      portENTER_CRITICAL(&winMux);
      ready = winRec == r && !resetPending && frame >= winStart && frame < winEnd;
      const bool coming = (winRec == r && !resetPending && frame >= winStart && frame <= winEnd + 64) ||
                          (resetPending && reqRec == r && frame >= reqFrame && frame <= reqFrame + 64);
      if (ready || coming) {
        if (!resetPending) wantFrame = frame;
      } else {
        reqRec = r;
        reqFrame = frame;
        resetPending = true;
      }
      portEXIT_CRITICAL(&winMux);
      if (ready || !volReady) break;
      xTaskNotifyGive(decodeHandle);
      vTaskDelay(1);
    }
    if (ready) memcpy(dst, reinterpret_cast<uint8_t *>(wavCache + (frame % kCacheFrames) * kFrameSamples) + inner, take);
    else memset(dst, 0, take);
    dst += take;
    pcmOff += take;
    n -= take;
  }
}

static void readSector(uint32_t lba, uint32_t offset, uint8_t *dst, uint32_t n) {
  memset(dst, 0, n);
  if (lba == 0) {
    uint8_t b[512] = {0xEB, 0x3C, 0x90, 'M', 'S', 'D', 'O', 'S', '5', '.', '0'};
    putLE(b + 11, 512, 2);
    b[13] = kSecPerClus;
    putLE(b + 14, kFat1, 2);
    b[16] = 2;
    putLE(b + 17, kRootEntries, 2);
    b[21] = 0xF8;
    putLE(b + 22, kFatSectors, 2);
    putLE(b + 24, 63, 2);
    putLE(b + 26, 255, 2);
    putLE(b + 32, kVolSectors, 4);
    b[36] = 0x80;
    b[38] = 0x29;
    putLE(b + 39, volSerial, 4);
    memcpy(b + 43, "FPVAUDIO   FAT16   ", 19);
    b[510] = 0x55;
    b[511] = 0xAA;
    memcpy(dst, b + offset, n);
  } else if (lba >= kFat1 && lba < kRootStart) {
    const uint32_t firstEntry = ((lba - kFat1) % kFatSectors) * 256;
    for (uint32_t i = offset / 2; i < (offset + n) / 2; ++i) {
      const uint32_t c = firstEntry + i;
      uint16_t v = 0;
      if (c == 0) v = 0xFFF8;
      else if (c == 1) v = 0xFFFF;
      else if (const VolFile *f = fileForCluster(c)) v = c + 1 < f->firstCluster + f->clusters ? c + 1 : 0xFFFF;
      putLE(dst + i * 2 - offset, v, 2);
    }
  } else if (lba >= kRootStart && lba < kDataStart) {
    memcpy(dst, rootDir + (lba - kRootStart) * 512 + offset, n);
  } else if (lba >= kDataStart) {
    const uint32_t rel = lba - kDataStart;
    const uint32_t cluster = rel / kSecPerClus + 2;
    const VolFile *f = fileForCluster(cluster);
    if (!f) return;
    uint32_t pos = (cluster - f->firstCluster) * kClusterBytes + (rel % kSecPerClus) * 512 + offset;
    if (pos >= f->size) return;
    n = std::min(n, f->size - pos);
    if (f->kind == kFileText) {
      memcpy(dst, f->text->data() + pos, n);
      return;
    }
    if (pos < kWavHeader) {
      uint8_t header[kWavHeader];
      buildWavHeader(*f->rec, header);
      const uint32_t take = std::min(n, kWavHeader - pos);
      memcpy(dst, header + pos, take);
      dst += take;
      pos += take;
      n -= take;
    }
    if (n) readPcm(f->rec, pos - kWavHeader, dst, n);
  }
}

static int32_t mscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  ++mscInRead;
  if (!volReady) { --mscInRead; return -1; }
  uint8_t *dst = static_cast<uint8_t *>(buffer);
  uint32_t done = 0;
  while (done < bufsize) {
    const uint32_t n = std::min<uint32_t>(bufsize - done, 512 - offset);
    readSector(lba, offset, dst + done, n);
    done += n;
    offset += n;
    if (offset == 512) { offset = 0; ++lba; }
  }
  --mscInRead;
  return static_cast<int32_t>(bufsize);
}

static int32_t mscWrite(uint32_t, uint32_t, uint8_t *, uint32_t) { return -1; }

static bool startDrive() {
  rootDir = static_cast<uint8_t *>(ps_malloc(kRootEntries * 32));
  wavCache = static_cast<int16_t *>(ps_malloc(kCacheFrames * kFrameBytes));
  if (!rootDir || !wavCache) return false;
  if (xTaskCreatePinnedToCore(decodeTask, "wavdecode", 32 * 1024, nullptr, 3, &decodeHandle, 0) != pdPASS)
    return false;
  MSC.vendorID("FPV");
  MSC.productID("Audio Recorder");
  MSC.productRevision("2.0");
  MSC.onRead(mscRead);
  MSC.onWrite(mscWrite);
  MSC.isWritable(false);
  MSC.mediaPresent(false);
  return MSC.begin(kVolSectors, 512);
}

// ---------- Format ----------

// Instant format: everything before this sequence number is ignored from now on and
// overwritten in ring order as new audio comes in.
static void wipeRecordings() {
  driveOffline();
  wipeSeq = nextSeq;
  prefs.putUInt("wipeseq", wipeSeq);
  scanStorage();
  driveOnline();
  Serial.printf("WIPE_DONE OK from_seq=%lu\n", static_cast<unsigned long>(wipeSeq));
}

static void pollBootButton() {
  static uint32_t pressedAt = 0;
  static bool pressed = false, wiped = false;
  const uint32_t now = millis();
  if (digitalRead(kBootButton) == HIGH || recording) {
    if (pressed) digitalWrite(kLed, recording ? LOW : HIGH);
    pressed = wiped = false;
    return;
  }
  if (!pressed) {
    pressed = true;
    pressedAt = now;
  }
  const uint32_t held = now - pressedAt;
  if (wiped) return;
  if (held >= 10000) {
    wipeRecordings();
    wiped = true;
    // Done: three long flashes.
    for (int i = 0; i < 3; ++i) {
      digitalWrite(kLed, LOW);
      delay(700);
      digitalWrite(kLed, HIGH);
      delay(400);
    }
  } else if (held >= 1000) {
    // Blink, faster after 5 s, as a warning.
    digitalWrite(kLed, ((held / (held >= 5000 ? 100 : 300)) & 1) ? LOW : HIGH);
  }
}

// ---------- USB ----------

static void wipeRecordings();

static void handleUsbCommand(const char *command) {
  uint16_t id = 0;
  if (strcmp(command, "I") == 0) {
    Serial.println("RECORDER 1 XIAO_ESP32S3_SENSE 16000 MONO16");
  } else if (strcmp(command, "T") == 0) {
    printTemperature();
  } else if (strcmp(command, "L") == 0) {
    if (recording) { Serial.println("ERROR: stop recording before listing files"); return; }
    Serial.println("LIST_BEGIN");
    for (auto &kv : recordings)
      Serial.printf("FILE /REC%04u.WAV %lu\n", kv.first,
                    static_cast<unsigned long>(kv.second.frames * kFrameBytes + 44));
    Serial.println("LIST_END");
  } else if (strcmp(command, "D") == 0) {
    if (!latestId) Serial.println("ERROR: no recordings found");
    else sendWav(latestId);
  } else if (command[0] == 'G' && command[1] == '/') {
    if (parseName(command + 1, id)) sendWav(id);
    else Serial.println("ERROR: invalid recording name");
  } else if (command[0] == 'O' && command[1] == '/') {
    if (parseName(command + 1, id)) sendOpus(id);
    else Serial.println("ERROR: invalid recording name");
  } else if (strcmp(command, "S") == 0) {
    if (fcArmed) Serial.println("ERROR: FC already armed");
    else startRecording(true);
  } else if (strcmp(command, "E") == 0) {
    if (manualRecording) stopRecording("USB");
  } else if (strcmp(command, "Q") == 0) {
    printStatus();
  } else if (strcmp(command, "WIPE") == 0) {
    if (recording) { Serial.println("ERROR: stop recording before wipe"); return; }
    wipeRecordings();
  } else if (strncmp(command, "CLOCK ", 6) == 0) {
    // Bench clock: CLOCK <unix ms, UTC>. The FC clock (GPS) wins when present. Command
    // names must not start with a legacy single-letter command (D I T L S E Q).
    setClock(strtoll(command + 6, nullptr, 10), kTimeUsb);
    Serial.printf("CLOCK %s %lld\n", timeSourceName(clockSource), static_cast<long long>(nowUtcMs()));
  } else if (strcmp(command, "ZONE") == 0 || strncmp(command, "ZONE ", 5) == 0) {
    // POSIX TZ rule for file names, e.g. ZONE EST5EDT,M3.2.0,M11.1.0 or ZONE UTC0.
    if (command[4] == ' ' && command[5]) {
      strlcpy(tzRule, command + 5, sizeof(tzRule));
      prefs.putString("tz", tzRule);
      setenv("TZ", tzRule, 1);
      tzset();
      if (!recording) { driveOffline(); driveOnline(); }
    }
    Serial.printf("ZONE %s\n", tzRule);
  }
}

static void consumeUsb() {
  while (Serial.available()) {
    const char byte = static_cast<char>(Serial.read());
    // Single-character legacy commands are supported for Easy Eject's initial client.
    if (usbLength == 0 && strchr("DITLSEQ", byte) != nullptr && byte != '\0') {
      const char single[] = {byte, '\0'};
      handleUsbCommand(single);
      continue;
    }
    if (byte == '\n' || byte == '\r') {
      usbCommand[usbLength] = '\0';
      if (usbLength) handleUsbCommand(usbCommand);
      usbLength = 0;
    } else if (usbLength < sizeof(usbCommand) - 1) {
      usbCommand[usbLength++] = byte;
    } else {
      usbLength = 0;
    }
  }
}

void setup() {
  pinMode(kLed, OUTPUT);
  digitalWrite(kLed, HIGH);
  pinMode(kBootButton, INPUT_PULLUP);
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, kFCRx, kFCTx);
  prefs.begin("recorder", false);
  nextId = prefs.getUShort("next", 1);
  bootCount = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);
  wipeSeq = prefs.getUInt("wipeseq", 0);
  strlcpy(tzRule, prefs.getString("tz", kDefaultTz).c_str(), sizeof(tzRule));
  setenv("TZ", tzRule, 1);
  tzset();
  // The drive has to be registered before USB starts; it stays empty until the scan.
  const bool driveOk = startDrive();
  USB.productName("FPV Audio Recorder");
  USB.manufacturerName("FPV Recorder");
  USB.begin();
  temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  if (temperature_sensor_install(&config, &chipTemp) == ESP_OK)
    temperature_sensor_enable(chipTemp);
  recPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "rec");
  if (!recPart) { Serial.println("ERROR: rec partition missing (flash partitions.csv)"); return; }
  sectorCount = recPart->size / kSector;
  pcmRing = static_cast<int16_t *>(ps_malloc(kRingFrames * kFrameBytes));
  if (!pcmRing) { Serial.println("ERROR: PSRAM allocation failed"); return; }
  int err = 0;
  encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_AUDIO, &err);
  decoder = opus_decoder_create(kSampleRate, 1, &err);
  if (!encoder || !decoder) { Serial.println("ERROR: Opus initialization failed"); return; }
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kBitrate));
  opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
  opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1));
  opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(kComplexity));
  opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));
  const uint32_t scanMs = scanStorage();
  if (!startMicrophone()) { Serial.println("ERROR: microphone initialization failed"); return; }
  driveOnline();
  ready = true;
  Serial.printf("FLASH_LOG sectors=%lu recordings=%u scan_ms=%lu boot=%lu drive=%s\n",
                static_cast<unsigned long>(sectorCount), static_cast<unsigned>(recordings.size()),
                static_cast<unsigned long>(scanMs), static_cast<unsigned long>(bootCount),
                driveOk ? "OK" : "ERROR");
  Serial.println("RECORDER_READY 1");
  printTemperature();
  hostRefreshPending = false;  // the host enumerates the drive fresh at power-up
}

void loop() {
  if (!ready) { delay(1000); return; }
  consumeMsp();
  consumeUsb();
  refreshHostIfPending();
  const uint32_t now = millis();
  if (now - lastPollAt >= 200) {
    lastPollAt = now;
    requestMsp(boxIdsKnown ? kMspStatus : kMspBoxIDs);
    // FC clock: every second until known, then every 10 s to follow drift.
    if (now - lastRtcAt >= (clockSource == kTimeFc ? 10000u : 1000u)) {
      lastRtcAt = now;
      requestMsp(kMspRtc);
    }
  }
  if (recording && !manualRecording && now - lastStatusAt > 3000) {
    fcArmed = false;
    armedReplies = disarmedReplies = 0;
    stopRecording("MSP_TIMEOUT");
  }
  if (recording) {
    if (!encodeAvailable()) stopRecording("FLASH_ERROR");
    delay(1);
  } else {
    pollBootButton();
    delay(5);
  }
  if (millis() - lastTempAt >= 5000) {
    lastTempAt = millis();
    printTemperature();
  }
}
