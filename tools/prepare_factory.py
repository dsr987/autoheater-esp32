#!/usr/bin/env python3
"""Создаёт самостоятельный Arduino-скетч factory/ из актуальных исходников."""

from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
FACTORY = ROOT / "factory"


def main():
    FACTORY.mkdir(parents=True, exist_ok=True)

    shutil.copyfile(ROOT / "App.ino", FACTORY / "factory.ino")
    shutil.copyfile(ROOT / "partitions.csv", FACTORY / "partitions.csv")

    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "embed_web_page.py"),
            str(ROOT / "index.html"),
            str(FACTORY / "web_page.h"),
        ],
        check=True,
    )
    print(f"Factory sketch prepared: {FACTORY}")


if __name__ == "__main__":
    main()
