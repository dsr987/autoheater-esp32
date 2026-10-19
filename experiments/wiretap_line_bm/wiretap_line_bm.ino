/*
 * Autoheater ESP32 — passive BM line wiretap (hypothesis branch)
 *
 * RX-only UART sniffer. It never transmits to the heater.
 * Candidate settings are intentionally configurable: the BM/blue-wire
 * equivalence and 25,000 baud are hypotheses, not confirmed facts.
 *
 * Board: classic ESP32 DevKit / ESP32-WROOM-32
 * RX: GPIO16 (UART2 RX)
 * USB serial console: 115200 baud
 */

#include <Arduino.h>

static constexpr int kBmRxPin = 16;
static constexpr uint32_t kConsoleBaud = 115200;
static constexpr uint32_t kCandidateLineBaud = 25000; // hypothesis only
static constexpr uint32_t kFrameGapUs = 4000;          // split display chunks after idle gap
static constexpr size_t kMaxChunk = 64;

HardwareSerial BmSerial(2);

uint8_t chunk[kMaxChunk];
size_t chunkLength = 0;
uint32_t chunkStartedUs = 0;
uint32_t lastByteUs = 0;
uint32_t bytesSeen = 0;
uint32_t chunksSeen = 0;

void printChunk() {
  if (chunkLength == 0) return;

  Serial.printf("t_us=%lu chunk=%lu len=%u data=",
                static_cast<unsigned long>(chunkStartedUs),
                static_cast<unsigned long>(chunksSeen++),
                static_cast<unsigned>(chunkLength));
  for (size_t i = 0; i < chunkLength; ++i) {
    Serial.printf("%02X", chunk[i]);
    if (i + 1 < chunkLength) Serial.write(' ');
  }
  Serial.println();

  chunkLength = 0;
}

void setup() {
  Serial.begin(kConsoleBaud);
  delay(300);
  Serial.println();
  Serial.println("# Autoheater BM passive wiretap");
  Serial.println("# MODE=RX_ONLY; TX is not configured");
  Serial.printf("# RX_GPIO=%d candidate_baud=%lu format=8N1 frame_gap_us=%lu\n",
                kBmRxPin,
                static_cast<unsigned long>(kCandidateLineBaud),
                static_cast<unsigned long>(kFrameGapUs));
  Serial.println("# NOTE: baud/protocol/voltage compatibility are unverified hypotheses.");

  // Configure RX buffering before starting UART. TX pin -1: do not drive BM.
  BmSerial.setRxBufferSize(4096);
  BmSerial.begin(kCandidateLineBaud, SERIAL_8N1, kBmRxPin, -1);
}

void loop() {
  while (BmSerial.available() > 0) {
    const int value = BmSerial.read();
    if (value < 0) break;

    const uint32_t nowUs = micros();
    if (chunkLength > 0 &&
        static_cast<uint32_t>(nowUs - lastByteUs) > kFrameGapUs) {
      printChunk();
    }

    if (chunkLength == 0) chunkStartedUs = nowUs;
    chunk[chunkLength++] = static_cast<uint8_t>(value);
    lastByteUs = nowUs;
    ++bytesSeen;

    if (chunkLength >= kMaxChunk) printChunk();
  }

  if (chunkLength > 0 &&
      static_cast<uint32_t>(micros() - lastByteUs) > kFrameGapUs) {
    printChunk();
  }

  // Periodic health counter, useful for distinguishing "no traffic" from a reset.
  static uint32_t lastStatsMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastStatsMs >= 10000) {
    lastStatsMs = nowMs;
    Serial.printf("# stats uptime_ms=%lu bytes=%lu chunks=%lu\n",
                  static_cast<unsigned long>(nowMs),
                  static_cast<unsigned long>(bytesSeen),
                  static_cast<unsigned long>(chunksSeen));
  }
}
