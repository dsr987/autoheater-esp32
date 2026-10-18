/*
 * Autoheater ESP32 — application entry point.
 * CHANGELOG_VERSION: 0.1.0
 * CHANGELOG_START
 * CHANGELOG_END
 * Версия разработки: 0.1.0
 *
 * Demo web UI only. No heater protocol or WebSocket is implemented.
 */

// -----------------------------------------------------------------------------
// Configuration — change these credentials before using the device outside a lab.
// -----------------------------------------------------------------------------
const char* WIFI_AP_SSID = "Autoheater";
const char* WIFI_AP_PASSWORD = "heater8260";  // WPA2 password: at least 8 characters

const char* WEB_ADMIN_USER = "admin";
const char* WEB_ADMIN_PASSWORD = "heater-admin-8260";  // HTTP Basic Authentication

#include <Arduino.h>
#include <WiFi.h>
#include "web_page.h"

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);

  if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD)) {
    // Keep the application safe if the access point could not be started.
    return;
  }

  setupWebPage();
}

void loop() {
  loopWebPage();
  delay(2);
}
