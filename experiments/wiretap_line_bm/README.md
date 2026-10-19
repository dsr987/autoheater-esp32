# BM line wiretap — first passive capture

**Status:** experimental hardware/software scaffold. This does not establish that the controller's `BM` pin uses the same protocol as the “blue wire” in other diesel-heater projects.

## What the firmware does

- Opens ESP32 UART2 as **receive-only** at a candidate 25,000 baud, 8N1.
- Reads bytes from GPIO16 and prints timestamped hexadecimal chunks to the USB serial console at 115200 baud.
- Splits console output after a 4 ms idle gap or 64 bytes. These are display chunks, **not decoded protocol packets**.
- Does not implement a heater protocol parser, send commands, drive a GPIO output, or use the OLED.
- Emits a 10-second counter so a quiet line can be distinguished from a reboot.

If no readable bytes appear, do not conclude that the line is inactive. The baud rate, framing, polarity, electrical interface, and even the BM/blue-wire equivalence are unverified. Capture the waveform with an oscilloscope or logic analyzer before changing decoder assumptions.

## Required components

1. Classic ESP32 DevKit / ESP32-WROOM-32 board (GPIO16 available).
2. USB cable for power/programming and the serial log.
3. Digital multimeter.
4. Oscilloscope or logic analyzer with a suitable input range; probe/adapter for the connector.
5. **A receive-only, high-impedance input interface** between BM and GPIO16, selected only after measuring the line:
   - common ground reference, after confirming it is safe to share;
   - if the line is a 5 V logic signal, a suitable divider or buffer that keeps the ESP32 input within its specified range;
   - if the line is open-drain, negative-going, noisy, or otherwise non-UART, design the interface from measured waveforms rather than assuming a resistor divider is sufficient.
6. Jumper wires / test leads that do not damage the factory harness.

Optional: 128×64 I²C OLED. It is not required for first captures; serial logs preserve more raw information and are easier to compare. This initial sketch does not drive an OLED.

## Connection / pinout

| Signal | Connect to | Notes |
|---|---|---|
| Controller connector `GND` (only after verified) | ESP32 GND | Establish common reference only after confirming ground with a meter. |
| Controller `BM` (only after measuring levels) | Input-interface signal input | **Do not connect directly** to an ESP32 GPIO while voltage/interface type is unknown. |
| Input-interface output | ESP32 GPIO16 (UART2 RX) | Output must be compatible with 3.3 V ESP32 logic and must not drive BM. |
| ESP32 USB | Computer | Serial console at 115200 baud. |
| ESP32 GPIO17 / UART2 TX | **Not connected** | Firmware sets TX pin to -1; do not wire TX to BM. |
| OLED (optional) | Not connected for this test | If added later: SDA/SCL depend on the specific ESP32 board and display module. |

Connector labels such as `GND`, `BM`, and `VCC` are provisional observations, not proof of electrical function. Verify the exact pads and connector orientation before wiring. Do not connect the connector's VCC to ESP32 3V3 or 5V.

### Example only: resistive divider for a verified 5 V signal

A 12 kΩ series/top resistor from BM to the RX node and a 20 kΩ resistor from RX node to GND would produce about 3.1 V from 5.0 V **under the assumed conditions**. This is not a universal interface design: first check idle voltage, maximum voltage, polarity, source impedance, and whether the line is shared/open-drain. Use a buffer or a purpose-built interface if those conditions are not established. Never let the RX node exceed the ESP32's permitted input voltage.

## Build and run

This is a standalone Arduino sketch (no third-party libraries required):

1. Open `wiretap_line_bm.ino` in Arduino IDE.
2. Select an ESP32 board package and the correct board/port.
3. Upload over USB.
4. Open Serial Monitor at **115200 baud**.
5. With the receive interface connected safely, observe the controller's normal activity and save the complete console log as a text file.

Example output shape:

```text
# Autoheater BM passive wiretap
# MODE=RX_ONLY; TX is not configured
# RX_GPIO=16 candidate_baud=25000 format=8N1 frame_gap_us=4000
t_us=123456 chunk=0 len=4 data=76 01 02 FF
# stats uptime_ms=10000 bytes=4 chunks=1
```

The example bytes above are illustrative only, not a real capture or a known packet.

## First experiment checklist

- [ ] Photograph the connector and record which side is viewed.
- [ ] Identify GND with power off and confirm with voltage measurements.
- [ ] Measure BM relative to GND with the controller disconnected/reconnected as appropriate; record off/on idle and peak levels.
- [ ] Observe BM on a scope/logic analyzer before attaching ESP32.
- [ ] Verify the interface output never exceeds ESP32 limits and does not load or drive BM.
- [ ] Record heater/panel state and user action alongside each capture.
- [ ] Save untouched raw logs; do not edit out bytes or infer packet fields yet.

## Limitations / next step

UART decoding at 25,000 baud is only a first test. If bytes are garbled or absent, use the scope/logic-analyzer trace to determine signal polarity, bit period, framing, and whether this is UART at all. Only then change baud/framing or add a pulse-timing capture path. Do not add transmission until the electrical interface and protocol are understood and explicitly reviewed.
