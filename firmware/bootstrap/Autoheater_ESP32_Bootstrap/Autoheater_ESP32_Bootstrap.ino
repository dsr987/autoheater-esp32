#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

namespace {
constexpr char AP_SSID[] = "Autoheater";
constexpr char AP_PASSWORD[] = "12345678";
constexpr char OTA_USERNAME[] = "admin";
constexpr char OTA_PASSWORD[] = "admin";
AsyncWebServer server(80);
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Autoheater ESP32</title>
<style>body{font:16px system-ui;background:#111827;color:#f3f4f6;max-width:620px;margin:12vh auto;padding:24px}main{background:#1f2937;padding:28px;border-radius:16px}a{display:block;padding:14px;background:#2563eb;color:white;text-align:center;border-radius:10px;text-decoration:none}p{line-height:1.6;color:#d1d5db}</style></head><body><main><h1>Autoheater ESP32</h1><p>Стартовая прошивка установлена. Загрузите основное приложение через ElegantOTA.</p><p>Точка доступа: <b>Autoheater</b>; адрес: <code>192.168.4.1</code></p><a href="/update">Открыть обновление прошивки</a></main></body></html>
)HTML";
}
void setup() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) { delay(500); ESP.restart(); return; }
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { request->send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) { request->send(200, "application/json", "{\"ok\":true,\"ota\":\"/update\"}"); });
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();
}
void loop() { ElegantOTA.loop(); }
