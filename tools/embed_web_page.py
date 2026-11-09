#!/usr/bin/env python3
"""Генерирует C++ PROGMEM-заголовок из корневого index.html."""

from pathlib import Path
import re
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: embed_web_page.py index.html output.h")

    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    html = source.read_text(encoding="utf-8")
    app_source = source.resolve().parent / "App.ino"
    app_text = app_source.read_text(encoding="utf-8")
    version_match = re.search(
        r"FIRMWARE_VERSION:\s*([0-9]+\.[0-9]+\.[0-9]+)", app_text
    )
    if not version_match:
        raise ValueError(f"FIRMWARE_VERSION marker not found in {app_source}")
    html = html.replace("__FIRMWARE_VERSION__", version_match.group(1))
    if "__FIRMWARE_VERSION__" in html:
        raise ValueError("Firmware version placeholder was not replaced")
    delimiter = "AUTOHEATER_HTML"

    if f"){delimiter}\"" in html:
        raise ValueError(f"HTML содержит запрещённую последовательность ){delimiter}\"")

    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        "#pragma once\n"
        "#include <Arduino.h>\n"
        f"static const char PAGE_HTML[] PROGMEM = R\"{delimiter}({html}){delimiter}\";\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"Embedded {source} -> {target}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
