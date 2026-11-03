/*
 * Autoheater ESP32 — heater protocol implementation
 * Protocol source of truth: docs/HEATER_PROTOCOL.md
 * RX GPIO16 via 10k/20k divider; TX GPIO17 through 1N4148 diode.
 * Common GND. Never connect a 5 V signal directly to an ESP32 GPIO.
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

namespace {
constexpr char AP_SSID[] = "Autoheater";
constexpr char AP_PASSWORD[] = "12345678";
constexpr char OTA_USERNAME[] = "admin";
constexpr char OTA_PASSWORD[] = "admin";
constexpr int PIN_HEATER_RX = 16, PIN_HEATER_TX = 17;
constexpr int PIN_OLED_SDA = 21, PIN_OLED_SCL = 22;
constexpr uint32_t CONSOLE_BAUD = 115200, HEATER_BAUD = 9600;
constexpr uint32_t TX_IDLE_GAP_MS = 15, TX_REPEAT_GAP_MS = 21;
constexpr uint32_t TELEMETRY_STALE_MS = 5000, SILENT_POLL_INTERVAL_MS = 2000;
constexpr size_t MAX_FRAME_DATA = 32, TX_QUEUE_SIZE = 32;
constexpr float POWER_LEVELS[] = {1000, 1900, 2800, 3700, 4600};

HardwareSerial HeaterSerial(1);
AsyncWebServer server(80);
AsyncWebSocket websocket("/ws");
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledReady = false;

struct HeaterState {
  uint8_t state = 0, step = 0, error = 0xFF;
  float nodeTemp = NAN, voltage = NAN, panelTemp = NAN;
  uint32_t fanRpm = 0, doses = 0, lastSeenMs = 0;
} heater;
float panelPower = NAN, desiredPower = 1000;
bool powerOverride = false, busEverSeen = false;
uint8_t pendingRegister = 0xFF;
uint32_t lastRxMs = 0, nextTxAt = 0, suppressEchoUntil = 0;
uint32_t lastSilentPollMs = 0, validFrames = 0, invalidFrames = 0;

uint8_t crc8Maxim(const uint8_t *data, size_t length) {
  uint8_t crc = 0;
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1;
  }
  return crc;
}
uint32_t readU32LE(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
float readFloatLE(const uint8_t *p) {
  float value;
  memcpy(&value, p, sizeof(value));
  return value;
}

struct ParsedFrame {
  uint8_t sync = 0, command = 0;
  uint16_t length = 0;
  const uint8_t *data = nullptr;
  bool crcOk = false;
};
class FrameParser {
 public:
  bool feed(uint8_t byte, ParsedFrame &out) {
    if (count == 0) {
      if (byte == 0xA5 || byte == 0xA0) buffer[count++] = byte;
      return false;
    }
    if (count >= sizeof(buffer)) {
      reset();
      if (byte == 0xA5 || byte == 0xA0) buffer[count++] = byte;
      return false;
    }
    buffer[count++] = byte;
    if (count < 4) return false;
    const uint16_t length = (uint16_t(buffer[2]) << 8) | buffer[3];
    if (length > MAX_FRAME_DATA) {
      reset();
      if (byte == 0xA5 || byte == 0xA0) buffer[count++] = byte;
      return false;
    }
    const size_t total = 4 + length + 1;
    if (count < total) return false;
    out.sync = buffer[0];
    out.command = buffer[1];
    out.length = length;
    out.data = buffer + 4;
    out.crcOk = crc8Maxim(buffer, total - 1) == buffer[total - 1];
    reset();
    return true;
  }
  void reset() { count = 0; }
 private:
  uint8_t buffer[4 + MAX_FRAME_DATA + 1] = {};
  size_t count = 0;
} parser;

struct TxFrame { uint8_t bytes[10] = {}; uint8_t length = 0; };
TxFrame txQueue[TX_QUEUE_SIZE];
size_t txHead = 0, txTail = 0;
bool txQueueEmpty() { return txHead == txTail; }
bool txQueueFull() { return ((txHead + 1) % TX_QUEUE_SIZE) == txTail; }
bool enqueueFrame(const uint8_t *bytes, uint8_t length) {
  if (txQueueFull() || length > sizeof(txQueue[0].bytes)) return false;
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

void handlePanelFrame(const ParsedFrame &frame) {
  if (frame.command == 0x30 && frame.length == 1) {
    pendingRegister = frame.data[0];
  } else if (frame.command == 0x01 && frame.length == 5 && frame.data[0] == 0x01) {
    const float incomingPower = readFloatLE(frame.data + 1);
    if (!isfinite(incomingPower)) return;
    panelPower = incomingPower;
    if (powerOverride && fabsf(incomingPower - desiredPower) > 0.5f)
      enqueueWriteFloat(0x01, desiredPower);
  } else if (frame.command == 0x20 && frame.length == 5 && frame.data[0] == 0x00) {
    heater.panelTemp = readFloatLE(frame.data + 1);
  }
}
void handleHeaterFrame(const ParsedFrame &frame) {
  heater.lastSeenMs = millis();
  busEverSeen = true;
  if (frame.command != 0x00 || (frame.length != 1 && frame.length != 4)) return;
  const uint8_t *data = frame.data;
  switch (pendingRegister) {
    case 0x0B: if (frame.length == 1) heater.error = data[0]; break;
    case 0x0C: if (frame.length == 4) heater.step = uint8_t(readU32LE(data)); break;
    case 0x50: if (frame.length == 1) heater.state = data[0]; break;
    case 0x20: if (frame.length == 4) heater.nodeTemp = readFloatLE(data); break;
    case 0x24: if (frame.length == 4) heater.voltage = readFloatLE(data); break;
    case 0x10: if (frame.length == 4) heater.doses = readU32LE(data); break;
    case 0x16: if (frame.length == 4) heater.fanRpm = readU32LE(data); break;
    default: break;
  }
  pendingRegister = 0xFF;
}
void onFrame(const ParsedFrame &frame) {
  if (!frame.crcOk) { ++invalidFrames; return; }
  ++validFrames;
  if (frame.sync == 0xA5) handlePanelFrame(frame);
  else if (frame.sync == 0xA0) handleHeaterFrame(frame);
}
void pumpRx() {
  while (HeaterSerial.available() > 0) {
    const int incoming = HeaterSerial.read();
    if (incoming < 0) continue;
    const uint32_t now = millis();
    if (now < suppressEchoUntil) continue;
    lastRxMs = now;
    ParsedFrame frame;
    if (parser.feed(uint8_t(incoming), frame)) onFrame(frame);
  }
}
void injectTask() {
  if (txQueueEmpty()) return;
  const uint32_t now = millis();
  if (now < nextTxAt || now - lastRxMs < TX_IDLE_GAP_MS) return;
  TxFrame &frame = txQueue[txTail];
  HeaterSerial.write(frame.bytes, frame.length);
  HeaterSerial.flush();
  if (frame.bytes[1] == 0x30 && frame.length == 6) pendingRegister = frame.bytes[4];
  suppressEchoUntil = millis() + 3;
  txTail = (txTail + 1) % TX_QUEUE_SIZE;
  nextTxAt = millis() + TX_REPEAT_GAP_MS;
}
void commandStart() {
  for (uint8_t i = 0; i < 3; ++i) enqueueWriteByte(0x02, 0x00);
  for (uint8_t i = 0; i < 3; ++i) enqueueWriteByte(0x03, 0x01);
}
void commandStop() {
  for (uint8_t i = 0; i < 3; ++i) enqueueWriteByte(0x03, 0x00);
}
void commandPower(int direction) {
  float current = powerOverride ? desiredPower : panelPower;
  if (!isfinite(current) || current < 500) current = POWER_LEVELS[0];
  size_t closest = 0;
  float smallestDifference = INFINITY;
  for (size_t i = 0; i < 5; ++i) {
    const float difference = fabsf(POWER_LEVELS[i] - current);
    if (difference < smallestDifference) { smallestDifference = difference; closest = i; }
  }
  const int selected = constrain(int(closest) + direction, 0, 4);
  desiredPower = POWER_LEVELS[selected];
  powerOverride = true;
  enqueueWriteFloat(0x01, desiredPower);
}

const char *stateText(uint8_t state) {
  switch (state) {
    case 0x20: return "Свеча";
    case 0x24: return "Розжиг";
    case 0x25: return "Пламя поймано";
    case 0x27: return "Набор мощности";
    case 0x2F: return "Разгон";
    case 0x2E: return "Горение";
    default: return nullptr;
  }
}
void broadcastState() {
  if (websocket.count() == 0) return;
  const bool live = heater.lastSeenMs && millis() - heater.lastSeenMs < TELEMETRY_STALE_MS;
  char json[420];
  snprintf(json, sizeof(json),
    "{\"st\":%u,\"step\":%u,\"err\":\"%02X\",\"t\":%.1f,\"v\":%.2f,"
    "\"fan\":%lu,\"dose\":%lu,\"tp\":%.1f,\"pw\":%.0f,\"live\":%s,"
    "\"valid\":%lu,\"invalid\":%lu}",
    heater.state, heater.step, heater.error,
    isnan(heater.nodeTemp) ? 0.0f : heater.nodeTemp,
    isnan(heater.voltage) ? 0.0f : heater.voltage,
    (unsigned long)heater.fanRpm, (unsigned long)heater.doses,
    isnan(heater.panelTemp) ? 0.0f : heater.panelTemp,
    isfinite(panelPower) ? panelPower : 0.0f, live ? "true" : "false",
    (unsigned long)validFrames, (unsigned long)invalidFrames);
  websocket.textAll(json);
}
void drawOled() {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AUTOHEATER ESP32");
  const bool live = heater.lastSeenMs && millis() - heater.lastSeenMs < TELEMETRY_STALE_MS;
  display.printf("Bus: %s CRC:%lu/%lu\n", live ? "LIVE" : "WAIT",
                 (unsigned long)validFrames, (unsigned long)invalidFrames);
  display.printf("State: %s\n", stateText(heater.state) ? stateText(heater.state) : "Unknown");
  display.printf("Temp: %.1f C\n", isnan(heater.nodeTemp) ? 0.0f : heater.nodeTemp);
  display.printf("Power: %.0f W\n", isfinite(panelPower) ? panelPower : 0.0f);
  display.printf("Fan: %lu RPM\n", (unsigned long)heater.fanRpm);
  display.printf("Voltage: %.2f V\n", isnan(heater.voltage) ? 0.0f : heater.voltage);
  display.display();
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#111827"><title>Autoheater ESP32</title>
<style>
*{box-sizing:border-box}body{font:16px system-ui,sans-serif;background:#10151f;color:#eee;margin:0;padding:16px}
main{max-width:520px;margin:auto}.card{background:#202938;border-radius:14px;padding:16px;margin:0 0 12px}
h1{font-size:1.35rem;margin:0 0 8px}.muted{color:#aab4c3;font-size:.9rem}
#state{font-size:1.4rem;font-weight:700;margin:12px 0}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.value{font-size:1.25rem;font-weight:700;color:#8ee5c2}.label{font-size:.8rem;color:#aab4c3}
button{border:0;border-radius:10px;padding:14px 8px;font-size:1rem;font-weight:650;background:#3b506b;color:white;width:100%;margin:4px 0}
.actions,.power{display:grid;grid-template-columns:1fr 1fr;gap:8px}#stop{background:#a33}
#connection{font-size:.9rem;color:#aab4c3}a{color:#8ee5c2}
</style></head><body><main>
<div class="card"><h1>Autoheater ESP32</h1><div id="connection">Подключение…</div><div id="state">Ожидание данных</div>
<div class="grid">
<div><div class="label">Температура узла</div><div class="value" id="temp">— °C</div></div>
<div><div class="label">Напряжение</div><div class="value" id="voltage">— В</div></div>
<div><div class="label">Обороты вентилятора</div><div class="value" id="fan">— об/мин</div></div>
<div><div class="label">Мощность пульта</div><div class="value" id="power">— Вт</div></div>
<div><div class="label">Шаг программы</div><div class="value" id="step">—</div></div>
<div><div class="label">Ошибка</div><div class="value" id="error">—</div></div>
</div></div>
<div class="card"><div class="actions"><button onclick="send('start')">Пуск</button>
<button id="stop" onclick="send('stop')">Стоп</button></div>
<div class="power"><button onclick="send('pdown')">Мощность −</button>
<button onclick="send('pup')">Мощность +</button></div>
<p class="muted">Команды передаются в паузах шины. Изменение мощности удерживается оверрайдом относительно штатного пульта.</p>
</div><div class="card muted"><a href="/update">Обновление прошивки OTA</a></div>
</main><script>
const el=id=>document.getElementById(id);
const socket=new WebSocket('ws://'+location.host+'/ws');
socket.onopen=()=>el('connection').textContent='Веб-интерфейс подключён';
socket.onclose=()=>el('connection').textContent='Соединение потеряно';
socket.onerror=()=>el('connection').textContent='Ошибка WebSocket';
socket.onmessage=e=>{const d=JSON.parse(e.data);
el('connection').textContent=d.live?'Связь с отопителем есть':'Нет свежих ответов от отопителя';
const labels={32:'Свеча',36:'Розжиг',37:'Пламя поймано',39:'Набор мощности',47:'Разгон',46:'Горение'};
el('state').textContent=labels[d.st]||('Код 0x'+d.st.toString(16).toUpperCase().padStart(2,'0'));
el('temp').textContent=Number(d.t).toFixed(1)+' °C';
el('voltage').textContent=Number(d.v).toFixed(2)+' В';
el('fan').textContent=d.fan+' об/мин';el('power').textContent=d.pw+' Вт';
el('step').textContent=d.step;el('error').textContent=d.err==='FF'?'Нет ошибки':'0x'+d.err;
};
function send(command){if(socket.readyState===WebSocket.OPEN)socket.send(command);}
</script></body></html>
)HTML";

void setupWeb() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("ERROR: Wi-Fi AP start failed");
    return;
  }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
  });
  websocket.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *,
                       AwsEventType type, void *arg, uint8_t *data, size_t length) {
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
    if (!info->final || info->index != 0 || info->len != length ||
        info->opcode != WS_TEXT || length == 0 || length > 16) return;
    char command[17] = {};
    memcpy(command, data, length);
    if (strcmp(command, "start") == 0) commandStart();
    else if (strcmp(command, "stop") == 0) commandStop();
    else if (strcmp(command, "pup") == 0) commandPower(+1);
    else if (strcmp(command, "pdown") == 0) commandPower(-1);
  });
  server.addHandler(&websocket);
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();
  Serial.printf("AP %s, IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}
}  // namespace

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(250);
  Serial.println("\nAutoheater ESP32 — protocol A5/A0, CRC-8/MAXIM");
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledReady) Serial.println("OLED not found at 0x3C");
  HeaterSerial.setRxBufferSize(1024);
  HeaterSerial.begin(HEATER_BAUD, SERIAL_8N1, PIN_HEATER_RX, PIN_HEATER_TX);
  lastRxMs = millis();
  setupWeb();
}
void loop() {
  pumpRx();
  injectTask();
  if (busEverSeen && txQueueEmpty() &&
      millis() - lastRxMs > SILENT_POLL_INTERVAL_MS &&
      millis() - lastSilentPollMs > SILENT_POLL_INTERVAL_MS) {
    lastSilentPollMs = millis();
    static bool alternate = false;
    alternate = !alternate;
    enqueueRead(alternate ? 0x50 : 0x24);
  }
  static uint32_t lastBroadcastMs = 0;
  if (millis() - lastBroadcastMs >= 500) {
    lastBroadcastMs = millis();
    broadcastState();
    drawOled();
  }
  websocket.cleanupClients();
  ElegantOTA.loop();
}