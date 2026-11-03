#!/usr/bin/env python3
"""Синхронизирует CHANGELOG.md с блоком изменений в App.ino."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "App.ino"
CHANGELOG = ROOT / "CHANGELOG.md"


def extract():
    text = APP.read_text(encoding="utf-8")
    version_match = re.search(
        r"^\s*\*\s*FIRMWARE_VERSION:\s*(\d+\.\d+\.\d+)\s*$", text, re.M
    )
    block_match = re.search(r"CHANGELOG_START(.*?)CHANGELOG_END", text, re.S)
    if not version_match or not block_match:
        raise ValueError("В App.ino отсутствует маркер версии или блок changelog")

    entries = []
    for line in block_match.group(1).splitlines():
        cleaned = re.sub(r"^\s*\*?\s?", "", line).strip()
        if cleaned.startswith("- "):
            entries.append(cleaned)

    if not entries:
        raise ValueError("Блок CHANGELOG_START / CHANGELOG_END пуст")
    return version_match.group(1), entries


def update(version, entries):
    old = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.exists() else "# История версий\n\n"
    heading = f"## {version}"
    section = heading + "\n" + "\n".join(entries) + "\n"
    pattern = re.compile(
        r"^##\s+" + re.escape(version) + r"\s*$.*?(?=^##\s+|\Z)",
        re.M | re.S,
    )
    match = pattern.search(old)

    if match:
        new = old[:match.start()] + section + "\n" + old[match.end():].lstrip("\n")
    else:
        top = re.match(r"^#.*?\n", old)
        if top:
            pos = top.end()
            new = old[:pos] + "\n" + section + "\n" + old[pos:].lstrip("\n")
        else:
            new = "# История версий\n\n" + section + "\n"

    CHANGELOG.write_text(new.rstrip() + "\n", encoding="utf-8", newline="\n")
    print(f"CHANGELOG.md: версия {version}, записей: {len(entries)}")


if __name__ == "__main__":
    try:
        version, entries = extract()
        update(version, entries)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
