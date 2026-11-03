#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_heater.py — декодер логов Logic 2 (CSV) шины автономного отопителя.

Формат кадра:  [SYNC][CMD][LEN16 BE][DATA][CRC8-MAXIM]
  SYNC: 0xA5 — пульт (мастер), 0xA0 — печка (слейв)
  CMD:  0x30 чтение регистра, 0x01 запись, 0x20 телеметрия пульта, 0x00 ответ слейва
  CRC-8/MAXIM: полином 0x8C (LSB-first), init 0x00, считается по всем байтам до CRC включительно.

Запуск:  python3 decode_heater.py файл.csv [файл2.csv ...]
"""

import sys, struct

CRC8_INIT = 0x00
def crc8_maxim(data):
    c = CRC8_INIT
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8C if c & 1 else c >> 1
    return c

def load_csv(path):
    # Logic 2: Time [s],Value,Parity Error,Framing Error
    samples = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("Time"):
                continue
            parts = line.split(",")
            try:
                t = float(parts[0]); v = int(parts[1], 16) & 0xFF
            except (ValueError, IndexError):
                continue
            samples.append((t, v))
    return samples

def split_frames(samples, gap=1.5e-3):
    """Нарезка по паузам: межбайтовая пауза внутри кадра ~1.03 мс, между кадрами ~10+ мс."""
    frames, cur = [], [samples[0]]
    for prev, s in zip(samples, samples[1:]):
        if s[0] - prev[0] > gap:
            frames.append(cur); cur = []
        cur.append(s)
    if cur: frames.append(cur)
    return frames

STATE_NAMES = {0x20:"свеча/прогрев", 0x24:"розжиг", 0x25:"пламя поймано",
               0x27:"фаза 7", 0x2E:"горение (уст.)", 0x2F:"выход на режим"}

def fmt_float(b):
    try:
        v = struct.unpack("<f", bytes(b))[0]
        return f"{v:.3f}"
    except Exception:
        return "?"

def u32(b):
    return int.from_bytes(bytes(b), "little")

def decode_frame(fr):
    raw = bytes(v for _, v in fr)
    t0 = fr[0][0]
    if len(raw) < 4 or raw[0] not in (0xA5, 0xA0):
        return None
    ln = (raw[2] << 8) | raw[3]
    total = 4 + ln + 1
    if len(raw) != total:
        return ("BADLEN", t0, raw.hex(" "))
    ok = crc8_maxim(raw[:-1]) == raw[-1]
    sync, cmd = raw[0], raw[1]
    data = raw[4:4+ln]
    who = "МАСТЕР" if sync == 0xA5 else "слейв "
    line = f"[{t0:9.3f}] {who} cmd=0x{cmd:02X} len={ln} data={data.hex(' ')}"
    if not ok:
        line += "  *** CRC FAIL ***"
        return ("CRC", line)
    if cmd == 0x30 and ln == 1:
        line += f"  READ reg=0x{data[0]:02X}"
    elif cmd == 0x00:
        # ответ слейва; содержимое интерпретируем в контексте таймлайна вручную
        pass
    elif cmd == 0x01 and data:
        reg = data[0]
        if ln == 5:
            line += f"  WRITE reg=0x{reg:02X} = {fmt_float(data[1:])}"
        elif ln == 2:
            line += f"  WRITE reg=0x{reg:02X} = 0x{data[1]:02X}"
    elif cmd == 0x20 and data:
        line += f"  ПУЛЬТ: idx=0x{data[0]:02X} val={fmt_float(data[1:])}" if ln == 5 else ""
    return ("OK", line)

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    for path in sys.argv[1:]:
        print(f"\n===== {path} =====")
        samples = load_csv(path)
        if not samples:
            print("  пустой файл"); continue
        frames = split_frames(samples)
        okc = badc = 0
        for fr in frames:
            r = decode_frame(fr)
            if r is None:  # мусор (обрывок до синхронизации)
                continue
            kind, line = r
            if kind == "OK": okc += 1
            else: badc += 1
            print(line)
        print(f"\n  Итого: кадров {len(frames)}, валидных {okc}, битых/обрывков {badc}, "
              f"байт {len(samples)}, время {samples[-1][0]-samples[0][0]:.1f} с")
    return 0

if __name__ == "__main__":
    sys.exit(main())
