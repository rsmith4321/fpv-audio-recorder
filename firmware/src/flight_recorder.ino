#include <ESP_I2S.h>
#include <FS.h>
#include <SD.h>
#include "driver/temperature_sensor.h"

// XIAO ESP32S3 Sense: PDM microphone and microSD on the Sense expansion board.
// FC UART wiring is pending identification of the actual flight controller.
// D6/GPIO43 TX -> FC RX; D7/GPIO44 RX <- FC TX; share GND.
static constexpr uint8_t kMicClock = 42;
static constexpr uint8_t kMicData = 41;
static constexpr uint8_t kSDChipSelect = 21;
static constexpr uint8_t kFCRx = 44;
static constexpr uint8_t kFCTx = 43;
static constexpr uint32_t kSampleRate = 16000;
static constexpr uint32_t kByteRate = kSampleRate * 2;
static constexpr uint8_t kMspStatus = 101;
static constexpr uint8_t kMspBoxIDs = 119;

static I2SClass microphone;
static File wavFile;
static temperature_sensor_handle_t chipTemp = nullptr;
static char currentName[20] = {};
static char latestName[20] = {};
static uint32_t audioBytes = 0;
static uint32_t lastHeaderAt = 0;
static uint32_t lastTempAt = 0;
static uint32_t lastPollAt = 0;
static uint32_t lastStatusAt = 0;
static bool recording = false;
static bool manualRecording = false;
static bool fcArmed = false;
static bool boxIdsKnown = false;
static bool ready = false;
static uint8_t armBoxIndex = 0xff;
static uint8_t armedReplies = 0;
static uint8_t disarmedReplies = 0;
static char usbCommand[40] = {};
static uint8_t usbLength = 0;

static void put16(uint8_t *bytes, unsigned offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

static void put32(uint8_t *bytes, unsigned offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

static bool updateWavHeader() {
  if (!wavFile || !wavFile.seek(0)) return false;
  uint8_t header[44] = {};
  memcpy(header, "RIFF", 4);
  put32(header, 4, audioBytes + 36);
  memcpy(header + 8, "WAVEfmt ", 8);
  put32(header, 16, 16);
  put16(header, 20, 1);
  put16(header, 22, 1);
  put32(header, 24, kSampleRate);
  put32(header, 28, kByteRate);
  put16(header, 32, 2);
  put16(header, 34, 16);
  memcpy(header + 36, "data", 4);
  put32(header, 40, audioBytes);
  if (wavFile.write(header, sizeof(header)) != sizeof(header)) return false;
  if (!wavFile.seek(audioBytes + 44)) return false;
  wavFile.flush();
  lastHeaderAt = millis();
  return true;
}

static void stopRecording(const char *reason) {
  if (!recording) return;
  const bool headerOK = updateWavHeader();
  wavFile.close();
  recording = false;
  manualRecording = false;
  Serial.printf("STOP %s %s %lu %s\n", currentName, reason,
                static_cast<unsigned long>(audioBytes), headerOK ? "OK" : "HEADER_ERROR");
  if (headerOK) snprintf(latestName, sizeof(latestName), "%s", currentName);
  currentName[0] = '\0';
}

static bool startRecording(bool manual) {
  if (recording) return false;
  char name[20];
  bool freeName = false;
  for (unsigned int n = 1; n <= 9999; ++n) {
    snprintf(name, sizeof(name), "/REC%04u.WAV", n);
    if (!SD.exists(name)) { freeName = true; break; }
  }
  if (!freeName) { Serial.println("ERROR: recording names exhausted"); return false; }
  wavFile = SD.open(name, FILE_WRITE);
  if (!wavFile) { Serial.println("ERROR: cannot open microSD recording"); return false; }
  audioBytes = 0;
  snprintf(currentName, sizeof(currentName), "%s", name);
  if (!updateWavHeader()) {
    wavFile.close();
    SD.remove(name);
    currentName[0] = '\0';
    Serial.println("ERROR: cannot write WAV header");
    return false;
  }
  recording = true;
  manualRecording = manual;
  Serial.printf("START %s %s\n", name, manual ? "USB" : "ARM");
  return true;
}

static float chipTemperature() {
  float celsius = NAN;
  if (chipTemp != nullptr) temperature_sensor_get_celsius(chipTemp, &celsius);
  return celsius;
}

static void printTemperature() {
  const float celsius = chipTemperature();
  if (isnan(celsius)) Serial.println("chip_temp_c unavailable");
  else Serial.printf("chip_temp_c %.1f\n", celsius);
}

static void requestMsp(uint8_t command) {
  const uint8_t packet[] = {'$', 'M', '<', 0, command, command};
  Serial1.write(packet, sizeof(packet));
}

static void handleMspReply(uint8_t command, const uint8_t *payload, uint8_t length) {
  if (command == kMspBoxIDs) {
    armBoxIndex = 0xff;
    for (uint8_t i = 0; i < length; ++i) {
      if (payload[i] == 0) { armBoxIndex = i; break; } // ARM permanent ID
    }
    boxIdsKnown = armBoxIndex < 32;
    Serial.printf("MSP_ARM_BOX %s\n", boxIdsKnown ? "FOUND" : "UNAVAILABLE");
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

static void findLatestRecording() {
  latestName[0] = '\0';
  unsigned int highest = 0;
  File root = SD.open("/");
  while (File entry = root.openNextFile()) {
    String name = entry.name();
    if (name.startsWith("/")) name.remove(0, 1);
    unsigned int n = 0;
    if (!entry.isDirectory() && name.length() == 11 &&
        name.startsWith("REC") && name.endsWith(".WAV") &&
        sscanf(name.c_str(), "REC%4u.WAV", &n) == 1 && n > highest && n <= 9999) {
      highest = n;
    }
    entry.close();
  }
  root.close();
  if (highest) snprintf(latestName, sizeof(latestName), "/REC%04u.WAV", highest);
}

static void sendFile(const char *name) {
  if (recording) { Serial.println("ERROR: stop recording before import"); return; }
  File file = SD.open(name, FILE_READ);
  if (!file || file.isDirectory()) { Serial.println("ERROR: recording not found"); return; }
  Serial.printf("BEGIN %lu %s\n", static_cast<unsigned long>(file.size()), name);
  uint8_t buffer[512];
  while (file.available()) {
    const size_t count = file.read(buffer, sizeof(buffer));
    if (count == 0) break;
    Serial.write(buffer, count);
    Serial.flush();
  }
  file.close();
  Serial.flush();
}

static void handleUsbCommand(const char *command) {
  if (strcmp(command, "I") == 0) {
    Serial.println("RECORDER 1 XIAO_ESP32S3_SENSE 16000 MONO16");
  } else if (strcmp(command, "T") == 0) {
    printTemperature();
  } else if (strcmp(command, "L") == 0) {
    if (recording) { Serial.println("ERROR: stop recording before listing files"); return; }
    Serial.println("LIST_BEGIN");
    File root = SD.open("/");
    while (File entry = root.openNextFile()) {
      String name = entry.name();
      if (name.startsWith("/")) name.remove(0, 1);
      if (!entry.isDirectory() && name.startsWith("REC") && name.endsWith(".WAV"))
        Serial.printf("FILE /%s %lu\n", name.c_str(), static_cast<unsigned long>(entry.size()));
      entry.close();
    }
    root.close();
    Serial.println("LIST_END");
  } else if (strcmp(command, "D") == 0) {
    if (!latestName[0]) findLatestRecording();
    if (!latestName[0]) Serial.println("ERROR: no recordings found");
    else sendFile(latestName);
  } else if (command[0] == 'G' && command[1] == '/') {
    const char *name = command + 1;
    if (strlen(name) == 12 && strncmp(name, "/REC", 4) == 0 &&
        strcmp(name + 8, ".WAV") == 0 &&
        isdigit(name[4]) && isdigit(name[5]) && isdigit(name[6]) && isdigit(name[7]))
      sendFile(name);
    else Serial.println("ERROR: invalid recording name");
  } else if (strcmp(command, "S") == 0) {
    if (fcArmed) Serial.println("ERROR: FC already armed");
    else startRecording(true);
  } else if (strcmp(command, "E") == 0) {
    if (manualRecording) stopRecording("USB");
  }
}

static void consumeUsb() {
  while (Serial.available()) {
    const char byte = static_cast<char>(Serial.read());
    // Single-character legacy commands are supported for Easy Eject's initial client.
    if (usbLength == 0 && (byte == 'D' || byte == 'I' || byte == 'T' || byte == 'L' || byte == 'S' || byte == 'E')) {
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
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, kFCRx, kFCTx);
  temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  if (temperature_sensor_install(&config, &chipTemp) == ESP_OK)
    temperature_sensor_enable(chipTemp);
  microphone.setPinsPdmRx(kMicClock, kMicData);
  if (!microphone.begin(I2S_MODE_PDM_RX, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("ERROR: microphone initialization failed");
    return;
  }
  if (!SD.begin(kSDChipSelect)) {
    Serial.println("ERROR: microSD initialization failed");
    return;
  }
  findLatestRecording();
  ready = true;
  Serial.println("RECORDER_READY 1");
  printTemperature();
}

void loop() {
  if (!ready) { delay(1000); return; }
  consumeMsp();
  consumeUsb();
  const uint32_t now = millis();
  if (now - lastPollAt >= 200) {
    lastPollAt = now;
    requestMsp(boxIdsKnown ? kMspStatus : kMspBoxIDs);
  }
  if (recording && !manualRecording && now - lastStatusAt > 3000) {
    fcArmed = false;
    armedReplies = disarmedReplies = 0;
    stopRecording("MSP_TIMEOUT");
  }
  if (recording) {
    uint8_t samples[512];
    const size_t count = microphone.readBytes(reinterpret_cast<char *>(samples), sizeof(samples));
    if (count == 0 || wavFile.write(samples, count) != count) {
      stopRecording("SD_ERROR");
    } else {
      audioBytes += count;
      if (millis() - lastHeaderAt >= 1000 && !updateWavHeader()) stopRecording("HEADER_ERROR");
    }
  } else {
    delay(5);
  }
  if (millis() - lastTempAt >= 5000) {
    lastTempAt = millis();
    printTemperature();
  }
}
