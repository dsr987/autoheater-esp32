# BM line wiretap — compatibility hypothesis test

**Goal:** test whether the controller's `BM` line is compatible with the published “blue-wire” Chinese diesel-heater protocol. This sketch uses the reference project's 48-byte exchange framing, CRC16/Modbus checks and telemetry byte offsets. Compatibility is not confirmed until valid CRC-checked exchanges and plausible changing live values are observed on this actual controller.

## Firmware behavior

- ESP32 UART2 receive-only at 25,000 baud, 8N1; GPIO16 is RX and TX is not configured.
- Collects candidate 48-byte exchanges beginning with `0x76 0x16` or `0x78 0x16`.
- Validates the controller and heater 24-byte subframes with the reference project's CRC16/Modbus algorithm, plus expected headers/check bytes.
- On valid exchanges, decodes heater state, requested duty, derived duty percentage, pump frequency, fan RPM, chamber temperature and error code.
- Shows telemetry and link freshness on an SSD1306 128×64 I²C OLED; also prints raw valid exchanges and decoded values to USB serial at 115200 baud.
- Does not transmit commands or drive the BM line.

Reference implementation: [timmchugh11/Chinese-Diesel-Heater---ESPHome, heater_uart.cpp](https://github.com/timmchugh11/Chinese-Diesel-Heater---ESPHome/blob/main/components/diesel_heater/heater_uart.cpp). Field offsets and CRC checks follow that code; this standalone sketch is an experimental port, not a certified port of the complete component.

## Required components

1. Classic ESP32 DevKit / ESP32-WROOM-32 board with GPIO16 available.
2. 128×64 I²C OLED using SSD1306 controller (the sketch uses I²C address `0x3C`).
3. USB data cable and computer.
4. Multimeter.
5. Oscilloscope or logic analyzer with an input range suitable for the measured signal.
6. A **receive-only, high-impedance input interface** between BM and GPIO16, selected after measuring the signal. If BM is 5 V, use a suitable divider or buffer that keeps the ESP32 input within its permitted range. If the line is open-drain/shared, negative-going, or otherwise nonstandard, design the interface from the observed waveform.
7. Jumper wires / connector breakout that does not damage the factory harness.

No transistor/button interface is required; this is telemetry-only. Do not connect the controller's VCC pin to ESP32 power.

## Pinout

| Signal | Connect to | Notes |
|---|---|---|
| Verified controller GND | ESP32 GND | Only after confirming the common reference is safe. |
| BM | Input-interface input | Measure before connecting. Never connect directly to GPIO16 while levels are unknown. |
| Input-interface output | ESP32 GPIO16 | Must be 3.3 V-compatible and must not drive BM. |
| OLED GND | ESP32 GND | Common ground. |
| OLED VCC | 3V3 (if supported by the module) | Check the specific OLED module's power requirements. |
| OLED SDA | GPIO21 | Configured in the sketch. |
| OLED SCL | GPIO22 | Configured in the sketch. |
| ESP32 GPIO17 / UART TX | **Not connected** | Firmware passes TX=-1; do not wire TX to BM. |
| USB | Computer | Serial Monitor at 115200 baud. |

OLED modules vary: verify supply voltage, I²C address and controller. The sketch currently probes address `0x3C`; common modules at `0x3D` need a code change.

### Optional divider — only after measurement

For a verified 0–5 V push-pull signal, 12 kΩ from BM to the RX node and 20 kΩ from that node to GND gives about 3.1 V from 5.0 V under the assumed conditions. This is an example, not an approved interface for an unknown bus. Check maximum and idle voltage, polarity, source impedance, and whether the line is shared/open-drain. Use a suitable buffer/interface where needed.

## Build and run

Arduino IDE libraries:
- Adafruit SSD1306
- Adafruit GFX Library

Then:

1. Open `wiretap_line_bm.ino`.
2. Select a classic ESP32 DevKit / ESP32-WROOM-32 board and its USB port.
3. Upload.
4. Open Serial Monitor at **115200 baud**.
5. After measuring the bus and installing the appropriate receive interface, connect BM to the interface input and the interface output to GPIO16.
6. Observe the OLED and save the serial log.

## Interpreting the result

- **Strong positive evidence:** repeated `RX valid` lines; both 24-byte CRCs pass; header/check bytes match; telemetry values are plausible and change consistently with actual heater operation or panel setting. This strongly supports compatibility with the reference protocol.
- **Inconclusive:** no valid exchanges. Possible causes include wrong electrical interface/polarity, baud/framing, wiring, missing common reference, dropped bytes, different controller traffic timing, or incompatible protocol. This alone does **not** disprove the hypothesis.
- **Evidence against direct compatibility:** stable, correctly captured waveforms and repeatable traffic at the expected baud, but repeated exchanges fail the reference framing/CRC checks; or valid frames are obtained but the mapped fields are consistently implausible. Preserve the raw waveform and bytes before concluding the protocol differs.

The OLED's `Waiting for valid data` screen means only that no accepted exchange has arrived. It is not itself a verdict.

## First experiment checklist

- [ ] Photograph the connector and note the viewing orientation.
- [ ] Confirm GND with power off and meter checks.
- [ ] Measure BM idle and peak voltage before attaching ESP32.
- [ ] Capture BM on a scope/logic analyzer and confirm bit timing/polarity.
- [ ] Verify the interface output stays within ESP32 input limits and does not drive/load BM.
- [ ] Record heater state and any panel actions alongside the capture.
- [ ] Save raw serial logs and scope/logic-analyzer captures unchanged.

## Safety and scope

This is RX-only. Do not add UART TX, send protocol commands, or emulate button presses in this test. ESP32 GPIOs are not 5 V tolerant. Stop if connecting the interface changes the controller's behavior or signal waveform.
