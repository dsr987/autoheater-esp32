/*
 * Autoheater ESP32
 * FIRMWARE_VERSION: 0.0.3
 * CHANGELOG_START
 * - Обновлён мобильный Web GUI: название проекта и версия сборки в шапке, индикаторы RSSI/override, полноэкранный портретный режим и увеличенные кнопки управления.
 * - Счётчики валидных кадров и CRC перенесены в диагностику; добавлен кольцевой журнал неизвестных состояний, ошибок, регистров и кадров с временными метками, копированием и TXT-экспортом.
 * - Состояния 0x04 (предположительно продувка после STOP) и 0x00 (предположительно ожидание) отмечены как гипотезы до повторной аппаратной проверки.
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
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
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
constexpr uint32_t FRAME_INTERBYTE_TIMEOUT_MS = 40;
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
constexpr uint8_t POLL_REGISTERS[] = {0x50, 0x0C, 0x0B, 0x20, 0x24, 0x16, 0x10, 0x01};

HardwareSerial HeaterSerial(1);
AsyncWebServer server(80);
AsyncWebSocket websocket("/ws");
Adafruit_SSD1306 display(128, 64, &Wire, -1);

struct HeaterState {
  uint8_t state = 0, step = 0, error = 0xFF;
  float nodeTemperature = NAN, voltage = NAN, panelTemperature = NAN;
  uint32_t fanRpm = 0, fuelDoses = 0, lastResponseMs = 0;
  bool stateReceived = false, errorReceived = false;
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

float panelPower = NAN, heaterPower = NAN, desiredPower = POWER_LEVELS[0];
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
constexpr size_t DIAGNOSTIC_LOG_CAPACITY = 64;
enum DiagnosticKind : uint8_t { DIAG_UNKNOWN_STATE, DIAG_UNKNOWN_ERROR, DIAG_UNKNOWN_REGISTER, DIAG_UNKNOWN_FRAME };
struct DiagnosticEntry {
  uint32_t atMs = 0;
  uint8_t kind = 0, code = 0, reg = 0xFF, sync = 0, command = 0;
  uint16_t length = 0;
};
DiagnosticEntry diagnosticLog[DIAGNOSTIC_LOG_CAPACITY];
size_t diagnosticHead = 0, diagnosticCount = 0;
uint32_t diagnosticDropped = 0;
portMUX_TYPE diagnosticMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastUnknownFrameSignature = 0, lastUnknownFrameAtMs = 0;
uint32_t lastUnknownRegisterSignature = 0, lastUnknownRegisterAtMs = 0;

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

bool isConfirmedHeaterState(uint8_t state) {
  switch (state) {
    case 0x20: case 0x24: case 0x25: case 0x27: case 0x2F: case 0x38: case 0x2E:
      return true;
    default: // 0x00 and 0x04 are observed after STOP, but remain hypotheses.
      return false;
  }
}
bool isKnownRegister(uint8_t reg) {
  switch (reg) {
    case 0x50: case 0x0C: case 0x0B: case 0x20: case 0x24: case 0x16: case 0x10: case 0x01:
      return true;
    default: return false;
  }
}
void addDiagnosticEntry(uint8_t kind,uint8_t code,uint8_t reg,uint8_t sync,uint8_t command,uint16_t length) {
  DiagnosticEntry entry;
  entry.atMs=millis();entry.kind=kind;entry.code=code;entry.reg=reg;entry.sync=sync;entry.command=command;entry.length=length;
  portENTER_CRITICAL(&diagnosticMux);
  diagnosticLog[diagnosticHead]=entry;
  diagnosticHead=(diagnosticHead+1)%DIAGNOSTIC_LOG_CAPACITY;
  if(diagnosticCount<DIAGNOSTIC_LOG_CAPACITY)++diagnosticCount;else++diagnosticDropped;
  portEXIT_CRITICAL(&diagnosticMux);
}
void incrementValidFrames(){portENTER_CRITICAL(&diagnosticMux);++validFrames;portEXIT_CRITICAL(&diagnosticMux);}
void incrementCrcErrors(){portENTER_CRITICAL(&diagnosticMux);++invalidFrames;portEXIT_CRITICAL(&diagnosticMux);}
void logUnknownFrame(const ParsedFrame &frame,uint8_t reg) {
  const uint32_t signature=(uint32_t(frame.sync)<<24)|(uint32_t(frame.command)<<16)|(uint32_t(reg)<<8)|frame.length;
  const uint32_t now=millis();
  if(signature==lastUnknownFrameSignature&&uint32_t(now-lastUnknownFrameAtMs)<1000)return;
  lastUnknownFrameSignature=signature;lastUnknownFrameAtMs=now;
  addDiagnosticEntry(DIAG_UNKNOWN_FRAME,0,reg,frame.sync,frame.command,frame.length);
}
void logUnknownRegister(uint8_t reg,const ParsedFrame &frame) {
  const uint32_t signature=(uint32_t(frame.sync)<<24)|(uint32_t(frame.command)<<16)|(uint32_t(reg)<<8)|frame.length;
  const uint32_t now=millis();
  if(signature==lastUnknownRegisterSignature&&uint32_t(now-lastUnknownRegisterAtMs)<1000)return;
  lastUnknownRegisterSignature=signature;lastUnknownRegisterAtMs=now;
  addDiagnosticEntry(DIAG_UNKNOWN_REGISTER,reg,reg,frame.sync,frame.command,frame.length);
}
String buildDiagnosticsReport() {
  DiagnosticEntry snapshot[DIAGNOSTIC_LOG_CAPACITY];
  size_t count=0;uint32_t dropped=0,valid=0,crcErrors=0;
  portENTER_CRITICAL(&diagnosticMux);
  count=diagnosticCount;dropped=diagnosticDropped;valid=validFrames;crcErrors=invalidFrames;
  const size_t first=(diagnosticHead+DIAGNOSTIC_LOG_CAPACITY-count)%DIAGNOSTIC_LOG_CAPACITY;
  for(size_t i=0;i<count;++i)snapshot[i]=diagnosticLog[(first+i)%DIAGNOSTIC_LOG_CAPACITY];
  portEXIT_CRITICAL(&diagnosticMux);
  String report;report.reserve(7500);
  report+="valid_frames=";report+=String(valid);report+="\n";
  report+="crc_errors=";report+=String(crcErrors);report+="\n";
  report+="log_overwritten=";report+=String(dropped);report+="\n";
  report+="timestamp_basis=milliseconds since ESP32 boot\n--- LOG ---\n";
  for(size_t i=0;i<count;++i){
    const DiagnosticEntry &entry=snapshot[i];
    const unsigned long ms=entry.atMs,h=ms/3600000UL,m=(ms/60000UL)%60UL,s=(ms/1000UL)%60UL,frac=ms%1000UL;
    char line[144];
    switch(entry.kind){
      case DIAG_UNKNOWN_STATE:snprintf(line,sizeof(line),"[%lu:%02lu:%02lu.%03lu] UNKNOWN_STATE register=0x%02X code=0x%02X",h,m,s,frac,entry.reg,entry.code);break;
      case DIAG_UNKNOWN_ERROR:snprintf(line,sizeof(line),"[%lu:%02lu:%02lu.%03lu] UNKNOWN_ERROR register=0x%02X code=0x%02X",h,m,s,frac,entry.reg,entry.code);break;
      case DIAG_UNKNOWN_REGISTER:snprintf(line,sizeof(line),"[%lu:%02lu:%02lu.%03lu] UNKNOWN_REGISTER code=0x%02X response_length=%u",h,m,s,frac,entry.code,entry.length);break;
      default:snprintf(line,sizeof(line),"[%lu:%02lu:%02lu.%03lu] UNKNOWN_FRAME sync=0x%02X command=0x%02X length=%u pending_register=0x%02X",h,m,s,frac,entry.sync,entry.command,entry.length,entry.reg);break;
    }
    report+=line;report+="\n";
  }
  if(!count)report+="Пока неизвестных или неподтверждённых кодов не обнаружено.\n";
  return report;
}

void handlePanelFrame(const ParsedFrame &frame) {
  lastPanelActivityMs=millis();bool recognized=false;
  if(frame.command==0x30&&frame.length==1){
    pendingRegister=frame.data[0];awaitingResponse=true;responseDeadlineMs=millis()+RESPONSE_TIMEOUT_MS;recognized=true;
    if(!isKnownRegister(pendingRegister))logUnknownRegister(pendingRegister,frame);
  }else if(frame.command==0x01&&frame.length==5&&frame.data[0]==0x01){
    const float incomingPower=readFloatLE(frame.data+1);if(!isfinite(incomingPower))return;
    panelPower=incomingPower;recognized=true;
    if(powerOverride&&fabsf(incomingPower-desiredPower)>0.5f)enqueueWriteFloat(0x01,desiredPower);
  }else if(frame.command==0x01&&frame.length==2&&(frame.data[0]==0x02||frame.data[0]==0x03)){recognized=true;}
  else if(frame.command==0x20&&frame.length==5&&(frame.data[0]==0x00||frame.data[0]==0x01)){
    if(frame.data[0]==0x00){const float t=readFloatLE(frame.data+1);if(isfinite(t))heater.panelTemperature=t;}
    recognized=true;
  }
  if(!recognized)logUnknownFrame(frame,0xFF);
}

void handleHeaterFrame(const ParsedFrame &frame) {
  if(frame.command!=0x00){logUnknownFrame(frame,pendingRegister);return;}
  if(!awaitingResponse||int32_t(millis()-responseDeadlineMs)>=0){logUnknownFrame(frame,0xFF);return;}
  if(frame.length!=1&&frame.length!=4){logUnknownFrame(frame,pendingRegister);clearPendingResponse();return;}
  const uint8_t *data=frame.data;bool accepted=false;
  switch(pendingRegister){
    case 0x0B:
      if(frame.length==1){
        if(data[0]!=0xFF&&(!heater.errorReceived||heater.error!=data[0]))addDiagnosticEntry(DIAG_UNKNOWN_ERROR,data[0],0x0B,frame.sync,frame.command,frame.length);
        heater.error=data[0];heater.errorReceived=true;accepted=true;
      }break;
    case 0x0C:if(frame.length==4){heater.step=uint8_t(readU32LE(data));accepted=true;}break;
    case 0x50:
      if(frame.length==1){
        if(!isConfirmedHeaterState(data[0])&&(!heater.stateReceived||heater.state!=data[0]))addDiagnosticEntry(DIAG_UNKNOWN_STATE,data[0],0x50,frame.sync,frame.command,frame.length);
        heater.state=data[0];heater.stateReceived=true;accepted=true;
      }break;
    case 0x20:{if(frame.length==4){const float v=readFloatLE(data);if(isfinite(v)){heater.nodeTemperature=v;accepted=true;}}break;}
    case 0x24:{if(frame.length==4){const float v=readFloatLE(data);if(isfinite(v)){heater.voltage=v;accepted=true;}}break;}
    case 0x10:if(frame.length==4){heater.fuelDoses=readU32LE(data);accepted=true;}break;
    case 0x16:if(frame.length==4){heater.fanRpm=readU32LE(data);accepted=true;}break;
    case 0x01:{if(frame.length==4){const float v=readFloatLE(data);if(isfinite(v)){heaterPower=v;accepted=true;}}break;}
    default:logUnknownRegister(pendingRegister,frame);break;
  }
  if(accepted){heater.lastResponseMs=millis();incrementValidFrames();}
  else if(isKnownRegister(pendingRegister))logUnknownFrame(frame,pendingRegister);
  clearPendingResponse();
}

void handleFrame(const ParsedFrame &frame) {
  if(!frame.crcOk){incrementCrcErrors();return;}
  if(frame.sync==0xA5){incrementValidFrames();handlePanelFrame(frame);}
  else if(frame.sync==0xA0){handleHeaterFrame(frame);}
  else logUnknownFrame(frame,0xFF);
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
  const size_t queued = (txHead + TX_QUEUE_SIZE - txTail) % TX_QUEUE_SIZE;
  if (queued + 6 >= TX_QUEUE_SIZE) return false;
  for (uint8_t i = 0; i < 3; ++i)
    if (!enqueueWriteByte(0x02, 0x00)) return false;
  for (uint8_t i = 0; i < 3; ++i)
    if (!enqueueWriteByte(0x03, 0x01)) return false;
  return true;
}
bool commandStop() {
  // Stop takes priority over queued start/power frames.
  txTail = txHead;
  bool result = true;
  for (uint8_t i = 0; i < 3; ++i)
    result = enqueueWriteByte(0x03, 0x00) && result;
  return result;
}
bool commandPower(int direction) {
  if (!heaterIsLive()) return false;
  const float current = powerOverride ? desiredPower :
      (isfinite(heaterPower) && heaterPower >= 500.0f ? heaterPower :
       (isfinite(panelPower) && panelPower >= 500.0f ? panelPower : POWER_LEVELS[0]));
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
    case 0x00: return "Ожидание (гипотеза)";
    case 0x04: return "Продувка после остановки (гипотеза)";
    case 0x20: return "Свеча / прогрев";
    case 0x24: return "Розжиг";
    case 0x25: return "Пламя поймано";
    case 0x27: return "Набор мощности";
    case 0x2F: return "Выход на режим";
    case 0x38: return "Переход";
    case 0x2E: return "Установившееся горение";
    default: return "Неизвестное состояние";
  }
}

int getStrongestApClientRssi() {
  wifi_sta_list_t stations={};
  if(esp_wifi_ap_get_sta_list(&stations)!=ESP_OK||stations.num==0)return -127;
  int strongest=-127;
  for(int i=0;i<stations.num;++i)if(stations.sta[i].rssi>strongest)strongest=stations.sta[i].rssi;
  return strongest;
}
void broadcastState() {
  if (!websocket.count()) return;
  const bool live = heaterIsLive();
  const float shownPower = powerOverride ? desiredPower :
      (isfinite(heaterPower) ? heaterPower : panelPower);
  char json[384];
  char heaterPowerJson[24];
  if (isfinite(heaterPower)) snprintf(heaterPowerJson, sizeof(heaterPowerJson), "%.0f", heaterPower);
  else strcpy(heaterPowerJson, "null");
  const int wifiRssi=getStrongestApClientRssi();
  snprintf(json,sizeof(json),
    "{\"st\":%u,\"step\":%u,\"err\":\"%02X\",\"t\":%.1f,\"v\":%.2f,"
    "\"fan\":%lu,\"dose\":%lu,\"tp\":%.1f,\"pw\":%.0f,\"hp\":%s,"
    "\"override\":%s,\"live\":%s,\"rssi\":%d}",
    heater.state,heater.step,heater.error,
    isfinite(heater.nodeTemperature)?heater.nodeTemperature:0.0f,
    isfinite(heater.voltage)?heater.voltage:0.0f,
    (unsigned long)heater.fanRpm,(unsigned long)heater.fuelDoses,
    isfinite(heater.panelTemperature)?heater.panelTemperature:0.0f,
    isfinite(shownPower)?shownPower:0.0f,heaterPowerJson,
    powerOverride?"true":"false",live?"true":"false",wifiRssi);
  websocket.textAll(json);
}

void drawOled() {
  if (!oledReady) return;
  const float shownPower = powerOverride ? desiredPower :
      (isfinite(heaterPower) ? heaterPower : panelPower);
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
  server.on("/diagnostics", HTTP_GET, [](AsyncWebServerRequest *request) {
    String report=buildDiagnosticsReport();
    AsyncWebServerResponse *response=request->beginResponse(200,"text/plain; charset=utf-8",report);
    response->addHeader("Cache-Control","no-store");
    request->send(response);
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
  Serial.println("\nAutoheater ESP32 v0.0.2");
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setClock(400000);
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
