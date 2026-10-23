# UART baud-rate diagnostic sweep

This branch adds a passive diagnostic sweep to the primary ESP32 wiretap firmware.

## Behaviour

- UART2 RX only on GPIO16; no TX pin is configured and no heater commands are sent.
- Cycles through 25000, 9600, 4800, and 2400 baud, 8N1, spending 12 seconds at each rate.
- Each switch flushes the current raw-byte chunk, drops the incomplete candidate, drains buffered bytes, and restarts UART at the next rate. A frame must not span baud-rate windows.
- Every `RAW` and `UART_GAP` log entry includes the active `baud=` value. `BAUD_SCAN` marks each window and reports cumulative bytes, accepted frames, and rejected candidates.
- The existing strict 48-byte/header/CRC decoder remains unchanged. A rate producing bytes is not necessarily a correct rate; the useful evidence is repeatable valid frames and plausible telemetry.

## How to test

1. Confirm the interface feeding GPIO16 is electrically safe for the ESP32 (3.3 V maximum at the GPIO; do not connect a 5 V line directly).
2. OTA-update with the build artifact from this PR, then reboot and connect to the Autoheater AP.
3. Leave the controller in a stable state for at least one full sweep (48 seconds). Prefer 2–3 sweeps; perform a repeatable single action during one sweep and note the time.
4. Download the log and group `RAW` lines by `baud=`. Check whether one setting yields a stable byte pattern and, most importantly, `VALID HEX:` and `DATA` lines.
5. If no rate yields valid frames, preserve the log and measure bit duration/polarity electrically before widening the search or changing the protocol parser.

## Why these rates

25000 baud is retained from the published compatible-heater reference implementation. 4800/9600 are exploratory candidates, and 2400 is included as a lower-rate diagnostic. The sweep is an experiment, not a claim that all four are supported by the target controller.

## Safety / interpretation

This firmware is receive-only. Do not connect ESP32 TX to BM. Do not treat the lack of valid frames as proof that the heater protocol is incompatible. A different electrical layer, polarity/inversion, UART framing, timing, or non-UART protocol may explain the result. This sweep does not automatically test inverted UART or parity settings.
