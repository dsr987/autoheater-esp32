#!/usr/bin/env python3
"""Генерирует C++ PROGMEM-заголовок из корневого index.html."""

from pathlib import Path
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: embed_web_page.py index.html output.h")

    source = Path(sys.argv[1])
    target = Path(sys.argv[2])
    html = source.read_text(encoding="utf-8")
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
