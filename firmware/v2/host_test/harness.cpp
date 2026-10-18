// Host test: runs recorder.cpp against stubbed hardware, records PCM from a file into the
// simulated flash log, then dumps the virtual USB drive as a disk image.
// Usage: harness <pcm s16le 16k mono> <existing out dir> [wrap|wipe]
// Build (Homebrew opus and zlib):
//   clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -Wno-string-plus-int -Istubs \
//     -I/opt/homebrew/include -I/opt/homebrew/include/opus -L/opt/homebrew/lib -lopus -lz \
//     harness.cpp -o harness
// The out dir gets drive.img; mount it read-only with hdiutil to inspect.
#include "../flight_recorder_flash/recorder.cpp"
#include <fstream>

int hostLed = 1, hostButton = 1;
HostNotify hostNotify;
HardwareSerial Serial0, Serial1;
HostUSB USB;
std::vector<uint8_t> hostFlash(0x750000, 0xff);
esp_partition_t hostPart = {0x750000};
int64_t hostTimeOffsetUs = 0;
msc_read_cb hostMscRead = nullptr;
bool hostMediaPresent = false;

static std::vector<int16_t> source;
static size_t sourcePos = 0;

static void feedSeconds(double seconds) {
  const uint32_t frames = static_cast<uint32_t>(seconds * 50);
  for (uint32_t i = 0; i < frames; ++i) {
    while (ringHead - ringTail >= kRingFrames - 1) encodeAvailable();
    int16_t *dst = pcmRing + (ringHead % kRingFrames) * kFrameSamples;
    for (int s = 0; s < kFrameSamples; ++s) {
      dst[s] = source[sourcePos++ % source.size()];
    }
    ringHead = ringHead + 1;
    samplesCaptured += kFrameSamples;
    hostTimeOffsetUs += 20000;  // simulated real time
    if (i % 10 == 0 && !encodeAvailable()) { fprintf(stderr, "encode failed\n"); exit(1); }
  }
  encodeAvailable();
}

static void rtcReply(int64_t utcMs) {
  const time_t t = utcMs / 1000;
  struct tm u;
  gmtime_r(&t, &u);
  uint8_t p[9];
  p[0] = (u.tm_year + 1900) & 0xff; p[1] = (u.tm_year + 1900) >> 8;
  p[2] = u.tm_mon + 1; p[3] = u.tm_mday; p[4] = u.tm_hour; p[5] = u.tm_min; p[6] = u.tm_sec;
  p[7] = (utcMs % 1000) & 0xff; p[8] = (utcMs % 1000) >> 8;
  handleMspReply(kMspRtc, p, 9);
}

static void dumpImage(const std::string &path) {
  // Wait for the volume, then read every sector the way a host would (4 KB requests).
  std::ofstream out(path, std::ios::binary);
  std::vector<uint8_t> buf(4096);
  const uint32_t lastData = kDataStart + 2 * kSecPerClus +
      [] { uint32_t m = 0; for (auto &f : volFiles) m = std::max(m, f.firstCluster + f.clusters); return m; }() * kSecPerClus;
  for (uint32_t lba = 0; lba < kVolSectors; lba += 8) {
    if (lba < lastData) {
      if (hostMscRead(lba, 0, buf.data(), 4096) != 4096) { fprintf(stderr, "read failed at %u\n", lba); exit(1); }
    } else {
      std::fill(buf.begin(), buf.end(), 0);
    }
    out.write(reinterpret_cast<char *>(buf.data()), 4096);
  }
  fprintf(stderr, "image %s written, %zu files\n", path.c_str(), volFiles.size());
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  std::ifstream in2(argv[1], std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(in2)), {});
  source.resize(raw.size() / 2);
  memcpy(source.data(), raw.data(), source.size() * 2);
  const std::string outDir = argv[2];
  const bool wrap = argc > 3 && strcmp(argv[3], "wrap") == 0;
  setvbuf(stdout, nullptr, _IONBF, 0);

  setup();
  if (!ready) return 1;
  // A: no clock, 20 s.
  startRecording(false);
  feedSeconds(20);
  stopRecording("DISARM");
  // B: bench clock (CLOCK), 180 s. 2026-10-13 21:31:45.250 UTC. Sent through the USB
  // byte parser, so the single-letter legacy commands can't swallow it.
  USBSerial.input = "ZONE EST5EDT,M3.2.0,M11.1.0\nCLOCK 1791927105250\n";
  consumeUsb();
  startRecording(false);
  feedSeconds(180);
  stopRecording("DISARM");
  // C: no clock at start, FC clock arrives after 15 s (simulated new power-up).
  clockSource = kTimeNone;
  flightNo = 0;
  bootCount = 7;
  startRecording(false);
  feedSeconds(15);
  rtcReply(1791930000000LL);  // 2026-10-13 22:20:00 UTC
  feedSeconds(45);
  stopRecording("DISARM");
  if (wrap) {
    // D: 32 minutes, wraps the ring and overwrites everything before it plus its own start.
    startRecording(false);
    feedSeconds(32 * 60);
    stopRecording("DISARM");
  }
  handleUsbCommand("Q");
  dumpImage(outDir + (wrap ? "/drive-wrap.img" : "/drive.img"));
  // Seek test: random reads straight from the firmware, compared with the sequential image.
  std::ifstream img(outDir + (wrap ? "/drive-wrap.img" : "/drive.img"), std::ios::binary);
  std::vector<uint8_t> want(4096), got(4096);
  uint32_t lastLba = kDataStart;
  for (auto &f : volFiles) lastLba = std::max(lastLba, kDataStart + (f.firstCluster + f.clusters - 2) * kSecPerClus);
  srand(7);
  int bad = 0, maxDiff = 0;
  for (int i = 0; i < 300; ++i) {
    const uint32_t lba = (kDataStart + rand() % (lastLba - kDataStart)) & ~7u;
    img.seekg(static_cast<uint64_t>(lba) * 512);
    img.read(reinterpret_cast<char *>(want.data()), 4096);
    hostMscRead(lba, 0, got.data(), 4096);
    if (memcmp(want.data(), got.data(), 4096)) {
      ++bad;
      for (int k = 0; k + 1 < 4096; k += 2) {
        const int a = static_cast<int16_t>(want[k] | want[k + 1] << 8), b = static_cast<int16_t>(got[k] | got[k + 1] << 8);
        maxDiff = std::max(maxDiff, std::abs(a - b));
      }
    }
  }
  fprintf(stderr, "seek test: %d of 300 random 4 KB reads differ, max sample diff %d\n", bad, maxDiff);
  if (argc > 3 && strcmp(argv[3], "wipe") == 0) {
    // Format by holding BOOT: early release cancels, 10 s wipes.
    hostButton = 0;
    for (int i = 0; i < 300; ++i) { pollBootButton(); delay(10); }
    hostButton = 1;
    pollBootButton();
    fprintf(stderr, "after 3 s hold: %zu recordings\n", recordings.size());
    hostButton = 0;
    for (int i = 0; i < 1050; ++i) { pollBootButton(); delay(10); }
    hostButton = 1;
    pollBootButton();
    fprintf(stderr, "after 10.5 s hold: %zu recordings, %zu files\n", recordings.size(), volFiles.size());
    startRecording(false);
    feedSeconds(5);
    stopRecording("DISARM");
    // Reboot: the wipe point and boot counter persist in prefs.
    scanStorage();
    fprintf(stderr, "after new 5 s flight and rescan: %zu recordings (id %u, %.1f s)\n", recordings.size(),
            recordings.begin()->first, recordings.begin()->second.frames * 0.02f);
  }
  return 0;
}
