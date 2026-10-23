/*
 * Autoheater ESP32 — compatibility hypothesis wiretap
 * Based on the published frame checks/field offsets in:
 * https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome
 *
 * PASSIVE RX ONLY. No TX pin is configured; do not connect ESP32 TX to BM.
 * The BM/blue-wire equivalence is a hypothesis until valid frames and
 * plausible live telemetry are observed on the actual controller.
 *
 * Board: classic ESP32 DevKit / ESP32-WROOM-32
 * BM interface output -> GPIO16 (UART2 RX)
 * OLED: SSD1306 128x64 I2C, SDA=GPIO21, SCL=GPIO22 (change if needed)
 * Serial log: 115200 baud
 * OTA: Wi-Fi AP Autoheater, http://192.168.4.1/update (bench defaults below)
 *
 * Libraries: Adafruit GFX Library, Adafruit SSD1306, AsyncTCP,
 * ESPAsyncWebServer, ElegantOTA
 */

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

namespace {
static constexpr char AP_SSID[] = "Autoheater";
static constexpr char AP_PASSWORD[] = "12345678";  // bench only; change before deployment
static constexpr char OTA_USERNAME[] = "admin";    // bench only; change before deployment
static constexpr char OTA_PASSWORD[] = "admin";    // bench only; change before deployment
static constexpr char LOG_PATH[] = "/wiretap.log";
static constexpr size_t RAM_LOG_LINES = 100;
static constexpr size_t RAM_LOG_LINE_LEN = 260;
static constexpr size_t RAW_CHUNK_BYTES = 24;
static constexpr size_t LOG_STAGE_BYTES = 3072;
AsyncWebServer server(80);
const char OTA_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#111827">
<title>Autoheater — мониторинг BM</title>
<style>
*{box-sizing:border-box}body{font:15px system-ui,sans-serif;background:#0b1120;color:#e5e7eb;max-width:920px;margin:auto;padding:14px}
h1{font-size:1.35rem;margin:4px 0 8px}.muted{color:#9ca3af}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.card{background:#172033;border:1px solid #2b3850;border-radius:12px;padding:13px;min-width:0}.wide{grid-column:1/-1}.k{font-size:.78rem;color:#9ca3af}.v{font-size:1.15rem;font-weight:650;overflow-wrap:anywhere}
#logs{font:12px ui-monospace,monospace;white-space:pre-wrap;overflow-wrap:anywhere;max-height:48vh;overflow:auto;background:#080d17;border-radius:8px;padding:10px;min-height:130px}
a,button{display:inline-block;border:0;border-radius:8px;padding:10px 13px;background:#2563eb;color:white;text-decoration:none;font:inherit}
header{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}.status{font-size:.85rem;color:#9ca3af}
@media(min-width:620px){.grid{grid-template-columns:repeat(3,minmax(0,1fr))}.wide{grid-column:1/-1}}
</style></head><body>
<header><div><h1>Autoheater ESP32</h1><div class="muted">Пассивный монитор BM · RX only</div></div><a href="/update">OTA</a></header>
<div class="status" id="conn">Подключение к контроллеру…</div>
<div class="grid" style="margin-top:12px">
<div class="card"><div class="k">Связь</div><div class="v" id="link">Ожидание данных</div></div>
<div class="card"><div class="k">Принято байт</div><div class="v" id="bytes">0</div></div>
<div class="card"><div class="k">Корректные обмены</div><div class="v" id="valid">0</div></div>
<div class="card"><div class="k">Отклонённые кадры</div><div class="v" id="bad">0</div></div>
<div class="card"><div class="k">Состояние</div><div class="v" id="state">—</div></div>
<div class="card"><div class="k">Ошибка</div><div class="v" id="error">—</div></div>
<div class="card"><div class="k">Температура камеры</div><div class="v" id="temp">—</div></div>
<div class="card"><div class="k">Нагрузка / мощность</div><div class="v" id="duty">—</div></div>
<div class="card"><div class="k">Вентилятор</div><div class="v" id="fan">—</div></div>
<div class="card"><div class="k">Топливный насос</div><div class="v" id="pump">—</div></div>
<div class="card"><div class="k">Последний кадр</div><div class="v" id="age">—</div></div>
<div class="card"><div class="k">Заголовок</div><div class="v" id="header">—</div></div>
<div class="card wide"><div style="display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap"><strong>Журнал захвата</strong><div style="display:flex;gap:8px;flex-wrap:wrap"><button onclick="refreshLogs()">Обновить</button><button onclick="downloadLog()">Скачать TXT</button><button onclick="clearLog()">Очистить</button></div></div><p class="muted">Полный журнал сохраняется в LittleFS и переживает перезапуск. В него входят сырые байты UART в HEX, временные отметки, найденные кандидаты, CRC и расшифрованные поля. Скачивайте файл периодически.</p><div class="muted" id="logInfo">Размер журнала: —</div><div id="logs">Ожидание журнала…</div></div>
<div class="card wide"><div class="muted">Проверка сырых данных и CRC — экспериментальная. Отсутствие корректных кадров само по себе не доказывает несовместимость протокола.</div></div>
</div>
<script>
const el=id=>document.getElementById(id);let lastLogs='';
async function refreshStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);const d=await r.json();
el('conn').textContent='Контроллер доступен · обновление каждую секунду';
el('bytes').textContent=d.bytes;el('valid').textContent=d.valid;el('bad').textContent=d.bad;
el('link').textContent=d.have?(d.stale?'Данные устарели':'Кадры получены'):'Нет корректных кадров';
el('state').textContent=d.have?d.stateText:'—';el('error').textContent=d.have?(d.error+' · '+d.errorText):'—';
el('temp').textContent=d.have?d.temp+' °C':'—';el('duty').textContent=d.have?d.demand+' / '+d.duty.toFixed(0)+'%':'—';
el('fan').textContent=d.have?d.fan+' об/мин':'—';el('pump').textContent=d.have?d.pump.toFixed(1)+' Гц':'—';
el('age').textContent=d.have?d.age+' с':'—';el('header').textContent=d.have?'0x'+d.header.toString(16).toUpperCase().padStart(2,'0'):'—';
}catch(e){el('conn').textContent='Нет связи с веб-интерфейсом: '+e.message;}}
async function refreshLogs(){try{const r=await fetch('/api/log',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const t=await r.text();if(t!==lastLogs){const box=el('logs');const atBottom=box.scrollTop+box.clientHeight>=box.scrollHeight-25;box.textContent=t||'Журнал пока пуст.';if(atBottom)box.scrollTop=box.scrollHeight;lastLogs=t;}const info=await fetch('/api/log-info',{cache:'no-store'});if(info.ok){const d=await info.json();el('logInfo').textContent='Файл: '+d.bytes+' байт · LittleFS: '+(d.mounted?'подключена':'НЕ подключена')+(d.exists?'':' · файл не найден')+' · свободно '+d.free+' из '+d.total+' байт'+(d.full?' · достигнут предел записи':'')+(d.error?' · '+d.error:'');}}catch(e){el('logInfo').textContent='Ошибка чтения журнала: '+e.message;}}
function downloadLog(){window.location.href='/download-log?ts='+Date.now();}
async function clearLog(){if(!confirm('Удалить сохранённый журнал и начать новый?'))return;try{const r=await fetch('/api/log/clear',{method:'POST'});if(!r.ok)throw Error((await r.text())||r.status);await new Promise(resolve=>setTimeout(resolve,400));lastLogs='';await refreshLogs();}catch(e){alert('Не удалось очистить журнал: '+e.message);}}
refreshStatus();refreshLogs();setInterval(refreshStatus,1000);setInterval(refreshLogs,1500);
</script></body></html>
)HTML";
}

static constexpr int PIN_BM_RX = 16;
static constexpr int PIN_OLED_SDA = 21;
static constexpr int PIN_OLED_SCL = 22;
static constexpr uint32_t CONSOLE_BAUD = 115200;
static constexpr uint32_t HEATER_BAUDS[] = {25000, 9600, 4800, 2400};
static constexpr size_t HEATER_BAUD_COUNT = sizeof(HEATER_BAUDS) / sizeof(HEATER_BAUDS[0]);
static constexpr uint32_t BAUD_DWELL_MS = 12000;  // one rate per 12 s
static constexpr uint32_t BUS_IDLE_GAP_MS = 100;
static constexpr uint8_t FRAME_BYTES = 48;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;

HardwareSerial HeaterSerial(2);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

uint8_t frame[FRAME_BYTES];
uint8_t frameCount = 0;
bool frameActive = false;
bool haveTelemetry = false;
bool oledReady = false;
uint32_t lastByteMs = 0;
uint32_t lastValidFrameMs = 0;
uint32_t validFrames = 0;
uint32_t badFrames = 0;
uint32_t bytesSeen = 0;
uint32_t activeBaud = HEATER_BAUDS[0];
uint32_t baudStartedMs = 0;
size_t baudIndex = 0;

char debugLog[RAM_LOG_LINES][RAM_LOG_LINE_LEN] = {};
size_t logWriteIndex = 0;
size_t logLineCount = 0;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
char pendingFileLog[LOG_STAGE_BYTES] = {};
size_t pendingFileLogLength = 0;
bool fileLogReady = false;
bool fileLogFull = false;
volatile bool clearLogRequested = false;
uint8_t rawChunk[RAW_CHUNK_BYTES] = {};
size_t rawChunkCount = 0;
uint32_t rawChunkStartMs = 0;
uint32_t rawChunkGapMs = 0;

void flushPersistentLog() {
  if (!fileLogReady || pendingFileLogLength == 0 || fileLogFull) return;
  const size_t total = LittleFS.totalBytes();
  if (total == 0 || LittleFS.usedBytes() + pendingFileLogLength > total - 4096) { fileLogFull = true; return; }
  File file = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!file) { fileLogFull = true; return; }
  const size_t written = file.write(reinterpret_cast<const uint8_t *>(pendingFileLog), pendingFileLogLength);
  file.close();
  if (written != pendingFileLogLength) { fileLogFull = true; return; }
  pendingFileLogLength = 0; pendingFileLog[0] = '\0';
}

void appendLog(const char *line) {
  char clean[RAM_LOG_LINE_LEN]; snprintf(clean, sizeof(clean), "%s", line);
  size_t n = strlen(clean);
  while (n > 0 && (clean[n - 1] == '\n' || clean[n - 1] == '\r')) clean[--n] = '\0';
  portENTER_CRITICAL(&logMux);
  snprintf(debugLog[logWriteIndex], RAM_LOG_LINE_LEN, "%s", clean);
  logWriteIndex = (logWriteIndex + 1) % RAM_LOG_LINES;
  if (logLineCount < RAM_LOG_LINES) ++logLineCount;
  portEXIT_CRITICAL(&logMux);
  if (!fileLogReady || fileLogFull || n == 0) return;
  const size_t needed = n + 1;
  if (pendingFileLogLength + needed >= LOG_STAGE_BYTES) flushPersistentLog();
  if (pendingFileLogLength + needed < LOG_STAGE_BYTES) {
    memcpy(pendingFileLog + pendingFileLogLength, clean, n); pendingFileLogLength += n;
    pendingFileLog[pendingFileLogLength++] = '\n'; pendingFileLog[pendingFileLogLength] = '\0';
  } else fileLogFull = true;
}

void logPrintln(const char *line = "") { Serial.println(line); appendLog(line); }
void logPrintf(const char *format, ...) {
  char line[RAM_LOG_LINE_LEN]; va_list args; va_start(args, format);
  vsnprintf(line, sizeof(line), format, args); va_end(args); Serial.print(line); appendLog(line);
}

String logSnapshot() {
  String out; portENTER_CRITICAL(&logMux);
  const size_t first = (logWriteIndex + RAM_LOG_LINES - logLineCount) % RAM_LOG_LINES;
  for (size_t i = 0; i < logLineCount; ++i) { out += debugLog[(first + i) % RAM_LOG_LINES]; out += '\n'; }
  portEXIT_CRITICAL(&logMux); return out;
}

void beginPersistentLog() {
  fileLogReady = LittleFS.begin(false);
  if (!fileLogReady) { Serial.println("# ERROR: LittleFS mount failed; persistent log unavailable."); return; }
  if (!LittleFS.exists(LOG_PATH)) { File file = LittleFS.open(LOG_PATH, FILE_WRITE); if (file) file.close(); }
  fileLogReady = LittleFS.exists(LOG_PATH);
  if (fileLogReady) { File file = LittleFS.open(LOG_PATH, FILE_APPEND); if (file) { file.printf("\n===== BOOT millis=%lu =====\n", (unsigned long)millis()); file.close(); } }
}

void clearPersistentLog() {
  if (!fileLogReady) return;
  pendingFileLogLength = 0; pendingFileLog[0] = '\0'; fileLogFull = false; rawChunkCount = 0;
  portENTER_CRITICAL(&logMux);
  logWriteIndex = 0; logLineCount = 0;
  portEXIT_CRITICAL(&logMux);
  File file = LittleFS.open(LOG_PATH, FILE_WRITE);
  if (file) { file.println("# Log cleared from web UI"); file.close(); } else fileLogFull = true;
}

void flushRawChunk() {
  if (rawChunkCount == 0) return; char line[250];
  int used = snprintf(line, sizeof(line), "RAW baud=%lu ms=%lu gap_ms=%lu n=%u hex=", (unsigned long)activeBaud, (unsigned long)rawChunkStartMs, (unsigned long)rawChunkGapMs, (unsigned)rawChunkCount);
  for (size_t i = 0; i < rawChunkCount && used > 0 && size_t(used) < sizeof(line) - 4; ++i) used += snprintf(line + used, sizeof(line) - size_t(used), "%02X%s", rawChunk[i], (i + 1 == rawChunkCount) ? "" : " ");
  logPrintln(line); rawChunkCount = 0;
}

void recordRawByte(uint8_t b, uint32_t nowMs, uint32_t gapMs) {
  if (rawChunkCount == 0) { rawChunkStartMs = nowMs; rawChunkGapMs = gapMs; }
  rawChunk[rawChunkCount++] = b; if (rawChunkCount >= RAW_CHUNK_BYTES) flushRawChunk();
}
struct Telemetry {
  uint8_t header = 0;
  uint8_t demand = 0;
  uint8_t state = 0xFF;
  uint8_t activeError = 0xFF;
  uint8_t error = 0xFF;
  float pumpHz = NAN;
  uint16_t fanRpm = 0;
  uint16_t chamberC = 0;
  float dutyPct = NAN;
} telem;

uint16_t crc16Modbus(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
  }
  return crc;
}

bool crcValid(const uint8_t *p) {
  const uint16_t crc = crc16Modbus(p, 22);
  return p[22] == uint8_t(crc >> 8) && p[23] == uint8_t(crc & 0xFF);
}

const char *stateText(uint8_t state) {
  switch (state) {
    case 0: return "Off";
    case 1: return "Starting";
    case 2: return "Pre-heat";
    case 3: return "Retry start";
    case 4: return "Ignition";
    case 5: return "Running";
    case 6: return "Stop command";
    case 7: return "Stopping";
    case 8: return "Cooldown";
    default: return "Unknown";
  }
}

const char *errorText(uint8_t error) {
  switch (error) {
    case 0: return "No error";
    case 1: return "Started";
    case 2: return "Voltage low";
    case 3: return "Voltage high";
    case 4: return "Glow plug";
    case 5: return "Pump overcurrent";
    case 6: return "Overtemperature";
    case 7: return "Motor failure";
    case 8: return "Serial lost";
    case 9: return "Flame out";
    case 10: return "Temp sensor";
    case 11: return "Ignition failed";
    case 12: return "Start attempt";
    case 13: return "Excess fuel";
    default: return "Unknown";
  }
}

void dumpFrame(const char *label, const uint8_t *p) {
  char line[RAM_LOG_LINE_LEN]; int used = snprintf(line, sizeof(line), "%s", label);
  for (uint8_t i = 0; i < FRAME_BYTES && used > 0 && size_t(used) < sizeof(line) - 4; ++i) used += snprintf(line + used, sizeof(line) - size_t(used), " %02X", p[i]);
  logPrintln(line);
}

void logCrcDetails(const uint8_t *p) {
  const uint16_t c0 = crc16Modbus(p, 22), c1 = crc16Modbus(p + 24, 22);
  logPrintf("CRC block0 calc=%04X rx=%02X%02X valid=%s; block1 calc=%04X rx=%02X%02X valid=%s\n", c0, p[22], p[23], crcValid(p) ? "yes" : "NO", c1, p[46], p[47], crcValid(p + 24) ? "yes" : "NO");
}

bool parseExchange(const uint8_t *p) {
  if ((p[0] != 0x76 && p[0] != 0x78) || p[1] != 0x16 || p[24] != 0x76 || p[25] != 0x16 || p[44] != 100 || p[45] != 0 || !crcValid(p) || !crcValid(p + 24)) {
    logPrintf("REJECT ms=%lu hdr=%02X%02X second=%02X%02X trailer=%u,%u\n", (unsigned long)millis(), p[0], p[1], p[24], p[25], p[44], p[45]);
    logCrcDetails(p); dumpFrame("CANDIDATE HEX:", p); return false;
  }

  telem.header = p[0];
  telem.demand = p[4];
  const uint8_t receivedState = p[26];
  if (receivedState <= 8) telem.state = receivedState;
  telem.activeError = p[27];
  telem.error = p[41];
  telem.pumpHz = p[40] * 0.1f;
  telem.fanRpm = uint16_t(p[30]) * 256U + p[31];
  telem.chamberC = uint16_t(p[34]) * 256U + p[35];
  telem.dutyPct = ((float(telem.demand) - 8.0f) / 27.0f) * 100.0f;

  haveTelemetry = true;
  lastValidFrameMs = millis();
  ++validFrames;
  dumpFrame("VALID HEX:", p);
  logPrintf("DATA ms=%lu state_raw=%u state=%s active_error=%u demand_raw=%u duty_calc=%.1f%% pump_raw=%u pump=%.1fHz fan_raw=%u,%u fan=%uRPM chamber_raw=%u,%u chamber=%uC error=%u (%s)\n", (unsigned long)millis(), telem.state, stateText(telem.state), telem.activeError, telem.demand, telem.dutyPct, p[40], telem.pumpHz, p[30], p[31], telem.fanRpm, p[34], p[35], telem.chamberC, telem.error, errorText(telem.error));
  return true;
}

void resetCapture() {
  frameCount = 0;
  frameActive = false;
}

void acceptByte(uint8_t b, uint32_t nowMs) {
  ++bytesSeen; const uint32_t gap = nowMs - lastByteMs; lastByteMs = nowMs;
  if (gap > BUS_IDLE_GAP_MS) { flushRawChunk(); logPrintf("UART_GAP baud=%lu ms=%lu gap_ms=%lu bytes_total=%lu partial_candidate=%u\n", (unsigned long)activeBaud, (unsigned long)nowMs, (unsigned long)gap, (unsigned long)bytesSeen, frameActive ? frameCount : 0); resetCapture(); }
  recordRawByte(b, nowMs, gap);

  if (!frameActive) {
    if (b == 0x76 || b == 0x78) {
      frame[0] = b;
      frameCount = 1;
      frameActive = true;
    }
    return;
  }

  if (frameCount == 1 && b != 0x16) {
    if (b == 0x76 || b == 0x78) {
      frame[0] = b;
      frameCount = 1;
    } else {
      resetCapture();
    }
    return;
  }

  frame[frameCount++] = b;
  if (frameCount == FRAME_BYTES) {
    if (!parseExchange(frame)) {
      ++badFrames;
      logPrintf("RX candidate rejected (header/length/CRC/check bytes); bad=%lu\n",
                    (unsigned long)badFrames);
    }
    resetCapture();
  }
}

void drawOled() {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AUTOHEATER WIRETAP");

  if (!haveTelemetry) {
    display.println("---------------------");
    display.println("Waiting for valid data");
    display.printf("RX bytes: %lu\n", (unsigned long)bytesSeen);
    display.printf("Valid exchanges: %lu\n", (unsigned long)validFrames);
    display.printf("Rejected: %lu\n", (unsigned long)badFrames);
    display.println("BM == blue wire?");
    display.println("HYPOTHESIS NOT PROVEN");
  } else {
    const bool stale = millis() - lastValidFrameMs > 15000;
    display.printf("Link: %s\n", stale ? "STALE" : "VALID");
    display.printf("State: %s\n", stateText(telem.state));
    display.printf("Chamber: %u C\n", telem.chamberC);
    display.printf("Demand: %u  Duty: %.0f%%\n", telem.demand, telem.dutyPct);
    display.printf("Fan: %u RPM\n", telem.fanRpm);
    display.printf("Pump: %.1f Hz\n", telem.pumpHz);
    display.printf("Error: %u %s\n", telem.error, errorText(telem.error));
  }
  display.display();
}

void setupOtaWeb() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    logPrintln("# ERROR: Wi-Fi AP start failed; OTA unavailable.");
    return;
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", OTA_PAGE);
  });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true,\"ota\":\"/update\"}");
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool stale = haveTelemetry && (millis() - lastValidFrameMs > 15000);
    char json[900];
    snprintf(json, sizeof(json),
      "{\"have\":%s,\"stale\":%s,\"bytes\":%lu,\"valid\":%lu,\"bad\":%lu,"
      "\"header\":%u,\"state\":%u,\"stateText\":\"%s\",\"error\":%u,\"errorText\":\"%s\","
      "\"temp\":%u,\"demand\":%u,\"duty\":%.2f,\"fan\":%u,\"pump\":%.1f,\"age\":%lu}",
      haveTelemetry ? "true" : "false", stale ? "true" : "false",
      (unsigned long)bytesSeen, (unsigned long)validFrames, (unsigned long)badFrames,
      telem.header, telem.state, stateText(telem.state), telem.error, errorText(telem.error),
      telem.chamberC, telem.demand, isnan(telem.dutyPct) ? 0.0 : telem.dutyPct,
      telem.fanRpm, isnan(telem.pumpHz) ? 0.0 : telem.pumpHz,
      haveTelemetry ? (unsigned long)((millis() - lastValidFrameMs) / 1000) : 0UL);
    request->send(200, "application/json; charset=utf-8", json);
  });
  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain; charset=utf-8", logSnapshot());
  });
  server.on("/download-log", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Always make a downloadable text log available. If LittleFS cannot be
    // mounted, fall back to the RAM ring buffer instead of returning HTTP 503.
    if (fileLogReady && LittleFS.exists(LOG_PATH)) {
      flushPersistentLog();
      AsyncWebServerResponse *response = request->beginResponse(LittleFS, LOG_PATH, "text/plain; charset=utf-8", true);
      response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      response->addHeader("Content-Disposition", "attachment; filename=wiretap.log.txt");
      request->send(response);
      return;
    }
    String snapshot = String("# WARNING: LittleFS unavailable; this download contains only the recent RAM log (up to 100 lines).\n") + logSnapshot();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain; charset=utf-8", snapshot);
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    response->addHeader("Content-Disposition", "attachment; filename=wiretap-ram-fallback.txt");
    request->send(response);
  });
  server.on("/api/log-info", HTTP_GET, [](AsyncWebServerRequest *request) {
    size_t fileBytes = 0; File f = fileLogReady ? LittleFS.open(LOG_PATH, FILE_READ) : File(); if (f) { fileBytes = f.size(); f.close(); }
    char json[320];
    const bool exists = fileLogReady && LittleFS.exists(LOG_PATH);
    const char *error = !fileLogReady ? "LittleFS mount or log file unavailable" : (fileLogFull ? "Log storage full or write failed" : "");
    snprintf(json, sizeof(json), "{\"mounted\":%s,\"exists\":%s,\"bytes\":%lu,\"total\":%lu,\"free\":%lu,\"full\":%s,\"error\":\"%s\"}", fileLogReady ? "true" : "false", exists ? "true" : "false", (unsigned long)fileBytes, fileLogReady ? (unsigned long)LittleFS.totalBytes() : 0UL, fileLogReady ? (unsigned long)(LittleFS.totalBytes() - LittleFS.usedBytes()) : 0UL, fileLogFull ? "true" : "false", error);
    request->send(200, "application/json; charset=utf-8", json);
  });
  server.on("/api/log/clear", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!fileLogReady) { request->send(503, "text/plain; charset=utf-8", "Persistent log unavailable"); return; }
    clearLogRequested = true;
    request->send(202, "text/plain; charset=utf-8", "Log clear queued");
  });
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();

  logPrintf("# OTA Wi-Fi: SSID=%s, IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  logPrintln("# OTA URL: http://192.168.4.1/update ; credentials: admin/admin (bench only)");
}

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(250);
  beginPersistentLog();
  logPrintln();
  logPrintln("# Autoheater compatibility hypothesis wiretap");
  logPrintln("# PASSIVE RX ONLY: no TX configured, no heater commands.");
  logPrintf("# UART2 RX GPIO%d, candidate %lu baud 8N1; OLED SDA=%d SCL=%d\n",
                PIN_BM_RX, (unsigned long)HEATER_BAUDS[0], PIN_OLED_SDA, PIN_OLED_SCL);
  logPrintln("# Parser checks 48-byte exchange, 0x76/0x78 headers, 0x16 markers, CRC16/Modbus.");
  logPrintln("# No valid frames does NOT by itself disprove protocol compatibility.");

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledReady) {
    logPrintln("# OLED not found at 0x3C; serial parser remains active.");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("AUTOHEATER WIRETAP");
    display.println("Waiting for BM...");
    display.display();
  }

  HeaterSerial.setRxBufferSize(4096);
  activeBaud = HEATER_BAUDS[0];
  baudIndex = 0;
  baudStartedMs = millis();
  HeaterSerial.begin(activeBaud, SERIAL_8N1, PIN_BM_RX, -1);
  logPrintf("# BAUD_SCAN_START baud=%lu dwell_ms=%lu\n", (unsigned long)activeBaud, (unsigned long)BAUD_DWELL_MS);
  logPrintf("# LittleFS persistent log: %s, file=%s\n", fileLogReady ? "ready" : "UNAVAILABLE", LOG_PATH);
  setupOtaWeb();
}

void loop() {
  // Passive baud-rate sweep: no TX pin is configured. Discard the partial
  // candidate at each rate change so bytes from different decoders cannot mix.
  if (millis() - baudStartedMs >= BAUD_DWELL_MS) {
    flushRawChunk();
    resetCapture();
    while (HeaterSerial.available() > 0) HeaterSerial.read();
    baudIndex = (baudIndex + 1) % HEATER_BAUD_COUNT;
    activeBaud = HEATER_BAUDS[baudIndex];
    HeaterSerial.end();
    delay(2);
    HeaterSerial.begin(activeBaud, SERIAL_8N1, PIN_BM_RX, -1);
    baudStartedMs = millis();
    lastByteMs = baudStartedMs;
    logPrintf("# BAUD_SCAN baud=%lu dwell_ms=%lu total_bytes=%lu valid=%lu rejected=%lu\n",
      (unsigned long)activeBaud, (unsigned long)BAUD_DWELL_MS,
      (unsigned long)bytesSeen, (unsigned long)validFrames, (unsigned long)badFrames);
  }
  if (clearLogRequested) { clearLogRequested = false; clearPersistentLog(); logPrintln("# Log cleared from web UI"); }
  while (HeaterSerial.available() > 0) {
    const int b = HeaterSerial.read();
    if (b >= 0) acceptByte(uint8_t(b), millis());
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 500) {
    lastDrawMs = millis(); drawOled(); flushRawChunk(); flushPersistentLog();
  }
  ElegantOTA.loop();
}
