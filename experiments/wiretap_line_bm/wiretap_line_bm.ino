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
AsyncWebServer server(80);
const char OTA_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Autoheater ESP32 — Wiretap</title>
<style>
body{font:16px system-ui;background:#111827;color:#f3f4f6;max-width:680px;margin:8vh auto;padding:20px}
main{background:#1f2937;padding:24px;border-radius:14px}a{display:block;padding:13px;background:#2563eb;color:white;text-align:center;border-radius:9px;text-decoration:none}
p{line-height:1.6;color:#d1d5db}code{overflow-wrap:anywhere}
</style></head><body><main><h1>Autoheater ESP32 — Wiretap</h1>
<p>Экспериментальная прошивка пассивного наблюдения линии BM.</p>
<p>UART работает только на приём. Передача команд отопителю не реализована.</p>
<p>Обновление прошивки: <code>/update</code></p><a href="/update">Открыть ElegantOTA</a>
</main></body></html>
)HTML";
}

static constexpr int PIN_BM_RX = 16;
static constexpr int PIN_OLED_SDA = 21;
static constexpr int PIN_OLED_SCL = 22;
static constexpr uint32_t CONSOLE_BAUD = 115200;
static constexpr uint32_t HEATER_BAUD = 25000;  // candidate, source-project setting
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

void dumpFrame(const uint8_t *p) {
  Serial.printf("RX valid #%lu: ", (unsigned long)validFrames);
  for (uint8_t i = 0; i < FRAME_BYTES; ++i) {
    if (i) Serial.write(' ');
    Serial.printf("%02X", p[i]);
  }
  Serial.println();
}

bool parseExchange(const uint8_t *p) {
  if ((p[0] != 0x76 && p[0] != 0x78) || p[1] != 0x16 ||
      p[24] != 0x76 || p[25] != 0x16 ||
      p[44] != 100 || p[45] != 0 ||
      !crcValid(p) || !crcValid(p + 24)) {
    return false;
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
  dumpFrame(p);
  Serial.printf("DATA state=%s demand=%u duty=%.0f%% pump=%.1fHz fan=%uRPM chamber=%uC error=%u (%s)\n",
                stateText(telem.state), telem.demand, telem.dutyPct,
                telem.pumpHz, telem.fanRpm, telem.chamberC,
                telem.error, errorText(telem.error));
  return true;
}

void resetCapture() {
  frameCount = 0;
  frameActive = false;
}

void acceptByte(uint8_t b, uint32_t nowMs) {
  ++bytesSeen;
  const uint32_t gap = nowMs - lastByteMs;
  lastByteMs = nowMs;
  if (gap > BUS_IDLE_GAP_MS) resetCapture();

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
      Serial.printf("RX candidate rejected (header/length/CRC/check bytes); bad=%lu\n",
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
    Serial.println("# ERROR: Wi-Fi AP start failed; OTA unavailable.");
    return;
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", OTA_PAGE);
  });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true,\"ota\":\"/update\"}");
  });
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();

  Serial.printf("# OTA Wi-Fi: SSID=%s, IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.println("# OTA URL: http://192.168.4.1/update ; credentials: admin/admin (bench only)");
}

void setup() {
  Serial.begin(CONSOLE_BAUD);
  delay(250);
  Serial.println();
  Serial.println("# Autoheater compatibility hypothesis wiretap");
  Serial.println("# PASSIVE RX ONLY: no TX configured, no heater commands.");
  Serial.printf("# UART2 RX GPIO%d, candidate %lu baud 8N1; OLED SDA=%d SCL=%d\n",
                PIN_BM_RX, (unsigned long)HEATER_BAUD, PIN_OLED_SDA, PIN_OLED_SCL);
  Serial.println("# Parser checks 48-byte exchange, 0x76/0x78 headers, 0x16 markers, CRC16/Modbus.");
  Serial.println("# No valid frames does NOT by itself disprove protocol compatibility.");

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledReady) {
    Serial.println("# OLED not found at 0x3C; serial parser remains active.");
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
  HeaterSerial.begin(HEATER_BAUD, SERIAL_8N1, PIN_BM_RX, -1);
  setupOtaWeb();
}

void loop() {
  while (HeaterSerial.available() > 0) {
    const int b = HeaterSerial.read();
    if (b >= 0) acceptByte(uint8_t(b), millis());
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 500) {
    lastDrawMs = millis();
    drawOled();
  }
  ElegantOTA.loop();
}
