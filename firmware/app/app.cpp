/*
 * Autoheater ESP32 — release metadata.
 * The active Arduino sketch is app.ino in this directory.
 * CHANGELOG_VERSION: 0.2.6
 * CHANGELOG_START
 * - Added persistent RX logs in LittleFS with rotation and download endpoints.
 * - Switched the passive Blue Wire protocol experiment to 25000 baud and retained the published 48-byte frame/CRC checks.
 * - Kept heater command transmission disabled; compatibility remains unconfirmed until real frames and telemetry are validated.
 * CHANGELOG_END
 * Версия разработки: 0.2.6
 *
 * This file intentionally contains no setup()/loop(); app.ino owns firmware runtime.
 */
