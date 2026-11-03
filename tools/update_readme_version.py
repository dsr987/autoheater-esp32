#!/usr/bin/env python3
"""Обновляет отображаемую версию README.md по маркеру в App.ino."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "App.ino"
README = ROOT / "README.md"


def main():
    source = APP.read_text(encoding="utf-8")
    match = re.search(
        r"^\s*\*\s*FIRMWARE_VERSION:\s*(\d+\.\d+\.\d+)\s*$", source, re.M
    )
    if not match:
        raise ValueError("Маркер FIRMWARE_VERSION не найден в App.ino")

    version = match.group(1)
    readme = README.read_text(encoding="utf-8")
    pattern = re.compile(
        r"^(\*\*Текущая версия прошивки:\s*)\d+\.\d+\.\d+(\*\*)$", re.M
    )
    updated, count = pattern.subn(
        lambda item: item.group(1) + version + item.group(2), readme, count=1
    )
    if count != 1:
        raise ValueError("Строка версии прошивки не найдена в README.md")

    README.write_text(updated, encoding="utf-8", newline="\n")
    print(f"README.md: версия прошивки {version}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
