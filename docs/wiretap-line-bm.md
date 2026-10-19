# BM passive wiretap — assembly notes

The initial sketch and its detailed wiring instructions are in [`experiments/wiretap_line_bm/`](../experiments/wiretap_line_bm/).

This branch tests a narrow hypothesis: the controller connector's `BM` line might use the same wired protocol as the “blue wire” documented by other diesel-heater projects. That equivalence is not confirmed.

## Minimal hardware

- ESP32 DevKit / ESP32-WROOM-32.
- USB data cable and computer for programming/logging.
- Multimeter and oscilloscope or logic analyzer.
- Receive-only high-impedance signal interface chosen after measuring BM.
- Test leads/adapters; optional I²C OLED is not needed for raw capture.

## Pinout summary

- BM → measured/protected input interface → ESP32 GPIO16 (UART2 RX).
- Verified controller GND → ESP32 GND only after confirming the shared reference is safe.
- ESP32 TX is not used and must not be connected to BM.
- USB serial console: 115200 baud.
- Initial candidate line format: 25,000 baud, 8N1; hypothesis only.

**Electrical safety:** ESP32 GPIOs are not 5 V tolerant. Measure the line before connecting the ESP32. Do not use a generic I²C level-shifter board by default for an unknown shared/UART-like signal. Do not connect VCC to the ESP32. This firmware is receive-only and must remain so during initial experiments.
