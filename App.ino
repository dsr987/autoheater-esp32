/*
 * Autoheater ESP32
 * FIRMWARE_VERSION: 0.0.1
 * CHANGELOG_START
 * - Первая рабочая реализация протокола A5/A0: телеметрия, управление и веб-интерфейс.
 * - Встроены CRC-8/MAXIM, контроль тайм-аутов ответов и тихий опрос после остановки штатного пульта.
 * CHANGELOG_END
 *
 * Единственный исходник прошивки. index.html в корне репозитория встраивается
 * генератором tools/prepare_sketch.py в web_page.h перед компиляцией.
 *
 * UART: RX GPIO16 через делитель 10k/20k; TX GPIO17 только через 1N4148.
 * Общая GND. Никогда не подключать линию 5 В напрямую к GPIO ESP32.
 */
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>
#include <string.h>
#include "web_page.h"

namespace {
constexpr char AP_SSID[] = "Autoheater";
constexpr char AP_PASSWORD[] = "12345678";  // Заменить перед эксплуатацией.
constexpr char OTA_USERNAME[] = "admin";    // Заменить перед эксплуатацией.
constexpr char OTA_PASSWORD[] = "admin";    // Заменить перед эксплуатацией.
constexpr int PIN_HEATER_RX = 16, PIN_HEATER_TX = 17;
constexpr int PIN_OLED_SDA = 21, PIN_OLED_SCL = 22;
constexpr uint32_t HEATER_BAUD = 9600;
constexpr uint32_t CONSOLE_BAUD = 115200;
constexpr uint32_t FRAME_INTERBYTE_TIMEOUT_MS = 4;
constexpr uint32_t TX_IDLE_GAP_MS = 25;
constexpr uint32_t TX_REPEAT_GAP_MS = 21;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 80;
constexpr uint32_t PANEL_SILENCE_MS = 2500;
constexpr uint32_t SILENT_POLL_INTERVAL_MS = 250;
constexpr uint32_t TELEMETRY_STALE_MS = 5000;
constexpr uint32_t UI_UPDATE_INTERVAL_MS = 500;
constexpr uint32_t OLED_UPDATE_INTERVAL_MS = 1000;
constexpr size_t MAX_FRAME_DATA = 32;
constexpr size_t TX_QUEUE_SIZE = 32;
constexpr float POWER_LEVELS[] = {1000.0f, 1900.0f, 2800.0f, 3700.0f, 4600.0f};
constexpr uint8_t POLL_REGISTERS[] = {0x50, 0x0C, 0x0B, 0x20, 0x24, 0x16, 0x10};

HardwareSerial HeaterSerial(1);
AsyncWebServer server(80);
AsyncWebSocket websocket("/ws");
Adafruit_SSD1306 display(128, 64, &Wire, -1);

struct HeaterState {
  uint8_t state = 0, step = 0, error = 0xFF;
  float nodeTemperature = NAN, voltage = NAN, panelTemperature = NAN;
  uint32_t fanRpm = 0, fuelDoses = 0, lastResponseMs = 0;
} heater;

struct ParsedFrame {
  uint8_t sync = 0, command = 0;
  uint16_t length = 0;
  uint8_t data[MAX_FRAME_DATA] = {};
  bool crcOk = false;
};

uint8_t crc8Maxim(const uint8_t *data, size_t length) {
  uint8_t crc = 0;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? uint8_t((crc >> 1) ^ 0x8C) : uint8_t(crc >> 1);
  }
  return crc;
}

class FrameParser {
 public:
  bool feed(uint8_t byte, uint32_t now, ParsedFrame &frame) {
    if (count_ && uint32_t(now - lastByteMs_) > FRAME_INTERBYTE_TIMEOUT_MS) reset();
    if (!count_) {
      if (byte != 0xA5 && byte != 0xA0) return false;
      buffer_[count_++] = byte;
      lastByteMs_ = now;
      return false;
    }
    if (count_ >= sizeof(buffer_)) {
      reset();
      if (byte == 0xA5 || byte == 0xA0) {
        buffer_[count_++] = byte;
        lastByteMs_ = now;
      }
      return false;
    }
    buffer_[count_++] = byte;
    lastByteMs_ = now;
    if (count_ < 4) return false;

    const uint16_t dataLength = (uint16_t(buffer_[2]) << 8) | buffer_[3];
    if (dataLength > MAX_FRAME_DATA) {
      reset();
      if (byte == 0xA5 || byte == 0xA0) {
        buffer_[count_++] = byte;
        lastByteMs_ = now;
      }
      return false;
    }
    const size_t frameLength = 4 + dataLength + 1;
    if (count_ < frameLength) return false;

    frame.sync = buffer_[0];
    frame.command = buffer_[1];
    frame.length = dataLength;
    if (dataLength) memcpy(frame.data, buffer_ + 4, dataLength);
    frame.crcOk = crc8Maxim(buffer_, frameLength - 1) == buffer_[frameLength - 1];
    reset();
    return true;
  }

 private:
  uint8_t buffer_[4 + MAX_FRAME_DATA + 1] = {};
  size_t count_ = 0;
  uint32_t lastByteMs_ = 0;
  void reset() { count_ = 0; lastByteMs_ = 0; }
} parser;

struct TxFrame { uint8_t bytes[10] = {}; uint8_t length = 0; };
TxFrame txQueue[TX_QUEUE_SIZE];
size_t txHead = 0, txTail = 0;

float panelPower = NAN, desiredPower = POWER_LEVELS[0];
bool powerOverride = false, oledReady = false;
uint8_t pendingRegister = 0xFF;
bool awaitingResponse = false;
uint32_t responseDeadlineMs = 0;
uint8_t echoBytes[10] = {};
size_t echoLength = 0, echoIndex = 0;
uint32_t echoDeadlineMs = 0;
uint32_t lastRxMs = 0, lastPanelActivityMs = 0, nextTxAtMs = 0;
uint32_t lastSilentPollMs = 0, lastUiUpdateMs = 0, lastOledUpdateMs = 0;
uint32_t validFrames = 0, invalidFrames = 0;
uint8_t silentPollIndex = 0;

uint32_t readU32LE(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
float readFloatLE(const uint8_t *p) {
  static_assert(sizeof(float) == 4, "Protocol requires float32");
  float value = NAN;
  memcpy(&value, p, sizeof(value));
  return value;
}
bool queueIsEmpty() { return txHead == txTail; }
bool queueIsFull() { return ((txHead + 1) % TX_QUEUE_SIZE) == txTail; }
bool enqueueFrame(const uint8_t *bytes, uint8_t length) {
  if (queueIsFull() || !length || length > sizeof(txQueue[0].bytes)) return false;
  memcpy(txQueue[txHead].bytes, bytes, length);
  txQueue[txHead].length = length;
  txHead = (txHead + 1) % TX_QUEUE_SIZE;
  return true;
}
bool enqueueWriteByte(uint8_t reg, uint8_t value) {
  uint8_t frame[7] = {0xA5, 0x01, 0x00, 0x02, reg, value, 0};
  frame[6] = crc8Maxim(frame, 6);
  return enqueueFrame(frame, sizeof(frame));
}
bool enqueueWriteFloat(uint8_t reg, float value) {
  if (!isfinite(value)) return false;
  uint8_t frame[10] = {0xA5, 0x01, 0x00, 0x05, reg, 0, 0, 0, 0, 0};
  memcpy(frame + 5, &value, sizeof(value));
  frame[9] = crc8Maxim(frame, 9);
  return enqueueFrame(frame, sizeof(frame));
}
bool enqueueRead(uint8_t reg) {
  uint8_t frame[6] = {0xA5, 0x30, 0x00, 0x01, reg, 0};
  frame[5] = crc8Maxim(frame, 5);
  return enqueueFrame(frame, sizeof(frame));
}
bool heaterIsLive() {
  return heater.lastResponseMs &&
         uint32_t(millis() - heater.lastResponseMs) < TELEMETRY_STALE_MS;
}
void clearPendingResponse() {
  awaitingResponse = false;
  pendingRegister = 0xFF;
  responseDeadlineMs = 0;
}
void expirePendingResponse(uint32_t now) {
  if (awaitingResponse && int32_t(now - responseDeadlineMs) >= 0)
    clearPendingResponse();
}

void handlePanelFrame(const ParsedFrame &frame) {
  lastPanelActivityMs = millis();
  if (frame.command == 0x30 && frame.length == 1) {
    pendingRegister = frame.data[0];
    awaitingResponse = true;
    responseDeadlineMs = millis() + RESPONSE_TIMEOUT_MS;
  } else if (frame.command == 0x01 && frame.length == 5 && frame.data[0] == 0x01) {
    const float incomingPower = readFloatLE(frame.data + 1);
    if (!isfinite(incomingPower)) return;
    panelPower = incomingPower;
    if (powerOverride && fabsf(incomingPower - desiredPower) > 0.5f)
      enqueueWriteFloat(0x01, desiredPower);
  } else if (frame.command == 0x20 && frame.length == 5 && frame.data[0] == 0x00) {
    const float temperature = readFloatLE(frame.data + 1);
    if (isfinite(temperature)) heater.panelTemperature = temperature;
  }
}

void handleHeaterFrame(const ParsedFrame &frame) {
  if (frame.command != 0x00 || (frame.length != 1 && frame.length != 4)) return;
  if (!awaitingResponse || int32_t(millis() - responseDeadlineMs) >= 0) return;

  const uint8_t *data = frame.data;
  bool accepted = false;
  switch (pendingRegister) {
    case 0x0B:
      if (frame.length == 1) { heater.error = data[0]; accepted = true; }
      break;
    case 0x0C:
      if (frame.length == 4) { heater.step = uint8_t(readU32LE(data)); accepted = true; }
      break;
    case 0x50:
      if (frame.length == 1) { heater.state = data[0]; accepted = true; }
      break;
    case 0x20: {
      if (frame.length == 4) {
        const float value = readFloatLE(data);
        if (isfinite(value)) { heater.nodeTemperature = value; accepted = true; }
      }
      break;
    }
    case 0x24: {
      if (frame.length == 4) {
        const float value = readFloatLE(data);
        if (isfinite(value)) { heater.voltage = value; accepted = true; }
      }
      break;
    }
    case 0x10:
      if (frame.length == 4) { heater.fuelDoses = readU32LE(data); accepted = true; }
      break;
    case 0x16:
      if (frame.length == 4) { heater.fanRpm = readU32LE(data); accepted = true; }
      break;
    default: break;
  }
  if (accepted) {
    heater.lastResponseMs = millis();
    ++validFrames;
  }
  clearPendingResponse();
}

void handleFrame(const ParsedFrame &frame) {
  if (!frame.crcOk) { ++invalidFrames; return; }
  if (frame.sync == 0xA5) {
    ++validFrames;
    handlePanelFrame(frame);
  } else if (frame.sync == 0xA0) {
    handleHeaterFrame(frame);
  }
}

bool consumeEcho(uint8_t byte, uint32_t now) {
  if (!echoLength) return false;
  if (int32_t(now - echoDeadlineMs) > 0) {
    echoLength = 0;
    echoIndex = 0;
    return false;
  }
  if (byte == echoBytes[echoIndex]) {
    if (++echoIndex >= echoLength) { echoLength = 0; echoIndex = 0; }
    return true;
  }
  echoLength = 0;
  echoIndex = 0;
  return false;
}

void pumpRx() {
  while (HeaterSerial.available() > 0) {
    const int incoming = HeaterSerial.read();
    if (incoming < 0) continue;
    const uint32_t now = millis();
    if (consumeEcho(uint8_t(incoming), now)) continue;
    lastRxMs = now;
    ParsedFrame frame;
    if (parser.feed(uint8_t(incoming), now, frame)) handleFrame(frame);
  }
}

void injectTask() {
  if (queueIsEmpty()) return;
  const uint32_t now = millis();
  expirePendingResponse(now);
  if (awaitingResponse) return;
  if (int32_t(now - nextTxAtMs) < 0) return;
  if (uint32_t(now - lastRxMs) < TX_IDLE_GAP_MS) return;

  TxFrame &frame = txQueue[txTail];
  memcpy(echoBytes, frame.bytes, frame.length);
  echoLength = frame.length;
  echoIndex = 0;
  echoDeadlineMs = now + 30;
  HeaterSerial.write(frame.bytes, frame.length);
  HeaterSerial.flush();

  if (frame.bytes[1] == 0x30 && frame.length == 6) {
    pendingRegister = frame.bytes[4];
    awaitingResponse = true;
    responseDeadlineMs = millis() + RESPONSE_TIMEOUT_MS;
  }
  txTail = (txTail + 1) % TX_QUEUE_SIZE;
  nextTxAtMs = millis() + TX_REPEAT_GAP_MS;
}

bool commandStart() {
  if (!heaterIsLive()) return false;
  const size_t queued = (txHead + TX_QUEUE_SIZE - txTail) % TX_QUEUE_SIZE;
  if (queued + 6 >= TX_QUEUE_SIZE) return false;
  for (uint8_t i = 0; i < 3; ++i)
    if (!enqueueWriteByte(0x02, 0x00)) return false;
  for (uint8_t i = 0; i < 3; ++i)
    if (!enqueueWriteByte(0x03, 0x01)) return false;
  return true;
}
bool commandStop() {
  bool result = true;
  for (uint8_t i = 0; i < 3; ++i)
    result = enqueueWriteByte(0x03, 0x00) && result;
  return result;
}
bool commandPower(int direction) {
  if (!heaterIsLive()) return false;
  const float current = powerOverride ? desiredPower :
      (isfinite(panelPower) && panelPower >= 500.0f ? panelPower : POWER_LEVELS[0]);
  size_t closest = 0;
  float smallestDifference = INFINITY;
  for (size_t i = 0; i < 5; ++i) {
    const float difference = fabsf(POWER_LEVELS[i] - current);
    if (difference < smallestDifference) { smallestDifference = difference; closest = i; }
  }
  const int selected = constrain(int(closest) + direction, 0, 4);
  desiredPower = POWER_LEVELS[selected];
  powerOverride = true;
  return enqueueWriteFloat(0x01, desiredPower);
}

void silentPollTask(uint32_t now) {
  if (uint32_t(now - lastPanelActivityMs) < PANEL_SILENCE_MS) return;
  if (awaitingResponse || !queueIsEmpty()) return;
  if (uint32_t(now - lastSilentPollMs) < SILENT_POLL_INTERVAL_MS) return;
  if (enqueueRead(POLL_REGISTERS[silentPollIndex])) {
    silentPollIndex = (silentPollIndex + 1) %
        (sizeof(POLL_REGISTERS) / sizeof(POLL_REGISTERS[0]));
    lastSilentPollMs = now;
  }
}

const char *stateName(uint8_t state) {
  switch (state) {
    case 0x20: return "Свеча / прогрев";
    case 0x24: return "Розжиг";
    case 0x25: return "Пламя поймано";
    case 0x27: return "Набор мощности";
    case 0x2F: return "Выход на режим";
    case 0x2E: return "Установившееся горение";
    default: return "Неизвестное состояние";
  }
}

void broadcastState() {
  if (!websocket.count()) return;
  const bool live = heaterIsLive();
  const float shownPower = powerOverride ? desiredPower : panelPower;
  char json[360];
  snprintf(json, sizeof(json),
    "{\"st\":%u,\"step\":%u,\"err\":\"%02X\",\"t\":%.1f,\"v\":%.2f,"
    "\"fan\":%lu,\"dose\":%lu,\"tp\":%.1f,\"pw\":%.0f,"
    "\"override\":%s,\"live\":%s,\"valid\":%lu,\"invalid\":%lu}",
    heater.state, heater.step, heater.error,
    isfinite(heater.nodeTemperature) ? heater.nodeTemperature : 0.0f,
    isfinite(heater.voltage) ? heater.voltage : 0.0f,
    (unsigned long)heater.fanRpm, (unsigned long)heater.fuelDoses,
    isfinite(heater.panelTemperature) ? heater.panelTemperature : 0.0f,
    isfinite(shownPower) ? shownPower : 0.0f,
    powerOverride ? "true" : "false", live ? "true" : "false",
    (unsigned long)validFrames, (unsigned long)invalidFrames);
  websocket.textAll(json);
}

void drawOled() {
  if (!oledReady) return;
  const float shownPower = powerOverride ? desiredPower : panelPower;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AUTOHEATER ESP32");
  display.printf("Bus: %s CRC:%lu/%lu\n", heaterIsLive() ? "LIVE" : "WAIT",
                 (unsigned long)validFrames, (unsigned long)invalidFrames);
  display.printf("State: %s\n", stateName(heater.state));
  display.printf("Temp: %.1f C\n", isfinite(heater.nodeTemperature) ? heater.nodeTemperature : 0.0f);
  display.printf("Power: %.0f W\n", isfinite(shownPower) ? shownPower : 0.0f);
  display.printf("Fan: %lu RPM\n", (unsigned long)heater.fanRpm);
  display.printf("Voltage: %.2f V\n", isfinite(heater.voltage) ? heater.voltage : 0.0f);
  display.display();
}

void setupWeb() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("[ERROR] Wi-Fi access point failed");
    return;
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", PAGE_HTML);
  });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
  });
  websocket.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType type,
                       void *arg, uint8_t *data, size_t length) {
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
    if (!info->final || info->index != 0 || info->len != length ||
        info->opcode != WS_TEXT || !length || length > 16) return;
    char command[17] = {};
    memcpy(command, data, length);
    if (strcmp(command, "start") == 0) {
      if (!commandStart()) Serial.println("[WARN] Start rejected: heater offline or TX queue full");
    } else if (strcmp(command, "stop") == 0) {
      if (!commandStop()) Serial.println("[WARN] Stop queue full");
    } else if (strcmp(command, "pup") == 0) {
      if (!commandPower(+1)) Serial.println("[WARN] Power command rejected");
    } else if (strcmp(command, "pdown") == 0) {
      if (!commandPower(-1)) Serial.println("[WARN] Power command rejected");
    }
  });
  server.addHandler(&websocket);
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();
  Serial.printf("[WiFi] AP %s, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}
}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(200);
  Serial.println("\nAutoheater ESP32 v0.0.1");
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledReady) Serial.println("[OLED] Display not found at 0x3C");

  HeaterSerial.setRxBufferSize(1024);
  HeaterSerial.begin(HEATER_BAUD, SERIAL_8N1, PIN_HEATER_RX, PIN_HEATER_TX);

  const uint32_t now = millis();
  lastRxMs = now;
  lastPanelActivityMs = now;
  lastSilentPollMs = now - SILENT_POLL_INTERVAL_MS;
  setupWeb();
}

void loop() {
  pumpRx();
  const uint32_t now = millis();
  expirePendingResponse(now);
  silentPollTask(now);
  injectTask();

  if (uint32_t(now - lastUiUpdateMs) >= UI_UPDATE_INTERVAL_MS) {
    lastUiUpdateMs = now;
    broadcastState();
  }
  if (uint32_t(now - lastOledUpdateMs) >= OLED_UPDATE_INTERVAL_MS) {
    lastOledUpdateMs = now;
    drawOled();
  }
  websocket.cleanupClients();
  ElegantOTA.loop();
}
