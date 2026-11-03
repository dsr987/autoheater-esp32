# Changelog

## Unreleased
- Integrated the A5/A0 heater protocol from HEATER_PROTOCOL.md into the primary ESP32 firmware.
- Added CRC-8/MAXIM frame parsing, register telemetry, start/stop/power commands, power override, silent polling, and a WebSocket control panel.
- Removed obsolete BM wiretap and UART baud-sweep experiment documentation and workflow.

## 0.1.0
- Initial Autoheater ESP32 firmware skeleton with ElegantOTA bootstrap and GitHub Actions build.
